# GEN-05 --check: regenerate into a staging dir and diff against the
# checked-in generated/ tree. Fresh generation -> --check passes (also proves
# generation is byte-stable across runs); a hand edit, a deleted file, and an
# extra file each fail the check naming the file. --check never writes into
# generated/ itself.
if(NOT GEN OR NOT THYME OR NOT TPL2_SRC)
  message(FATAL_ERROR "GEN, THYME and TPL2_SRC required")
endif()
get_filename_component(THYME_ABS "${THYME}" ABSOLUTE)
set(TPL "${CMAKE_BINARY_DIR}/check_tpl")
set(ROOT "${CMAKE_BINARY_DIR}/check_app")
file(REMOVE_RECURSE "${TPL}" "${ROOT}")
file(COPY "${TPL2_SRC}/" DESTINATION "${TPL}")

function(run_check expect_rc out_var)
  execute_process(COMMAND "${GEN}" --check --out "${ROOT}"
      --template-dir "${TPL}" --in "${TPL}/graph.yaml"
    RESULT_VARIABLE rc OUTPUT_VARIABLE cout ERROR_VARIABLE cerr)
  if(NOT rc EQUAL ${expect_rc})
    message(FATAL_ERROR
      "gen_graph --check exited ${rc}, expected ${expect_rc}:\n${cout}${cerr}")
  endif()
  set(${out_var} "${cout}${cerr}" PARENT_SCOPE)
endfunction()

# Fresh generation -> check passes.
execute_process(COMMAND "${GEN}" --out "${ROOT}" --update generated
    --template-dir "${TPL}" --in "${TPL}/graph.yaml" RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "gen_graph (check fixture) failed: ${rc}")
endif()
run_check(0 out)
string(FIND "${out}" "up to date" pos)
if(pos EQUAL -1)
  message(FATAL_ERROR "--check on a fresh tree did not report up to date:\n${out}")
endif()

# Hand edit one generated file -> check fails naming it.
file(APPEND "${ROOT}/generated/wiring.cpp" "// hand edit\n")
run_check(1 out)
string(FIND "${out}" "wiring.cpp" pos)
if(pos EQUAL -1)
  message(FATAL_ERROR "--check did not name the hand-edited file:\n${out}")
endif()
string(FIND "${out}" "content differs" pos)
if(pos EQUAL -1)
  message(FATAL_ERROR "--check did not report a content diff:\n${out}")
endif()

# The check itself must not modify the tree: wiring.cpp still carries the edit.
file(READ "${ROOT}/generated/wiring.cpp" wiring_body)
string(FIND "${wiring_body}" "// hand edit" pos)
if(pos EQUAL -1)
  message(FATAL_ERROR "--check wrote into generated/ (hand edit lost)")
endif()

# Regenerate -> clean again; then delete a file -> check fails naming it.
execute_process(COMMAND "${GEN}" --out "${ROOT}" --update generated
    --template-dir "${TPL}" --in "${TPL}/graph.yaml" RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "gen_graph (check fixture regen) failed: ${rc}")
endif()
run_check(0 out)
file(REMOVE "${ROOT}/generated/shm_slots.h")
run_check(1 out)
string(FIND "${out}" "shm_slots.h: missing" pos)
if(pos EQUAL -1)
  message(FATAL_ERROR "--check did not name the deleted file:\n${out}")
endif()

# Regenerate -> extra (stale) file -> check fails naming it.
execute_process(COMMAND "${GEN}" --out "${ROOT}" --update generated
    --template-dir "${TPL}" --in "${TPL}/graph.yaml" RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "gen_graph (check fixture regen 2) failed: ${rc}")
endif()
file(WRITE "${ROOT}/generated/hand_made.h" "#pragma once\n")
run_check(1 out)
string(FIND "${out}" "hand_made.h: stale" pos)
if(pos EQUAL -1)
  message(FATAL_ERROR "--check did not name the extra file:\n${out}")
endif()

# --check only applies to the generated tree.
execute_process(COMMAND "${GEN}" --check --update all --out "${ROOT}"
    --template-dir "${TPL}" --in "${TPL}/graph.yaml"
  RESULT_VARIABLE rc ERROR_VARIABLE cerr)
if(NOT rc EQUAL 2)
  message(FATAL_ERROR "--check --update all exited ${rc}, expected 2")
endif()

# No staging dirs left behind (GEN-03: pid-unique .generated.<tag>.<pid> dirs).
file(GLOB leftovers "${ROOT}/.generated.*" "${ROOT}/.gen_graph_tmp*"
     "${ROOT}/.gen_graph_tpl")
if(leftovers)
  message(FATAL_ERROR "--check left staging dirs behind: ${leftovers}")
endif()

message(STATUS "gen_graph --check fixture OK")
