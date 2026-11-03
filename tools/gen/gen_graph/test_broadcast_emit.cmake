# Task 5: emit-only checks for broadcast Ports / Dispatch / wiring Bind.
# Does not create testdata/broadcast_tpl (Task 6 runtime fixture).
if(NOT GEN OR NOT THYME)
  message(FATAL_ERROR "GEN and THYME required")
endif()
get_filename_component(THYME_ABS "${THYME}" ABSOLUTE)
set(TPL "${THYME_ABS}/tools/gen/gen_graph/testdata/event_tpl")
set(ROOT "${CMAKE_BINARY_DIR}/broadcast_emit_app")
set(YAML "${CMAKE_BINARY_DIR}/broadcast_emit_graph.yaml")
file(REMOVE_RECURSE "${ROOT}")

file(WRITE "${YAML}" [=[
version: 1
pool_n: 4
ring_cap: 8
nodes:
  - id: prod
    type: producer
    hz: 20
    process: prod
  - id: cyc
    type: cyc
    hz: 20
    process: cyc
  - id: ev
    type: ev
    hz: 20
    process: ev
    exec: event
  - id: evl
    type: evl
    hz: 20
    process: evl
    exec: event
edges:
  - from: prod
    to: cyc
    channel: tick
    cpp_type: Reading
    shm: _tick
    delivery: broadcast
    read: latest
  - from: prod
    to: ev
    channel: tick
    cpp_type: Reading
    shm: _tick
    delivery: broadcast
    read: follow
  - from: prod
    to: evl
    channel: tick
    cpp_type: Reading
    shm: _tick
    delivery: broadcast
    read: latest
processes:
  - id: prod
    role: prod
  - id: cyc
    role: cyc
  - id: ev
    role: ev
  - id: evl
    role: evl
supervisor:
  shm_prefix: "/c9_%pid"
  health_n: 5
]=])

execute_process(COMMAND "${GEN}" --out "${ROOT}" --update generated
  --template-dir "${TPL}" --in "${YAML}" --no-health-reporter
  RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "gen_graph (broadcast emit) failed: ${rc}")
endif()

function(expect_contains file needle)
  file(READ "${file}" body)
  string(FIND "${body}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "broadcast emit: ${file} missing: ${needle}")
  endif()
endfunction()

function(expect_missing file needle)
  file(READ "${file}" body)
  string(FIND "${body}" "${needle}" pos)
  if(NOT pos EQUAL -1)
    message(FATAL_ERROR "broadcast emit: ${file} must not contain: ${needle}")
  endif()
endfunction()

# Known producer: graph-injected writer, not a known_types.txt pool token.
expect_contains("${ROOT}/generated/mp_ports.h" "FrameChannelRef<Reading,")
expect_contains("${ROOT}/generated/prod_generated.h" "FrameChannelRef<Reading,")
expect_contains("${ROOT}/generated/prod_generated.h" "tick_w_(ports.tick_w_)")
expect_contains("${ROOT}/generated/prod_generated.h" "bool OnTick(const TickIn& in, TickOut* out) = 0")

# Cyclic consumer: FrameChannelRef member and the same tick transaction.
expect_contains("${ROOT}/generated/mp_ports.h" "FrameChannelRef<Reading,")
expect_contains("${ROOT}/generated/cyc_generated.h" "FrameChannelRef<Reading,")
expect_contains("${ROOT}/generated/cyc_generated.h" "OnTick(in, &out);")
expect_missing("${ROOT}/generated/cyc_generated.h" "TryPop")
expect_missing("${ROOT}/generated/cyc_generated.h" "ReadLatest")
file(READ "${ROOT}/generated/cyc_generated.h" cyc_body)
string(FIND "${cyc_body}" "void Loop() override final" loop_pos)
string(FIND "${cyc_body}" "OnTick(in, &out);" onloop_pos)
if(loop_pos EQUAL -1 OR onloop_pos EQUAL -1 OR NOT loop_pos LESS onloop_pos)
  message(FATAL_ERROR "broadcast emit: cyclic Loop() must call OnTick()")
endif()

# Event follow: while(TryRead) + AddDrops; event latest: if(TryRead).
expect_contains("${ROOT}/generated/ev_generated.h" "while (tick_.TryRead(&v) == thyme::ChannelStatus::kOk)")
expect_contains("${ROOT}/generated/ev_generated.h" "AddDrops")
expect_contains("${ROOT}/generated/ev_generated.h" "drop_count()")
expect_contains("${ROOT}/generated/ev_generated.h"
  "virtual void OnTick(const FrameChannelRef<Reading,")
expect_contains("${ROOT}/generated/evl_generated.h" "if (tick_.TryRead(&v) == thyme::ChannelStatus::kOk)")
expect_contains("${ROOT}/generated/ev_generated.h"
  "NoteCallbackWork(static_cast<uint8_t>(RecordStructId::kTick), NowNs() - callback_begin);")
expect_contains("${ROOT}/generated/evl_generated.h"
  "NoteCallbackWork(static_cast<uint8_t>(RecordStructId::kTick), NowNs() - callback_begin);")
expect_missing("${ROOT}/generated/evl_generated.h" "while (tick_->TryPop(v))")

# MP factories take BroadcastTransports; wait inputs are SpmcReader*.
expect_contains("${ROOT}/generated/mp_unknown.h" "BroadcastTransports& bc")
expect_contains("${ROOT}/generated/mp_unknown.h" "&bc.ev_tick_r_")
expect_contains("${ROOT}/generated/mp_unknown.h" "&bc.evl_tick_r_")
expect_contains("${ROOT}/generated/mp_roles.h" "BroadcastTransports& bc")
expect_contains("${ROOT}/generated/mp_roles.h" "&bc.tick_w_")
expect_contains("${ROOT}/generated/mp_unknown.h" "&bc.cyc_tick_r_")

# Broadcast must not land on EventTransports NotifPipe / EventPipeRef.
expect_missing("${ROOT}/generated/mp_unknown.h" "ev_tick_pipe")
expect_missing("${ROOT}/generated/mp_ports.h"
  "EventPipeRef<thyme::Frame<Reading>, 8> tick_;")
expect_missing("${ROOT}/generated/replay_inject.h" "ev_tick_pipe")
expect_contains("${ROOT}/generated/replay_inject.h"
  "broadcast_transports_.tick_w_.Push")

# Pipeline wiring Bind (reader -> writer ring).
expect_contains("${ROOT}/generated/wiring.cpp" "SpmcRead::Follow")
expect_contains("${ROOT}/generated/wiring.cpp" "SpmcRead::Latest")
expect_contains("${ROOT}/generated/wiring.cpp" ".Bind(&")
expect_contains("${ROOT}/generated/wiring.cpp" "&broadcast_transports_.ev_tick_r_")

message(STATUS "broadcast emit golden OK")
