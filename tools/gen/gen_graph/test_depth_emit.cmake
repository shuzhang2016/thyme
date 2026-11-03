# M5.3: per-edge depth changes EventPipeRef / NotifPipe / ShmSpscRing Cap.
if(NOT GEN OR NOT THYME)
  message(FATAL_ERROR "GEN and THYME required")
endif()
get_filename_component(THYME_ABS "${THYME}" ABSOLUTE)
set(TPL "${THYME_ABS}/tools/gen/gen_graph/testdata/event_tpl")

function(expect_contains file needle)
  file(READ "${file}" body)
  string(FIND "${body}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "depth emit: ${file} missing: ${needle}")
  endif()
endfunction()

function(expect_missing file needle)
  file(READ "${file}" body)
  string(FIND "${body}" "${needle}" pos)
  if(NOT pos EQUAL -1)
    message(FATAL_ERROR "depth emit: ${file} must not contain: ${needle}")
  endif()
endfunction()

# Same-process notify: depth 16 overrides ring_cap 8.
set(ROOT_N "${CMAKE_BINARY_DIR}/depth_notify_app")
file(REMOVE_RECURSE "${ROOT_N}")
execute_process(COMMAND "${GEN}" --out "${ROOT_N}" --update generated
  --template-dir "${TPL}" --in "${TPL}/graph_depth_notify.yaml"
  --no-health-reporter RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "gen_graph (depth notify) failed: ${rc}")
endif()
expect_contains("${ROOT_N}/generated/sink_generated.h"
  "FrameChannelRef<Reading, thyme::ChannelSemanticsPack(thyme::ChannelTransportKind::kFrame, thyme::ChannelRetention::kQueue, thyme::ChannelReaders::kOne, thyme::ChannelOverflow::kDrop), 16> readings_;")
expect_contains("${ROOT_N}/generated/wiring.h"
  "thyme::NotifPipe<thyme::Frame<Reading>, 16> sink_readings_pipe;")
expect_missing("${ROOT_N}/generated/sink_generated.h"
  "EventPipeRef<thyme::Frame<Reading>, 8> readings_;")

# Cross-process queue: depth 32 on ShmSpscRing (MP EventTransports).
set(ROOT_Q "${CMAKE_BINARY_DIR}/depth_queue_app")
file(REMOVE_RECURSE "${ROOT_Q}")
execute_process(COMMAND "${GEN}" --out "${ROOT_Q}" --update generated
  --template-dir "${TPL}" --in "${TPL}/graph_depth_queue.yaml"
  --no-health-reporter RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "gen_graph (depth queue) failed: ${rc}")
endif()
expect_contains("${ROOT_Q}/generated/mp_unknown.h"
  "thyme::ShmSpscRing<thyme::Frame<Reading>, 32> sink_readings_pipe;")
expect_contains("${ROOT_Q}/generated/mp_ports.h"
  "FrameChannelRef<Reading, thyme::ChannelSemanticsPack(thyme::ChannelTransportKind::kFrame, thyme::ChannelRetention::kQueue, thyme::ChannelReaders::kOne, thyme::ChannelOverflow::kReject), 32> readings_;")
expect_missing("${ROOT_Q}/generated/mp_unknown.h"
  "ShmSpscRing<thyme::Frame<Reading>, 8>")

message(STATUS "depth emit golden OK")
