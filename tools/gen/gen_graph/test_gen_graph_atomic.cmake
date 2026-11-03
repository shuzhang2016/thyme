# GEN-03: EmitGenerated rewrites generated/ atomically — the fresh tree is
# staged in a pid-unique sibling dir and swapped in with two renames, so
#   * stale files from previous runs are deleted,
#   * a crash/failure mid-generation never leaves a half-written generated/
#     (the old tree stays in place until the swap),
#   * user-owned scaffold files (src/nodes/...) are never touched,
#   * no staging/trash dirs remain after success OR failure.
if(NOT GEN OR NOT THYME OR NOT TPL2_SRC)
  message(FATAL_ERROR "GEN, THYME and TPL2_SRC required")
endif()
get_filename_component(THYME_ABS "${THYME}" ABSOLUTE)
set(TPL "${CMAKE_BINARY_DIR}/atomic_tpl")
set(ROOT "${CMAKE_BINARY_DIR}/atomic_app")
set(REF "${CMAKE_BINARY_DIR}/atomic_ref")
file(REMOVE_RECURSE "${TPL}" "${ROOT}" "${REF}")
file(COPY "${TPL2_SRC}/" DESTINATION "${TPL}")

function(expect_no_stale_dirs dir)
  file(GLOB leftovers "${dir}/.generated.*" "${dir}/.gen_graph_tmp*"
       "${dir}/.gen_graph_tpl")
  if(leftovers)
    message(FATAL_ERROR "staging/trash dirs left behind in ${dir}: ${leftovers}")
  endif()
endfunction()

# 1) Bootstrap a fresh app (--update all also scaffolds src/, include/, ...).
#    Covers the "generated/ does not exist yet" path.
execute_process(COMMAND "${GEN}" --out "${ROOT}" --update all
  --template-dir "${TPL}" --in "${TPL}/graph.yaml" RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "gen_graph --update all failed: ${rc}")
endif()
if(NOT EXISTS "${ROOT}/generated/wiring.cpp")
  message(FATAL_ERROR "fresh run did not produce generated/wiring.cpp")
endif()

# Reference tree: pristine --update generated into an empty root.
execute_process(COMMAND "${GEN}" --out "${REF}" --update generated
  --template-dir "${TPL}" --in "${TPL}/graph.yaml" RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "gen_graph (reference) failed: ${rc}")
endif()

# 2) Plant a stale file inside generated/ and a user file in src/nodes/, then
#    re-run. The stale file must be gone, the user file preserved, and the
#    tree byte-identical to the fresh reference.
file(WRITE "${ROOT}/generated/stale_leftover.h" "// stale from an older graph\n")
file(MAKE_DIRECTORY "${ROOT}/generated/old_subdir")
file(WRITE "${ROOT}/generated/old_subdir/nested_stale.h" "// stale\n")
file(WRITE "${ROOT}/src/nodes/my_notes.txt" "user-owned, never generated\n")
execute_process(COMMAND "${GEN}" --out "${ROOT}" --update all
  --template-dir "${TPL}" --in "${TPL}/graph.yaml" RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "gen_graph re-run failed: ${rc}")
endif()
if(EXISTS "${ROOT}/generated/stale_leftover.h")
  message(FATAL_ERROR "stale file survived the atomic rewrite")
endif()
if(EXISTS "${ROOT}/generated/old_subdir")
  message(FATAL_ERROR "stale subdirectory survived the atomic rewrite")
endif()
if(NOT EXISTS "${ROOT}/src/nodes/my_notes.txt")
  message(FATAL_ERROR "--update all wiped a user-owned file in src/nodes/")
endif()
execute_process(COMMAND diff -r "${REF}/generated" "${ROOT}/generated"
  RESULT_VARIABLE rc OUTPUT_VARIABLE dout ERROR_VARIABLE derr)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "re-run tree differs from the fresh reference:\n${dout}${derr}")
endif()
expect_no_stale_dirs("${ROOT}")

# 3) Failure mid-fill: a noworker catalog type without its <type>_generated.h.inc
#    fails inside the staging dir, after several files were already written.
#    The pre-existing generated/ tree must stay byte-intact and no staging
#    dirs may remain.
set(TPL_BAD "${CMAKE_BINARY_DIR}/atomic_tpl_bad")
file(REMOVE_RECURSE "${TPL_BAD}")
file(COPY "${TPL2_SRC}/" DESTINATION "${TPL_BAD}")
file(APPEND "${TPL_BAD}/known_types.txt" "broken|BrokenThread|broken||noworker\n")
file(READ "${TPL_BAD}/graph.yaml" bad_yaml)
# Insert the broken node into the nodes list, keeping health_reporter last
# (the emitted graph_check.h static_asserts on that order).
string(REPLACE "  - id: health_reporter"
  "  - id: broken\n    type: broken\n    hz: 10\n    process: main\n  - id: health_reporter"
  bad_yaml "${bad_yaml}")
file(WRITE "${TPL_BAD}/graph_bad_fill.yaml" "${bad_yaml}")

file(WRITE "${ROOT}/generated/keep_sentinel.h" "// planted before a failing run\n")
execute_process(COMMAND "${GEN}" --out "${ROOT}" --update generated
  --template-dir "${TPL_BAD}" --in "${TPL_BAD}/graph_bad_fill.yaml"
  RESULT_VARIABLE rc OUTPUT_VARIABLE cout ERROR_VARIABLE cerr)
if(NOT rc EQUAL 2)
  message(FATAL_ERROR "gen_graph (bad noworker) exited ${rc}, expected 2:\n${cout}${cerr}")
endif()
string(FIND "${cerr}" "broken_generated.h.inc" pos)
if(pos EQUAL -1)
  message(FATAL_ERROR "failure did not name the missing inc:\n${cerr}")
endif()
string(FIND "${cerr}" "${TPL_BAD}/graph_bad_fill.yaml:13:" pos)
if(pos EQUAL -1)
  message(FATAL_ERROR "failure did not name the broken node line:\n${cerr}")
endif()
if(NOT EXISTS "${ROOT}/generated/keep_sentinel.h")
  message(FATAL_ERROR "failed run touched the old generated/ tree (sentinel lost)")
endif()
if(EXISTS "${ROOT}/generated/broken_generated.h")
  message(FATAL_ERROR "failed run installed partial output into generated/")
endif()
expect_no_stale_dirs("${ROOT}")

# 3b) Oversized placement is an emit failure with the node location, not a
# silently discarded AppendPlacement result. The old tree remains intact.
file(READ "${TPL}/graph.yaml" placement_yaml)
set(cpus "[0")
foreach(i RANGE 1 64)
  string(APPEND cpus ",0")
endforeach()
string(APPEND cpus "]")
string(REPLACE "    process: main\n  - id: health_reporter"
  "    process: main\n    cpu_set: ${cpus}\n  - id: health_reporter"
  placement_yaml "${placement_yaml}")
file(WRITE "${TPL}/graph_bad_placement.yaml" "${placement_yaml}")
execute_process(COMMAND "${GEN}" --out "${ROOT}" --update generated
  --template-dir "${TPL}" --in "${TPL}/graph_bad_placement.yaml"
  RESULT_VARIABLE rc OUTPUT_VARIABLE cout ERROR_VARIABLE cerr)
if(NOT rc EQUAL 2)
  message(FATAL_ERROR "gen_graph (bad placement) exited ${rc}, expected 2:\n${cout}${cerr}")
endif()
string(FIND "${cerr}" "cpu_set exceeds ThreadPlacement::kMaxCpus" pos)
if(pos EQUAL -1)
  message(FATAL_ERROR "placement failure lost its diagnostic:\n${cerr}")
endif()
string(FIND "${cerr}" "${TPL}/graph_bad_placement.yaml:9:" pos)
if(pos EQUAL -1)
  message(FATAL_ERROR "placement failure did not name the consumer node line:\n${cerr}")
endif()
if(NOT EXISTS "${ROOT}/generated/keep_sentinel.h")
  message(FATAL_ERROR "placement failure touched the old generated/ tree")
endif()
expect_no_stale_dirs("${ROOT}")

# 4) The old tree is still fully usable: --check against the failing template
#    would drift, but against the original template the tree (minus the planted
#    sentinel) matches. Remove the sentinel and check.
file(REMOVE "${ROOT}/generated/keep_sentinel.h")
execute_process(COMMAND "${GEN}" --check --out "${ROOT}"
  --template-dir "${TPL}" --in "${TPL}/graph.yaml"
  RESULT_VARIABLE rc OUTPUT_VARIABLE cout ERROR_VARIABLE cerr)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "--check after the failed run did not pass:\n${cout}${cerr}")
endif()
expect_no_stale_dirs("${ROOT}")

message(STATUS "gen_graph atomic rewrite fixture OK")
