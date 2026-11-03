cmake_minimum_required(VERSION 3.16)
project(thyme_gen_graph_tools LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
add_executable(test_gen_graph_yaml test_yaml.cpp yaml.cpp)
target_include_directories(test_gen_graph_yaml PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_CURRENT_SOURCE_DIR}/../../../include)

add_executable(test_gen_graph_yaml_string test_yaml_string.cpp yaml.cpp)
target_include_directories(test_gen_graph_yaml_string PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_CURRENT_SOURCE_DIR}/../../../include)

add_executable(test_gen_graph_rate test_rate.cpp yaml.cpp dag.cpp rate.cpp)
target_include_directories(test_gen_graph_rate PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_CURRENT_SOURCE_DIR}/../../../include)
target_compile_definitions(test_gen_graph_rate PRIVATE
  GEN_GRAPH_TESTDATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/testdata")

add_executable(test_gen_graph_rate_warn test_rate_warn.cpp yaml.cpp rate.cpp depth.cpp)
target_include_directories(test_gen_graph_rate_warn PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_CURRENT_SOURCE_DIR}/../../../include)
target_compile_definitions(test_gen_graph_rate_warn PRIVATE
  GEN_GRAPH_TESTDATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/testdata")

add_executable(test_gen_graph_depth_warn test_depth_warn.cpp yaml.cpp rate.cpp depth.cpp)
target_include_directories(test_gen_graph_depth_warn PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_CURRENT_SOURCE_DIR}/../../../include)
target_compile_definitions(test_gen_graph_depth_warn PRIVATE
  DEPTH_SUGGEST_FIXTURE="${CMAKE_CURRENT_SOURCE_DIR}/testdata/depth_suggest.json")

add_executable(test_gen_graph_validate test_validate.cpp yaml.cpp validate.cpp)
target_include_directories(test_gen_graph_validate PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_CURRENT_SOURCE_DIR}/../../../include)

add_executable(test_channel_capabilities test_channel_capabilities.cpp yaml.cpp capability.cpp)
target_include_directories(test_channel_capabilities PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_CURRENT_SOURCE_DIR}/../../../include)

# GEN-09: generated-tree metadata (hash stability, JSON round-trip, migration
# diagnostics). Pure unit test, no filesystem or binary needed.
add_executable(test_gen_graph_meta test_meta.cpp meta.cpp)
target_include_directories(test_gen_graph_meta PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_CURRENT_SOURCE_DIR}/../../../include)

add_executable(test_gen_graph_sched test_sched.cpp yaml.cpp dag.cpp capacity.cpp sched_plan.cpp)
target_include_directories(test_gen_graph_sched PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_CURRENT_SOURCE_DIR}/../../../include)
target_compile_definitions(test_gen_graph_sched PRIVATE
  GEN_GRAPH_TESTDATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/testdata")

add_executable(gen_graph main.cpp yaml.cpp graph_json.cpp graph_shard.cpp dag.cpp rate.cpp depth.cpp capacity.cpp sched_plan.cpp emit.cpp emit_util.cpp
               emit_validate.cpp emit_ports.cpp emit_nodes.cpp emit_ir.cpp
               emit_sched.cpp
               emit_record.cpp emit_replay.cpp emit_wiring.cpp
               emit_integration.cpp emit_viz.cpp scaffold.cpp catalog.cpp
               derive_template.cpp validate.cpp meta.cpp capability.cpp emit_capability.cpp emit_pinned_slot.cpp emit_frame.cpp hub.cpp)
target_include_directories(gen_graph PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_CURRENT_SOURCE_DIR}/../../../include)
target_compile_definitions(gen_graph PRIVATE
  GEN_GRAPH_TEMPLATE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/scaffold")
# NO target_link_libraries to thyme_runtime: gen_graph is a host tool.
# test_empty_app is registered in the top-level CMakeLists.txt.

# GEN-07 — structured fuzz driver for gen_graph inputs. Spawns the built
# binary on fixture-seeded YAML-aware mutations and asserts exit-code/output
# properties; the quick/full ctest registrations live in the top-level
# CMakeLists next to the other TST-03 fuzz passes.
add_executable(fuzz_gen_graph fuzz_gen_graph.cpp)
target_include_directories(fuzz_gen_graph PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_CURRENT_SOURCE_DIR}/../../../tests)
target_compile_definitions(fuzz_gen_graph PRIVATE
  GEN_GRAPH_BIN="$<TARGET_FILE:gen_graph>"
  GEN_GRAPH_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
  GEN_GRAPH_FUZZ_TMPDIR="${CMAKE_BINARY_DIR}/fuzz_gen_graph")
add_dependencies(fuzz_gen_graph gen_graph)

option(THYME_ENABLE_FUZZ "Build libFuzzer harness for gen_graph yaml parser" OFF)
if(THYME_ENABLE_FUZZ)
  if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    message(FATAL_ERROR
      "THYME_ENABLE_FUZZ requires Clang (got ${CMAKE_CXX_COMPILER_ID})")
  endif()
  add_executable(fuzz_gen_graph_yaml fuzz_yaml.cpp yaml.cpp)
  target_include_directories(fuzz_gen_graph_yaml PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../include)
  target_compile_options(fuzz_gen_graph_yaml PRIVATE
    -fsanitize=fuzzer,address,undefined -g -O1)
  target_link_options(fuzz_gen_graph_yaml PRIVATE
    -fsanitize=fuzzer,address,undefined)
  file(GLOB_RECURSE FUZZ_YAML_CORPUS CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/testdata/*.yaml")
  add_test(NAME test_fuzz_yaml_corpus
    COMMAND fuzz_gen_graph_yaml -runs=0 ${FUZZ_YAML_CORPUS})
  set_tests_properties(test_fuzz_yaml_corpus PROPERTIES LABELS "fuzz;mr_gate")
endif()
