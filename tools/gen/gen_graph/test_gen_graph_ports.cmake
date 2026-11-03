# Build-and-run check for the generated mp wiring shape (ports fixture):
#   - mp_ports.h ports/context structs + grouped ctor call sites compile
#   - generated graph_check.h enforces the health_reporter-last order
#   (a graph with health_reporter not last must FAIL to compile)
if(NOT GEN OR NOT THYME OR NOT TPL2_SRC)
  message(FATAL_ERROR "GEN, THYME and TPL2_SRC required")
endif()
get_filename_component(THYME_ABS "${THYME}" ABSOLUTE)
set(TPL "${CMAKE_BINARY_DIR}/ports_tpl")
set(ROOT "${CMAKE_BINARY_DIR}/ports_app")
set(ROOT_BAD "${CMAKE_BINARY_DIR}/ports_app_bad")
file(REMOVE_RECURSE "${TPL}" "${ROOT}" "${ROOT_BAD}")
file(COPY "${TPL2_SRC}/" DESTINATION "${TPL}")

# --- good graph: generate, configure, build, run ---
execute_process(COMMAND "${GEN}" --out "${ROOT}" --update all
  --template-dir "${TPL}" --in "${TPL}/graph.yaml" RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "gen_graph (ports fixture) failed: ${rc}")
endif()

# Pre-seed the handwritten node bodies so their OnLoop is observable: the
# scaffold emits empty OnLoop stubs for every node (known and unknown); the
# test overrides them. When replay pauses the producer (base Loop guard), the
# health slot keeps zero drops while the consumer and the health reporter run.
file(WRITE "${ROOT}/src/nodes/prod.h"
  "#pragma once\n#include \"generated/prod_generated.h\"\n"
  "class ProducerThread final : public ProducerThreadGenerated {\n"
  " public:\n"
  "  using ProducerThreadGenerated::ProducerThreadGenerated;\n"
  " protected:\n"
  "  bool OnTick(const TickIn& in, TickOut* out) override;\n"
  "};\n")
file(WRITE "${ROOT}/src/nodes/prod.cpp"
  "#include \"nodes/prod.h\"\n"
  "bool ProducerThread::OnTick(const TickIn&, TickOut* out) {\n"
  "  my_health_->AddDrops(1);\n"
  "  out->ready_payload = true;\n"
  "  return true;\n"  "}\n")
file(WRITE "${ROOT}/src/nodes/cons.cpp"
  "#include \"nodes/cons.h\"\n"
  "bool ConsumerThread::OnTick(const TickIn&, TickOut*) {\n"
  "  my_health_->AddDrops(1);\n"
  "  return true;\n"  "}\n")
file(WRITE "${ROOT}/src/nodes/health_reporter.cpp"
  "#include \"nodes/health_reporter.h\"\n"
  "void HealthReporterThread::OnLoop() { my_health_->AddDrops(1); }\n")

function(expect_contains file needle)
  file(READ "${file}" body)
  string(FIND "${body}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "ports: ${file} missing expected content: ${needle}")
  endif()
endfunction()

expect_contains("${ROOT}/generated/mp_ports.h" "struct MpContext")
expect_contains("${ROOT}/generated/mp_ports.h" "struct ProducerPorts")
expect_contains("${ROOT}/generated/mp_ports.h" "struct ConsumerThreadPorts")
expect_contains("${ROOT}/generated/mp_ports.h" "thyme::LocalIndexQueue<FooPool, kRingCap> foo_queue")
expect_contains("${ROOT}/generated/mp_ports.h" "kRingCap> foo_queue")
expect_contains("${ROOT}/generated/wiring.cpp"
  "consumer(ConsumerThreadPorts{thyme::LocalIndexQueue<FooPool, kRingCap>(&foo_pool, &foo_ring)}, MpContext{&health[1]})")
expect_contains("${ROOT}/generated/wiring.cpp"
  "prod(ProducerPorts{thyme::LocalIndexQueue<FooPool, kRingCap>(&foo_pool, &foo_ring)}, MpContext{&health[0]})")
expect_contains("${ROOT}/generated/mp_roles.h"
  "return new ConsumerThread(ConsumerThreadPorts{thyme::LocalIndexQueue<FooPool, kRingCap>(&l.foo_pool, &l.foo_ring)}, MpContext{&health_slots[1]});")

# REP-03: drive the real generated ReplayInjectOne pool/latest path through
# RunReplay, including source-id remapping, REP-12 and REC-04 seek behavior.
configure_file("${THYME_ABS}/tools/gen/gen_graph/testdata/replay_behavior.cpp"
  "${ROOT}/tools/replay_behavior.cpp" COPYONLY)
file(APPEND "${ROOT}/CMakeLists.txt"
  "\nadd_executable(replay_behavior tools/replay_behavior.cpp)\n"
  "target_link_libraries(replay_behavior PRIVATE ports_app_lib)\n")

execute_process(COMMAND "${CMAKE_COMMAND}" -S "${ROOT}" -B "${ROOT}/build"
  -DTHYME_SOURCE_DIR=${THYME_ABS} -DTHYME_BUILD_TESTS=OFF RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "ports app configure failed")
endif()
execute_process(COMMAND "${CMAKE_COMMAND}" --build "${ROOT}/build" --parallel 1
  RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "ports app build failed")
endif()
execute_process(COMMAND "${ROOT}/build/replay_behavior"
  "${CMAKE_BINARY_DIR}/ports_replay_data" RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "generated replay behavior exited ${rc}")
endif()
execute_process(COMMAND "${ROOT}/build/app" RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "ports app exited ${rc}")
endif()

# --- produce: many emits the bounded OnProduce loop (stub publishes nothing) ---
set(MANY_ROOT "${CMAKE_BINARY_DIR}/ports_many_app")
file(REMOVE_RECURSE "${MANY_ROOT}")
file(READ "${TPL}/graph.yaml" ports_yaml)
string(REPLACE "  - id: prod\n    type: producer\n"
  "  - id: prod\n    type: producer\n    produce: many\n"
  many_yaml "${ports_yaml}")
set(MANY_GRAPH "${CMAKE_BINARY_DIR}/ports_many_graph.yaml")
file(WRITE "${MANY_GRAPH}" "${many_yaml}")
execute_process(COMMAND "${GEN}" --out "${MANY_ROOT}" --update all
  --template-dir "${TPL}" --in "${MANY_GRAPH}" RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "produce: many generation failed")
endif()
expect_contains("${MANY_ROOT}/generated/prod_generated.h"
  "OnProduce(const TickIn& in, TickOut* out)")
expect_contains("${MANY_ROOT}/generated/prod_generated.h"
  "for (uint32_t budget = 0; budget < kRingCap; ++budget)")
file(READ "${MANY_ROOT}/generated/prod_generated.h" many_generated)
string(REGEX MATCHALL "auto out_[a-z_]+ = [a-z_]+\\.BeginWrite\\(\\)" many_borrows "${many_generated}")
list(LENGTH many_borrows many_borrow_count)
if(NOT many_borrow_count EQUAL 1)
  message(FATAL_ERROR "produce: many must borrow only inside its bounded loop")
endif()
file(WRITE "${MANY_ROOT}/src/nodes/cons.cpp"
  "#include \"nodes/cons.h\"\n"
  "bool ConsumerThread::OnTick(const TickIn&, TickOut*) {\n"
  "  my_health_->AddDrops(1);\n"
  "  return true;\n"  "}\n")
file(WRITE "${MANY_ROOT}/src/nodes/health_reporter.cpp"
  "#include \"nodes/health_reporter.h\"\n"
  "void HealthReporterThread::OnLoop() { my_health_->AddDrops(1); }\n")
execute_process(COMMAND "${CMAKE_COMMAND}" -S "${MANY_ROOT}" -B "${MANY_ROOT}/build"
  -DTHYME_SOURCE_DIR=${THYME_ABS} -DTHYME_BUILD_TESTS=OFF RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "produce: many app configure failed")
endif()
execute_process(COMMAND "${CMAKE_COMMAND}" --build "${MANY_ROOT}/build" --parallel 1
  RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "produce: many app build failed")
endif()
execute_process(COMMAND "${MANY_ROOT}/build/app" RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "produce: many app exited ${rc}")
endif()

# --- bad graph (health_reporter not last): must fail to compile ---
execute_process(COMMAND "${GEN}" --out "${ROOT_BAD}" --update all
  --template-dir "${TPL}" --in "${TPL}/graph_bad.yaml" RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "gen_graph (bad fixture) failed: ${rc}")
endif()
execute_process(COMMAND "${CMAKE_COMMAND}" -S "${ROOT_BAD}" -B "${ROOT_BAD}/build"
  -DTHYME_SOURCE_DIR=${THYME_ABS} -DTHYME_BUILD_TESTS=OFF RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "bad ports app configure failed")
endif()
execute_process(COMMAND "${CMAKE_COMMAND}" --build "${ROOT_BAD}/build"
  RESULT_VARIABLE rc OUTPUT_VARIABLE bad_out ERROR_VARIABLE bad_out)
if(rc EQUAL 0)
  message(FATAL_ERROR "bad ports app compiled; graph_check.h last-node assert did not fire")
endif()
