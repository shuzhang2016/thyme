# GEN-11 escape hatch: a loop closed by an edge marked `feedback: true` with
# an explicit `delay` generates successfully and the rate analysis marks the
# loop unresolved; `feedback: true` without a delay is rejected with exit 2.
if(NOT GEN OR NOT THYME)
  message(FATAL_ERROR "GEN and THYME required")
endif()
get_filename_component(THYME_ABS "${THYME}" ABSOLUTE)
set(DATA "${THYME_ABS}/tools/gen/gen_graph/testdata/cycle")
set(ROOT "${CMAKE_BINARY_DIR}/feedback_marker_check")
file(REMOVE_RECURSE "${ROOT}")

# --- feedback + delay: generates ---
execute_process(COMMAND "${GEN}"
    --in "${DATA}/feedback.yaml" --out "${ROOT}/app" --update generated
    --template-dir "${DATA}/tpl" --no-health-reporter
  RESULT_VARIABLE rc OUTPUT_VARIABLE cout ERROR_VARIABLE cerr)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "feedback_marker: gen_graph (feedback+delay) failed: ${rc}\n${cout}${cerr}")
endif()
if(NOT EXISTS "${ROOT}/app/generated/wiring.cpp" OR
   NOT EXISTS "${ROOT}/app/generated/graph_ir.h")
  message(FATAL_ERROR "feedback_marker: expected generated files missing")
endif()
# The loop cut is reported on stderr during codegen.
string(FIND "${cerr}" "loop unresolved" pos)
if(pos EQUAL -1)
  message(FATAL_ERROR
    "feedback_marker: codegen stderr missing loop-unresolved note:\n${cerr}")
endif()

# --- explain-rates: truncates the loop and annotates it ---
execute_process(COMMAND "${GEN}"
    --in "${DATA}/feedback.yaml" --explain-rates
  RESULT_VARIABLE rc OUTPUT_VARIABLE cout ERROR_VARIABLE cerr)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "feedback_marker: --explain-rates failed: ${rc}\n${cout}${cerr}")
endif()
string(FIND "${cout}" "\"loop_unresolved\"" pos)
if(pos EQUAL -1)
  message(FATAL_ERROR
    "feedback_marker: explain JSON missing loop_unresolved:\n${cout}")
endif()
string(FIND "${cout}" "loop unresolved at feedback edge node_c->node_a:ch_ca" pos)
if(pos EQUAL -1)
  message(FATAL_ERROR
    "feedback_marker: explain JSON does not annotate the cut edge:\n${cout}")
endif()

# --- feedback without delay: rejected, message explains both are required ---
execute_process(COMMAND "${GEN}"
    --in "${DATA}/feedback_no_delay.yaml" --out "${ROOT}/bad"
  RESULT_VARIABLE rc OUTPUT_VARIABLE cout ERROR_VARIABLE cerr)
if(NOT rc EQUAL 2)
  message(FATAL_ERROR
    "feedback_marker: feedback without delay gave exit ${rc}, expected 2:\n${cout}${cerr}")
endif()
foreach(needle "feedback" "delay")
  string(FIND "${cerr}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR
      "feedback_marker: rejection message missing '${needle}':\n${cerr}")
  endif()
endforeach()
if(EXISTS "${ROOT}/bad")
  message(FATAL_ERROR
    "feedback_marker: gen_graph left partial output behind at ${ROOT}/bad")
endif()
