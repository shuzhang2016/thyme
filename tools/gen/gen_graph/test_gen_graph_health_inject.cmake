# Default health_reporter injection: a graph that does not declare the
# aggregator gets a synthetic one (last node, health_n bumped) under the
# default codegen path, and omits it under --no-health-reporter. Uses the
# minimal_norecord overlay (which has no known_types.txt of its own), so the
# injected node resolves via the built-in catalog row + base scaffold
# health_reporter_generated.h.inc.
if(NOT GEN OR NOT THYME)
  message(FATAL_ERROR "GEN and THYME required")
endif()
get_filename_component(THYME_ABS "${THYME}" ABSOLUTE)
set(TPL "${THYME_ABS}/tools/gen/gen_graph/testdata/minimal_norecord")
set(ROOT_ON "${CMAKE_BINARY_DIR}/health_inject_on")
set(ROOT_OFF "${CMAKE_BINARY_DIR}/health_inject_off")
file(REMOVE_RECURSE "${ROOT_ON}" "${ROOT_OFF}")

execute_process(COMMAND "${GEN}" --out "${ROOT_ON}" --update generated
  --template-dir "${TPL}" --in "${TPL}/graph.yaml" RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "gen_graph (health inject on) failed: ${rc}")
endif()

if(NOT EXISTS "${ROOT_ON}/generated/health_reporter_generated.h")
  message(FATAL_ERROR "default run must emit health_reporter_generated.h")
endif()
file(READ "${ROOT_ON}/generated/nodes.cmake" nodes_cmake_on)
if(NOT nodes_cmake_on MATCHES "health_reporter")
  message(FATAL_ERROR "default run must list health_reporter in nodes.cmake")
endif()
file(READ "${ROOT_ON}/generated/graph_ir.h" ir_on)
if(NOT ir_on MATCHES "kGraphNodesCount = 2")
  message(FATAL_ERROR "default run must inject exactly one extra node (tick + health_reporter)")
endif()

execute_process(COMMAND "${GEN}" --out "${ROOT_OFF}" --update generated
  --template-dir "${TPL}" --in "${TPL}/graph.yaml" --no-health-reporter
  RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "gen_graph (health inject off) failed: ${rc}")
endif()
if(EXISTS "${ROOT_OFF}/generated/health_reporter_generated.h")
  message(FATAL_ERROR "--no-health-reporter run must not emit health_reporter_generated.h")
endif()
file(READ "${ROOT_OFF}/generated/graph_ir.h" ir_off)
if(NOT ir_off MATCHES "kGraphNodesCount = 1")
  message(FATAL_ERROR "--no-health-reporter run must keep the single-node graph")
endif()

message(STATUS "health reporter injection OK")
