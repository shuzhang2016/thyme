cmake_minimum_required(VERSION 3.20)
project(thyme_app LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT TARGET thyme::runtime)
  set(THYME_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../thyme" CACHE PATH "thyme source")
  add_subdirectory("${THYME_SOURCE_DIR}" "${CMAKE_BINARY_DIR}/thyme")
endif()
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../thyme-viz/CMakeLists.txt")
  add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../thyme-viz" "${CMAKE_BINARY_DIR}/thyme-viz")
  set(APP_HAVE_VIZ ON)
else()
  set(APP_HAVE_VIZ OFF)
  message(STATUS "thyme-viz not found as sibling — skipping viz_bridge/recorder tools")
endif()
include(${CMAKE_CURRENT_SOURCE_DIR}/generated/nodes.cmake)
foreach(id IN LISTS APP_GEN_NODES)
  if(NOT EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/src/nodes/${id}.cpp)
    message(FATAL_ERROR "missing hand node src/nodes/${id}.cpp")
  endif()
endforeach()
set(_node_srcs "")
foreach(id IN LISTS APP_GEN_NODES)
  list(APPEND _node_srcs src/nodes/${id}.cpp)
endforeach()
# Generic scaffold glue from src/support plus the multi-process role/supervisor
# adapters (src/mp). The mp entry points are declared in src/mp/mp_entry.h
# (Seam A); the role/supervisor config is derived from the generated tables.
# viz_poll.cpp and the thyme-viz link only build when the optional sibling
# repo is present (greenfield invariant: ../thyme alone must build).
set(_app_srcs ${_node_srcs} generated/wiring.cpp
  src/support/record_trigger.cpp
  src/support/replay_runner.cpp src/support/scheduling_setup.cpp
  src/support/trigger_relay.cpp
  src/support/join_metrics.cpp
  src/mp/mp_roles.cpp src/mp/mp_supervisor_adapter.cpp)
set(_app_link_libs thyme::runtime thyme::recorder thyme::app thyme::replay)
if(APP_HAVE_VIZ)
  list(APPEND _app_srcs src/support/viz_poll.cpp)
  list(APPEND _app_link_libs thyme_viz::viz)
endif()
add_library(thyme_app_lib STATIC ${_app_srcs})
target_include_directories(thyme_app_lib PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_CURRENT_SOURCE_DIR}/include
  ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(thyme_app_lib PUBLIC ${_app_link_libs})
add_executable(app src/main.cpp)
target_link_libraries(app PRIVATE thyme_app_lib)

if(APP_HAVE_VIZ)
add_executable(recorder tools/recorder/main.cpp)
target_link_libraries(recorder PRIVATE thyme_app_lib thyme_viz::recorder_cli)
endif()

add_executable(thyme-recorderctl tools/recorderctl/main.cpp)
target_link_libraries(thyme-recorderctl PRIVATE thyme::recorder)

add_executable(replay_tool tools/replay_tool/main.cpp)
target_link_libraries(replay_tool PRIVATE thyme_app_lib)

if(APP_HAVE_VIZ)
add_executable(viz_bridge tools/viz_bridge/main.cpp)
target_link_libraries(viz_bridge PRIVATE thyme_app_lib thyme_viz::viz)
endif()

add_executable(bench_ad_stack tools/bench/bench_main.cpp)
target_link_libraries(bench_ad_stack PRIVATE thyme_app_lib)

add_executable(eval_chains tools/eval_chains.cpp)
target_link_libraries(eval_chains PRIVATE thyme_app_lib)

enable_testing()
add_test(NAME test_baseline_gate
  COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/tools/bench/test_baseline_gate.py)
set_tests_properties(test_baseline_gate PROPERTIES LABELS "unit;mr_gate")

add_test(NAME test_render_eval
  COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/tools/bench/test_render_eval.py)
set_tests_properties(test_render_eval PROPERTIES LABELS "unit;mr_gate")
