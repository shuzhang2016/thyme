# OPS-06: tools/ops/upgrade/thyme_upgrade.sh must default to a dry run. This test
# proves that the dry run touches nothing: no unit control call, no install
# copy, no backup, no SHM unlink, no package deletion. It then runs --apply
# against temp directories with a fake unit manager to prove the sequence and
# the "no deletion without an explicit flag" rule.
#
# Invoked by ctest with -DSCRIPT=<thyme_upgrade.sh> -DWORK=<temp dir>.

if(NOT DEFINED SCRIPT OR NOT DEFINED WORK)
  message(FATAL_ERROR "usage: cmake -DSCRIPT=<script> -DWORK=<dir> -P this")
endif()

file(REMOVE_RECURSE "${WORK}")
file(MAKE_DIRECTORY "${WORK}")

function(write_exec path content)
  file(WRITE "${path}" "${content}")
  execute_process(COMMAND chmod +x "${path}" RESULT_VARIABLE rc)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "chmod failed for ${path}")
  endif()
endfunction()

# Fake probe: one old page (v2 vs this v3 build). Honours the modes the script
# uses and stays read-only, like the real thing.
write_exec("${WORK}/fake_probe"
"#!/bin/sh
# args: abi [--mismatches|--matrix] [--prefix P]...
case \" $* \" in
  *\" --matrix \"*) echo '{\"schema\":1,\"entries\":[]}'; exit 0 ;;
esac
case \" $* \" in
  *\" --mismatches \"*)
    echo '/thyme_fake_old expected=3 actual=2'
    exit 3 ;;
esac
echo '{\"schema\":1,\"ok\":false}'
exit 3
")

# Fake probe where the page is NEWER than the build: a downgrade must be
# refused unless --allow-downgrade.
write_exec("${WORK}/fake_probe_newer"
"#!/bin/sh
case \" $* \" in
  *\" --matrix \"*) echo '{\"schema\":1,\"entries\":[]}'; exit 0 ;;
esac
case \" $* \" in
  *\" --mismatches \"*)
    echo '/thyme_fake_new expected=2 actual=9'
    exit 3 ;;
esac
exit 3
")

# Fake probe on a clean host (no pages).
write_exec("${WORK}/fake_probe_clean"
"#!/bin/sh
case \" $* \" in
  *\" --matrix \"*) echo '{\"schema\":1,\"entries\":[]}'; exit 0 ;;
esac
exit 2
")

write_exec("${WORK}/fake_ctl"
"#!/bin/sh
echo \"\$@\" >> \"${WORK}/ctl.log\"
exit 0
")

function(read_file path out)
  file(READ "${path}" _content)
  set(${out} "${_content}" PARENT_SCOPE)
endfunction()

function(expect_file path needle what)
  if(NOT EXISTS "${path}")
    message(FATAL_ERROR "${what}: missing file ${path}")
  endif()
  read_file("${path}" content)
  string(FIND "${content}" "${needle}" pos)
  if(pos LESS 0)
    message(FATAL_ERROR "${what}: ${path} does not contain '${needle}'\n${content}")
  endif()
endfunction()

# ---------------------------------------------------------------- scenario 1
# Dry run with every destructive flag set: nothing may happen.
file(MAKE_DIRECTORY "${WORK}/stage1/bin" "${WORK}/prefix1/bin")
file(WRITE "${WORK}/stage1/bin/role" "new\n")
file(WRITE "${WORK}/prefix1/bin/role" "old\n")

execute_process(
  COMMAND "${SCRIPT}"
    --probe "${WORK}/fake_probe"
    --unit-ctl "${WORK}/fake_ctl"
    --package "${WORK}/stage1"
    --prefix "${WORK}/prefix1"
    --backup-dir "${WORK}/backup1"
    --purge-shm
    --delete-package "${WORK}/stage1"
    --shm-prefix thyme_fake_
    --verify-wait-ms 0
  OUTPUT_VARIABLE dry_out
  ERROR_VARIABLE dry_err
  RESULT_VARIABLE dry_rc)
if(NOT dry_rc EQUAL 0)
  message(FATAL_ERROR "dry run failed (rc=${dry_rc})\n${dry_out}\n${dry_err}")
endif()
string(FIND "${dry_out}" "DRY RUN" dry_marker)
if(dry_marker LESS 0)
  message(FATAL_ERROR "dry run did not announce itself:\n${dry_out}")
endif()
string(FIND "${dry_out}" "(dry-run) would run:" plan_marker)
if(plan_marker LESS 0)
  message(FATAL_ERROR "dry run did not print the plan:\n${dry_out}")
endif()
if(EXISTS "${WORK}/ctl.log")
  read_file("${WORK}/ctl.log" ctl)
  message(FATAL_ERROR "dry run called the unit manager:\n${ctl}")
endif()
if(EXISTS "${WORK}/backup1")
  message(FATAL_ERROR "dry run created the backup directory")
endif()
if(NOT EXISTS "${WORK}/stage1/bin/role")
  message(FATAL_ERROR "dry run deleted the package")
endif()
expect_file("${WORK}/stage1/bin/role" "new" "dry run modified the package")
expect_file("${WORK}/prefix1/bin/role" "old" "dry run modified the install prefix")

# ---------------------------------------------------------------- scenario 2
# A newer page than this build must be refused even in check-only mode.
execute_process(
  COMMAND "${SCRIPT}" --check-only --probe "${WORK}/fake_probe_newer"
  OUTPUT_VARIABLE newer_out
  ERROR_VARIABLE newer_err
  RESULT_VARIABLE newer_rc)
if(NOT newer_rc EQUAL 2)
  message(FATAL_ERROR
    "downgrade was not refused (rc=${newer_rc})\n${newer_out}\n${newer_err}")
endif()
string(FIND "${newer_out}${newer_err}" "downgrade" downgrade_word)
if(downgrade_word LESS 0)
  message(FATAL_ERROR "downgrade refusal gave no reason:\n${newer_out}${newer_err}")
endif()

# --allow-downgrade turns the refusal into a warning.
execute_process(
  COMMAND "${SCRIPT}" --check-only --allow-downgrade
    --probe "${WORK}/fake_probe_newer"
  OUTPUT_VARIABLE allow_out
  ERROR_VARIABLE allow_err
  RESULT_VARIABLE allow_rc)
if(NOT allow_rc EQUAL 0)
  message(FATAL_ERROR "--allow-downgrade still failed (rc=${allow_rc})\n${allow_out}${allow_err}")
endif()

# Clean host / no pages is a pass.
execute_process(
  COMMAND "${SCRIPT}" --check-only --probe "${WORK}/fake_probe_clean"
  RESULT_VARIABLE clean_rc)
if(NOT clean_rc EQUAL 0)
  message(FATAL_ERROR "clean check-only failed (rc=${clean_rc})")
endif()

# ---------------------------------------------------------------- scenario 3
# --apply: stop -> install -> start, backup captured, package kept.
file(MAKE_DIRECTORY "${WORK}/stage2/bin" "${WORK}/prefix2/bin")
file(WRITE "${WORK}/stage2/bin/role" "new\n")
file(WRITE "${WORK}/prefix2/bin/role" "old\n")
file(REMOVE "${WORK}/ctl.log")

execute_process(
  COMMAND "${SCRIPT}" --apply
    --probe "${WORK}/fake_probe"
    --unit-ctl "${WORK}/fake_ctl"
    --package "${WORK}/stage2"
    --prefix "${WORK}/prefix2"
    --backup-dir "${WORK}/backup2"
    --unit thyme-test
    --shm-prefix thyme_fake_
    --verify-wait-ms 0
  OUTPUT_VARIABLE apply_out
  ERROR_VARIABLE apply_err
  RESULT_VARIABLE apply_rc)
if(NOT apply_rc EQUAL 0)
  message(FATAL_ERROR "apply failed (rc=${apply_rc})\n${apply_out}\n${apply_err}")
endif()
expect_file("${WORK}/prefix2/bin/role" "new" "apply did not install the package")
expect_file("${WORK}/backup2/bin/role" "old" "apply did not back up the old tree")
expect_file("${WORK}/ctl.log" "stop thyme-test" "apply did not stop the unit")
expect_file("${WORK}/ctl.log" "start thyme-test" "apply did not start the unit")
if(NOT EXISTS "${WORK}/stage2/bin/role")
  message(FATAL_ERROR "apply deleted the package without --delete-package")
endif()

# The stop must be recorded before the start.
read_file("${WORK}/ctl.log" ctl2)
string(FIND "${ctl2}" "stop thyme-test" stop_pos)
string(FIND "${ctl2}" "start thyme-test" start_pos)
if(stop_pos GREATER start_pos)
  message(FATAL_ERROR "unit was started before it was stopped:\n${ctl2}")
endif()

# ---------------------------------------------------------------- scenario 4
# Only the explicit --delete-package removes a package tree, and only --apply
# does it.
file(MAKE_DIRECTORY "${WORK}/stage3/bin" "${WORK}/prefix3/bin")
file(WRITE "${WORK}/stage3/bin/role" "new\n")
execute_process(
  COMMAND "${SCRIPT}" --apply
    --probe "${WORK}/fake_probe_clean"
    --unit-ctl "${WORK}/fake_ctl"
    --package "${WORK}/stage3"
    --prefix "${WORK}/prefix3"
    --delete-package "${WORK}/stage3"
    --unit thyme-test
    --shm-prefix thyme_fake_
    --verify-wait-ms 0
  OUTPUT_VARIABLE del_out
  ERROR_VARIABLE del_err
  RESULT_VARIABLE del_rc)
if(NOT del_rc EQUAL 0)
  message(FATAL_ERROR "delete run failed (rc=${del_rc})\n${del_out}\n${del_err}")
endif()
if(EXISTS "${WORK}/stage3")
  message(FATAL_ERROR "--delete-package did not remove the package")
endif()
expect_file("${WORK}/prefix3/bin/role" "new" "delete run did not install")

# ---------------------------------------------------------------- scenario 5
# Rollback: newer pages than the restored build are expected (the whole point
# of a rollback), so the direction check must not refuse them. Dry run first:
# the prefix must stay untouched, then --apply restores the backup.
file(MAKE_DIRECTORY "${WORK}/backup3/bin" "${WORK}/prefix4/bin")
file(WRITE "${WORK}/backup3/bin/role" "old\n")
file(WRITE "${WORK}/prefix4/bin/role" "new\n")

execute_process(
  COMMAND "${SCRIPT}"
    --probe "${WORK}/fake_probe_newer"
    --unit-ctl "${WORK}/fake_ctl"
    --rollback "${WORK}/backup3"
    --prefix "${WORK}/prefix4"
    --unit thyme-test
    --shm-prefix thyme_fake_
    --verify-wait-ms 0
  OUTPUT_VARIABLE rollback_dry_out
  ERROR_VARIABLE rollback_dry_err
  RESULT_VARIABLE rollback_dry_rc)
if(NOT rollback_dry_rc EQUAL 0)
  message(FATAL_ERROR
    "rollback dry run failed (rc=${rollback_dry_rc})\n"
    "${rollback_dry_out}\n${rollback_dry_err}")
endif()
expect_file("${WORK}/prefix4/bin/role" "new" "rollback dry run modified the prefix")

execute_process(
  COMMAND "${SCRIPT}" --apply
    --probe "${WORK}/fake_probe_newer"
    --unit-ctl "${WORK}/fake_ctl"
    --rollback "${WORK}/backup3"
    --prefix "${WORK}/prefix4"
    --unit thyme-test
    --shm-prefix thyme_fake_
    --verify-wait-ms 0
  OUTPUT_VARIABLE rollback_out
  ERROR_VARIABLE rollback_err
  RESULT_VARIABLE rollback_rc)
if(NOT rollback_rc EQUAL 0)
  message(FATAL_ERROR
    "rollback apply failed (rc=${rollback_rc})\n${rollback_out}\n${rollback_err}")
endif()
expect_file("${WORK}/prefix4/bin/role" "old" "rollback did not restore the backup")
string(FIND "${rollback_out}" "newer than the restored build" rollback_note)
if(rollback_note LESS 0)
  message(FATAL_ERROR "rollback did not explain the newer-page direction:\n${rollback_out}")
endif()

message(STATUS "test_upgrade_script: all checks passed")
