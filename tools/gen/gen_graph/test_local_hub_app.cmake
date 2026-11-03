if(NOT GEN OR NOT THYME)
  message(FATAL_ERROR "GEN and THYME required")
endif()
get_filename_component(THYME_ABS "${THYME}" ABSOLUTE)
set(TPL "${THYME_ABS}/tools/gen/gen_graph/testdata/broadcast_tpl")
set(ROOT "${CMAKE_BINARY_DIR}/local_hub_app_test")
set(BUILD "${ROOT}/build")
set(YAML "${CMAKE_BINARY_DIR}/local_hub_app_test.yaml")
file(REMOVE_RECURSE "${ROOT}")
file(WRITE "${YAML}" [=[version: 1
pool_n: 4
ring_cap: 8
nodes:
  - id: prod
    type: prod
    hz: 20
    process: main
  - id: sink_a
    type: sink_a
    hz: 20
    process: main
    exec: event
  - id: sink_b
    type: sink_b
    hz: 20
    process: main
    exec: event
edges:
  - from: prod
    to: sink_a
    channel: tick
    cpp_type: Tick
  - from: prod
    to: sink_b
    channel: tick
    cpp_type: Tick
processes:
  - id: main
    role: main
supervisor:
  shm_prefix: "/c9_%pid"
  health_n: 3
]=])

execute_process(COMMAND "${GEN}" --out "${ROOT}" --update all
  --template-dir "${TPL}" --in "${YAML}" --no-health-reporter
  RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "gen_graph (local Hub fixture) failed: ${rc}")
endif()

function(expect_contains file needle)
  file(READ "${file}" body)
  string(FIND "${body}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "local Hub: ${file} missing expected content: ${needle}")
  endif()
endfunction()
function(expect_one file needle)
  file(READ "${file}" body)
  string(FIND "${body}" "${needle}" first)
  if(first EQUAL -1)
    message(FATAL_ERROR "local Hub: ${file} missing expected content: ${needle}")
  endif()
  string(FIND "${body}" "${needle}" second)
  # CMake has no find-after-start for strings; count via replacement length.
  string(REPLACE "${needle}" "" shortened "${body}")
  string(LENGTH "${body}" before_len)
  string(LENGTH "${shortened}" after_len)
  string(LENGTH "${needle}" needle_len)
  math(EXPR count "(${before_len} - ${after_len}) / ${needle_len}")
  if(NOT count EQUAL 1)
    message(FATAL_ERROR "local Hub: ${file} contains ${count} copies of: ${needle}")
  endif()
endfunction()

expect_contains("${ROOT}/generated/channel_capabilities.json" "\"hub_required\": true")
expect_contains("${ROOT}/generated/wiring.h" "LocalFrameHub<Tick, 8, 2> tick_hub_")
expect_contains("${ROOT}/generated/wiring.h" "SpHubDispatcher hub_dispatcher_")
expect_contains("${ROOT}/generated/mp_unknown.h" "class LocalHubDispatcher")
expect_one("${ROOT}/generated/wiring.cpp" "tick_hub_.input().SetRecordTap")
expect_one("${ROOT}/generated/mp_unknown.h" "tick_hub_.input().SetRecordTap")

file(WRITE "${ROOT}/src/nodes/prod.cpp" [==[#include "nodes/prod.h"
bool ProdThread::OnTick(const TickIn&, TickOut* out) {
  if (out->tick == nullptr) return true;
  out->tick->value = static_cast<uint32_t>(loop_count() + 1);
  return true;
}
]==])
file(WRITE "${ROOT}/src/nodes/sink_a.cpp" [==[#include "nodes/sink_a.h"
#include <cstdlib>
void SinkAThread::OnTick(const decltype(tick_)::Sample& value) {
  if (value.payload == nullptr || value.payload->value != value.sequence) std::abort();
}
]==])
file(WRITE "${ROOT}/src/nodes/sink_b.cpp" [==[#include "nodes/sink_b.h"
#include <cstdlib>
void SinkBThread::OnTick(const decltype(tick_)::Sample& value) {
  if (value.payload == nullptr || value.payload->value != value.sequence) std::abort();
}
]==])

execute_process(COMMAND "${CMAKE_COMMAND}" -S "${ROOT}" -B "${BUILD}"
  -DTHYME_SOURCE_DIR=${THYME_ABS} -DTHYME_BUILD_TESTS=OFF RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "local Hub app configure failed")
endif()
execute_process(COMMAND "${CMAKE_COMMAND}" --build "${BUILD}" --target app --parallel 4
  RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "local Hub app build failed")
endif()

execute_process(COMMAND "${BUILD}/app" --seconds 1
  RESULT_VARIABLE rc OUTPUT_VARIABLE sp_out ERROR_VARIABLE sp_err)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "local Hub SP exited ${rc}: ${sp_out}\n${sp_err}")
endif()
if(NOT sp_out MATCHES "sink_a[ ]+handled=[1-9][0-9]*" OR
   NOT sp_out MATCHES "sink_b[ ]+handled=[1-9][0-9]*")
  message(FATAL_ERROR "local Hub SP did not reach both outputs: ${sp_out}\n${sp_err}")
endif()

execute_process(COMMAND "${BUILD}/app" --mp --seconds 1
  RESULT_VARIABLE rc OUTPUT_VARIABLE mp_out ERROR_VARIABLE mp_err)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "local Hub MP exited ${rc}: ${mp_out}\n${mp_err}")
endif()
if(mp_out MATCHES "err=[1-9][0-9]*")
  message(FATAL_ERROR "local Hub MP reported an error: ${mp_out}\n${mp_err}")
endif()

# One local subscriber stays direct unless dynamic explicitly requests a Hub.
set(DIRECT_YAML "${CMAKE_BINARY_DIR}/local_hub_direct.yaml")
file(WRITE "${DIRECT_YAML}" [=[version: 1
pool_n: 4
ring_cap: 8
nodes:
  - id: prod
    type: prod
    hz: 20
    process: main
  - id: sink_a
    type: sink_a
    hz: 20
    process: main
    exec: event
edges:
  - from: prod
    to: sink_a
    channel: tick
    cpp_type: Tick
processes:
  - id: main
    role: main
supervisor:
  shm_prefix: "/c9_%pid"
  health_n: 2
]=])
foreach(mode direct dynamic)
  set(OUT "${CMAKE_BINARY_DIR}/local_hub_${mode}")
  file(REMOVE_RECURSE "${OUT}")
  if(mode STREQUAL dynamic)
    file(READ "${DIRECT_YAML}" mode_yaml)
    string(REPLACE "cpp_type: Tick" "cpp_type: Tick\n    dynamic: true" mode_yaml "${mode_yaml}")
    file(WRITE "${DIRECT_YAML}" "${mode_yaml}")
  endif()
  execute_process(COMMAND "${GEN}" --out "${OUT}" --update generated
    --template-dir "${TPL}" --in "${DIRECT_YAML}" --no-health-reporter
    RESULT_VARIABLE rc)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "gen_graph (${mode} local fan-out) failed: ${rc}")
  endif()
  file(READ "${OUT}/generated/wiring.h" mode_wiring)
  string(FIND "${mode_wiring}" "LocalFrameHub" hub_pos)
  if(mode STREQUAL direct AND NOT hub_pos EQUAL -1)
    message(FATAL_ERROR "single direct subscriber unexpectedly emitted a Hub")
  endif()
  if(mode STREQUAL dynamic AND hub_pos EQUAL -1)
    message(FATAL_ERROR "dynamic:true did not emit a Hub")
  endif()
endforeach()
