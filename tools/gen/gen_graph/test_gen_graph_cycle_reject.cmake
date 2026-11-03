# GEN-11 negative case: a directed cycle with no feedback marker must be
# rejected with exit 2, the error must name the cycle members, and gen_graph
# must not leave any partial output behind.
if(NOT GEN OR NOT THYME)
  message(FATAL_ERROR "GEN and THYME required")
endif()
get_filename_component(THYME_ABS "${THYME}" ABSOLUTE)
set(ROOT "${CMAKE_BINARY_DIR}/cycle_reject_check")
file(REMOVE_RECURSE "${ROOT}")

execute_process(COMMAND "${GEN}"
    --in "${THYME_ABS}/tools/gen/gen_graph/testdata/cycle/cycle.yaml"
    --out "${ROOT}/app"
  RESULT_VARIABLE rc OUTPUT_VARIABLE cout ERROR_VARIABLE cerr)
if(rc EQUAL 0)
  message(FATAL_ERROR "cycle_reject: gen_graph accepted a cyclic graph")
endif()
if(NOT rc EQUAL 2)
  message(FATAL_ERROR "cycle_reject: exit code ${rc}, expected 2:\n${cout}${cerr}")
endif()

foreach(member node_a node_b node_c)
  string(FIND "${cerr}" "${member}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR
      "cycle_reject: error message does not name cycle member ${member}:\n${cerr}")
  endif()
endforeach()
string(FIND "${cerr}" "cycle" pos)
if(pos EQUAL -1)
  message(FATAL_ERROR "cycle_reject: error message does not mention a cycle:\n${cerr}")
endif()

if(EXISTS "${ROOT}/app")
  message(FATAL_ERROR
    "cycle_reject: gen_graph left partial output behind at ${ROOT}/app")
endif()
