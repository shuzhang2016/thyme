# End-to-end event-node fixture: generate the event_tpl graph (a known-type
# cyclic producer forking into an exec=event consumer over a NotifPipe edge,
# then an exec=event -> exec=event hop), check the generated text shape, then
# compile and run the app asserting every pushed message reaches its
# On<Channel> handler exactly once and in order.
if(NOT GEN OR NOT THYME)
  message(FATAL_ERROR "GEN and THYME required")
endif()
get_filename_component(THYME_ABS "${THYME}" ABSOLUTE)
set(TPL "${THYME_ABS}/tools/gen/gen_graph/testdata/event_tpl")
set(ROOT "${CMAKE_BINARY_DIR}/event_app")
set(BUILD "${ROOT}/build")
file(REMOVE_RECURSE "${ROOT}")

execute_process(COMMAND "${GEN}" --out "${ROOT}" --update all
  --template-dir "${TPL}" --in "${TPL}/graph.yaml" --no-health-reporter
  RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "gen_graph (event fixture) failed: ${rc}")
endif()

function(expect_contains file needle)
  file(READ "${file}" body)
  string(FIND "${body}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "event golden: ${file} missing: ${needle}")
  endif()
endfunction()

# Generated event base: EventThread subclass, per-channel handlers, pipe drain.
expect_contains("${ROOT}/generated/sink_generated.h" "public thyme::EventThread")
expect_contains("${ROOT}/generated/sink_generated.h" "void Dispatch() override final")
expect_contains("${ROOT}/generated/sink_generated.h" "virtual void OnReadings(const FrameChannelRef<Reading,")
expect_contains("${ROOT}/generated/sink_generated.h" "NoteHandled();")
expect_contains("${ROOT}/generated/sink_generated.h" "const uint64_t callback_begin = NowNs();")
expect_contains("${ROOT}/generated/sink_generated.h"
  "NoteCallbackWork(static_cast<uint8_t>(RecordStructId::kReadings), NowNs() - callback_begin);")
expect_contains("${ROOT}/generated/relay_generated.h"
  "NoteCallbackWork(static_cast<uint8_t>(RecordStructId::kForwarded), NowNs() - callback_begin);")
expect_contains("${ROOT}/src/mp/mp_roles.cpp" "RegisterObservability(&ev_proc[ev_thread_count]);")
expect_contains("${ROOT}/src/mp/mp_roles.cpp" "ev_proc[i].FlushNew(proc_bw, ev_node[i]);")
expect_contains("${ROOT}/src/mp/mp_roles.cpp" "ev_threads[i]->cb_work(), 0")
expect_contains("${ROOT}/src/mp/mp_roles.cpp" "%s_cbwork_%s_%d.bin")
expect_contains("${ROOT}/generated/sink_generated.h" "ReplayProducerShouldPause(name())")
expect_contains("${ROOT}/generated/sink_generated.h" "FrameChannelRef<Reading,")
expect_contains("${ROOT}/generated/sink_generated.h" "RecordTapMirror(record_page_")
expect_contains("${ROOT}/generated/sink_generated.h" "forwarded_;")
expect_contains("${ROOT}/generated/relay_generated.h" "virtual void OnForwarded(const FrameChannelRef<Reading,")
expect_contains("${ROOT}/generated/prod_generated.h" "FrameChannelRef<Reading,")
expect_contains("${ROOT}/generated/prod_generated.h" "readings_(ports.readings_)")
expect_contains("${ROOT}/generated/prod_generated.h" "bool OnTick(const TickIn& in, TickOut* out) = 0")
# Wiring: pipes live on SpEventTransports; wait inputs registered on the event node.
expect_contains("${ROOT}/generated/wiring.h" "struct SpEventTransports {")
expect_contains("${ROOT}/generated/wiring.h" "thyme::NotifPipe<thyme::Frame<Reading>, 8> sink_readings_pipe;")
expect_contains("${ROOT}/generated/wiring.h" "SpEventTransports event_transports_;")
expect_contains("${ROOT}/generated/wiring.cpp" "sink.SetWaitInputs(ws, 1);")
expect_contains("${ROOT}/generated/wiring.cpp" "relay.SetWaitInputs(ws, 1);")
expect_contains("${ROOT}/generated/wiring.cpp" "&event_transports_.sink_readings_pipe")
# Event sinks registered before cyclic producers (App starts the event list first).
file(READ "${ROOT}/generated/wiring.cpp" wiring_cpp)
string(FIND "${wiring_cpp}" "if (!app_.Add(&sink))" sink_add)
string(FIND "${wiring_cpp}" "if (!app_.Add(&relay))" relay_add)
string(FIND "${wiring_cpp}" "if (!app_.Add(&prod))" prod_add)
if(sink_add EQUAL -1 OR relay_add EQUAL -1 OR prod_add EQUAL -1)
  message(FATAL_ERROR "event golden: wiring.cpp missing app_.Add for sink/relay/prod")
endif()
if(NOT sink_add LESS prod_add)
  message(FATAL_ERROR "event golden: wiring must Add sink before prod")
endif()
if(NOT relay_add LESS prod_add)
  message(FATAL_ERROR "event golden: wiring must Add relay before prod")
endif()
# IR + role factories.
expect_contains("${ROOT}/generated/graph_ir.h" "kGraphNodeIsEvent[] = {false, true, true}")
expect_contains("${ROOT}/generated/mp_unknown.h" "inline thyme::EventThread* MakeEventThread(")
# No shm queue edges in this fixture → no WireEventWake helper (wake attach
# lives inside MakeEventThread when queue consumers exist).
file(READ "${ROOT}/generated/mp_unknown.h" mp_unknown_body)
string(FIND "${mp_unknown_body}" "inline void WireEventWake" wire_wake_pos)
if(NOT wire_wake_pos EQUAL -1)
  message(FATAL_ERROR "event golden: mp_unknown.h must not emit WireEventWake")
endif()

# Positive queue-consumer golden: shm in-edge → EventTransports carries
# <node>_wake and MakeEventThread SetWakeWord's it. Emit-only (no compile/run).
set(ROOT_Q "${CMAKE_BINARY_DIR}/event_queue_app")
file(REMOVE_RECURSE "${ROOT_Q}")
execute_process(COMMAND "${GEN}" --out "${ROOT_Q}" --update generated
  --template-dir "${TPL}" --in "${TPL}/graph_shm_queue.yaml"
  --no-health-reporter RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "gen_graph (event shm-queue fixture) failed: ${rc}")
endif()
expect_contains("${ROOT_Q}/generated/mp_unknown.h"
  "inline thyme::EventThread* MakeEventThread(")
expect_contains("${ROOT_Q}/generated/mp_unknown.h"
  "struct EventTransports")
expect_contains("${ROOT_Q}/generated/mp_unknown.h"
  "std::atomic<uint32_t>* sink_wake")
expect_contains("${ROOT_Q}/generated/mp_unknown.h"
  "if (ev.sink_wake == nullptr)")
expect_contains("${ROOT_Q}/generated/mp_unknown.h"
  "t->SetWakeWord(ev.sink_wake)")
file(READ "${ROOT_Q}/generated/mp_unknown.h" mp_queue_body)
string(FIND "${mp_queue_body}" "!= nullptr) t->SetWakeWord" optional_wake_pos)
if(NOT optional_wake_pos EQUAL -1)
  message(FATAL_ERROR
    "event queue golden: MakeEventThread must not skip SetWakeWord when wake is null")
endif()
# (This fixture has no shm_kinds catalog, so the channel array stays empty —
# queue edges still drive EventTransports / SP NotifPipe downgrade below.)
expect_contains("${ROOT_Q}/generated/graph_ir.h" "const char* delivery;")
file(READ "${ROOT_Q}/generated/graph_ir.h" ir_q_body)
string(FIND "${ir_q_body}" "kGraphShmQueueChannels" qch_pos)
if(NOT qch_pos EQUAL -1)
  message(FATAL_ERROR
    "event queue golden: graph_ir.h must not emit kGraphShmQueueChannels")
endif()
# SP Pipeline downgrades delivery=queue to NotifPipe (no ShmSpscRing / wake).
expect_contains("${ROOT_Q}/generated/wiring.h" "struct SpEventTransports {")
expect_contains("${ROOT_Q}/generated/wiring.h"
  "thyme::NotifPipe<thyme::Frame<Reading>, 8> sink_readings_pipe;")
file(READ "${ROOT_Q}/generated/wiring.h" wiring_q_body)
string(FIND "${wiring_q_body}" "ShmSpscRing" shm_ring_pos)
if(NOT shm_ring_pos EQUAL -1)
  message(FATAL_ERROR
    "event queue golden: SP wiring.h must downgrade queue to NotifPipe")
endif()
string(FIND "${wiring_q_body}" "wake_region" wake_pos)
if(NOT wake_pos EQUAL -1)
  message(FATAL_ERROR
    "event queue golden: SP wiring.h must not create queue wake regions")
endif()
file(READ "${ROOT_Q}/generated/mp_unknown.h" mp_queue_body)
string(FIND "${mp_queue_body}" "inline void WireEventWake" wire_wake_q_pos)
if(NOT wire_wake_q_pos EQUAL -1)
  message(FATAL_ERROR
    "event queue golden: mp_unknown.h must not emit WireEventWake")
endif()
# Known-type producer: event out-edges are injected into Ports (channel_);
# MakeKnownThread takes EventTransports& (not a flat per-edge list).
expect_contains("${ROOT}/generated/mp_ports.h" "FrameChannelRef<Reading,")
expect_contains("${ROOT}/generated/mp_unknown.h" "struct EventTransports")
expect_contains("${ROOT}/generated/mp_roles.h" "EventTransports& ev")
expect_contains("${ROOT}/generated/mp_roles.h" "ProducerThreadPorts{&ev.sink_readings_pipe}")
# GEN-08: the generated check headers carry per-channel transport/cap asserts;
# this fixture's app build evaluates them, and the greps pin the emission.
expect_contains("${ROOT}/generated/graph_check.h"
  "framed wire bytes exceed the 64 KB transport limit")
expect_contains("${ROOT}/generated/graph_check.h" "Cap must be a power of two")
expect_contains("${ROOT}/generated/graph_check.h" "ring_cap must be a power of two")
expect_contains("${ROOT}/generated/record_registry.h"
  "512 KB record payload ceiling")
# SCH-03/SCH-04: input_budget + overrun policy are emitted into the generated
# EventThread base and its Dispatch guard.
set(ROOT_SCHED "${CMAKE_BINARY_DIR}/event_sched_app")
file(REMOVE_RECURSE "${ROOT_SCHED}")
execute_process(COMMAND "${GEN}" --out "${ROOT_SCHED}" --update generated
  --template-dir "${TPL}" --in "${TPL}/graph_sched.yaml"
  --no-health-reporter RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "gen_graph (event sched-policy fixture) failed: ${rc}")
endif()
expect_contains("${ROOT_SCHED}/generated/sink_generated.h"
  "SetInputBudget(4u, 1u);")
expect_contains("${ROOT_SCHED}/generated/sink_generated.h"
  "SetOverrunPolicy(thyme::OverrunPolicy::kDegrade);")
expect_contains("${ROOT_SCHED}/generated/sink_generated.h"
  "InputBudgetAvailable(0) && readings_.TryRead(&v) == thyme::ChannelStatus::kOk")
expect_contains("${ROOT_SCHED}/generated/sink_generated.h"
  "ConsumeInputBudget(0);")

# An unrecorded edge has no RecordStructId: do not invent a callback-work id or
# pull the record registry into a no-record graph.
set(ROOT_NOREC "${CMAKE_BINARY_DIR}/event_norecord_app")
set(YAML_NOREC "${CMAKE_BINARY_DIR}/event_norecord.yaml")
file(REMOVE_RECURSE "${ROOT_NOREC}")
file(WRITE "${YAML_NOREC}" [=[version: 1
pool_n: 4
ring_cap: 8
nodes:
  - id: prod
    type: producer
    hz: 50
    process: main
  - id: sink
    type: sink
    hz: 50
    process: main
    exec: event
edges:
  - from: prod
    to: sink
    channel: readings
    cpp_type: Reading
    transport: spsc
    record: false
processes:
  - id: main
    role: main
supervisor:
  shm_prefix: "/c9_%pid"
  health_n: 2
]=])
execute_process(COMMAND "${GEN}" --out "${ROOT_NOREC}" --update generated
  --template-dir "${TPL}" --in "${YAML_NOREC}" --no-health-reporter
  RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "gen_graph (event no-record fixture) failed: ${rc}")
endif()
file(READ "${ROOT_NOREC}/generated/sink_generated.h" norec_body)
string(FIND "${norec_body}" "NoteCallbackWork" norec_callback)
if(NOT norec_callback EQUAL -1)
  message(FATAL_ERROR "unrecorded event input must not emit NoteCallbackWork")
endif()
if(EXISTS "${ROOT_NOREC}/generated/record_registry.h")
  message(FATAL_ERROR "no-record event graph must not emit record_registry.h")
endif()

# Known catalog type + exec=event (orthogonal exec; no _ev type clone).
set(ROOT_KE "${CMAKE_BINARY_DIR}/event_known_app")
file(REMOVE_RECURSE "${ROOT_KE}")
execute_process(COMMAND "${GEN}" --out "${ROOT_KE}" --update generated
  --template-dir "${TPL}" --in "${TPL}/graph_known_event.yaml"
  --no-health-reporter RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "gen_graph (known+event fixture) failed: ${rc}")
endif()
expect_contains("${ROOT_KE}/generated/cons_generated.h" "public thyme::EventThread")
expect_contains("${ROOT_KE}/generated/cons_generated.h" "class ConsumerThreadEventGenerated")
expect_contains("${ROOT_KE}/generated/cons_generated.h" "const ConsumerThreadPorts& ports")
expect_contains("${ROOT_KE}/generated/mp_ports.h" "struct ConsumerThreadPorts")
expect_contains("${ROOT_KE}/generated/mp_unknown.h" "new ConsThread(")
expect_contains("${ROOT_KE}/generated/wiring.h" "ConsThread cons;")
# Replay injection into the pipe transport.
expect_contains("${ROOT}/generated/replay_inject.h"
  "PushOrWait(tmp, thyme::app::replay_mode::MakePushWaitGate())")
expect_contains("${ROOT}/generated/replay_stages.h"
  "if (thyme::app::replay_mode::Paused()) return true;")
# Hand stubs for the event node carry handler overrides, not OnLoop.
expect_contains("${ROOT}/src/nodes/sink.h" "void OnReadings(const FrameChannelRef<Reading,")

# Hand nodes: producer pushes 40 readings (retrying while the pipe is full);
# the sink counts, order-checks and forwards every handler invocation; the
# relay re-checks count and order on the event->event hop.
file(WRITE "${ROOT}/src/nodes/prod.h" [==[#pragma once
#include "generated/prod_generated.h"
class ProducerThread final : public ProducerThreadGenerated {
 public:
  using ProducerThreadGenerated::ProducerThreadGenerated;
  uint32_t sent = 0;
 protected:
  bool OnTick(const TickIn& in, TickOut* out) override;
};
]==])
file(WRITE "${ROOT}/src/nodes/prod.cpp" [==[#include "nodes/prod.h"
bool ProducerThread::OnTick(const TickIn&, TickOut* out) {
  if (sent >= 40) return false;
  if (out->readings == nullptr) return true;
  out->readings->value = sent + 1;
  ++sent;
  return true;
}
]==])
file(WRITE "${ROOT}/src/nodes/sink.h" [==[#pragma once
#include "generated/sink_generated.h"
#include <atomic>
class SinkThread final : public SinkThreadGenerated {
 public:
  using SinkThreadGenerated::SinkThreadGenerated;
  std::atomic<uint32_t> got{0};
  std::atomic<uint32_t> last_seq{0};
  std::atomic<int> order_violations{0};
  std::atomic<int> forward_failures{0};
 protected:
  void OnReadings(const decltype(readings_)::Sample& v) override;
};
]==])
file(WRITE "${ROOT}/src/nodes/sink.cpp" [==[#include "nodes/sink.h"
void SinkThread::OnReadings(const decltype(readings_)::Sample& v) {
  RecordTap(RecordStructId::kReadings, *v.frame());
  const uint32_t s = v.sequence;
  if (s <= last_seq.load(std::memory_order_relaxed)) {
    order_violations.fetch_add(1, std::memory_order_relaxed);
  }
  last_seq.store(s, std::memory_order_relaxed);
  got.fetch_add(1, std::memory_order_relaxed);
  if (Publish(forwarded_, *v.payload) != thyme::ChannelStatus::kOk) {
    forward_failures.fetch_add(1, std::memory_order_relaxed);
  }
}
]==])
file(WRITE "${ROOT}/src/nodes/relay.h" [==[#pragma once
#include "generated/relay_generated.h"
#include <atomic>
class RelayThread final : public RelayThreadGenerated {
 public:
  using RelayThreadGenerated::RelayThreadGenerated;
  std::atomic<uint32_t> got{0};
  std::atomic<uint32_t> last_seq{0};
  std::atomic<int> order_violations{0};
 protected:
  void OnForwarded(const decltype(forwarded_)::Sample& v) override;
};
]==])
file(WRITE "${ROOT}/src/nodes/relay.cpp" [==[#include "nodes/relay.h"
void RelayThread::OnForwarded(const decltype(forwarded_)::Sample& v) {
  const uint32_t s = v.sequence;
  if (s <= last_seq.load(std::memory_order_relaxed)) {
    order_violations.fetch_add(1, std::memory_order_relaxed);
  }
  last_seq.store(s, std::memory_order_relaxed);
  got.fetch_add(1, std::memory_order_relaxed);
}
]==])
file(WRITE "${ROOT}/src/main.cpp" [==[#include "generated/wiring.h"
#include "generated/mp_unknown.h"
#include "support/record_mirror_ctx.h"
#include "thyme/app/replay_mode.h"
#include "thyme/runtime/app.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>
int main() {
  const bool record = std::getenv("REC19_RECORD") != nullptr;
  const std::string mirror_name = record_mirror::DefaultShmName();
  if (record && !record_mirror::Create(mirror_name.c_str())) return 20;
  auto p = Pipeline::Create();
  p->Start();
  int rc = 0;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (p->relay.got.load() < 40 && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  if (p->prod.sent != 40) rc = 1;               // producer pushed all
  if (rc == 0 && p->sink.got.load() != 40) rc = 2;   // every message handled once
  if (rc == 0 && p->sink.order_violations.load() != 0) rc = 3;
  if (rc == 0 && p->sink.handled_count() != 40) rc = 4;  // message-granular loops
  if (rc == 0 && p->sink.dispatch_count() < 1) rc = 5;
  if (rc == 0 && p->sink.dispatch_count() > 40) rc = 6;  // batched, never per-nothing
  if (rc == 0 && p->sink.forward_failures.load() != 0) rc = 7;
  if (rc == 0 && p->relay.got.load() != 40) rc = 8;      // event->event hop lossless
  if (rc == 0 && p->relay.order_violations.load() != 0) rc = 9;
  if (rc == 0 && p->relay.handled_count() != 40) rc = 10;

  // Replay pause: with the sink paused, queued input must not busy-spin the
  // dispatcher — the re-dispatch rate stays bounded.
  if (rc == 0) {
    thyme::app::replay_mode::SetActive(true);
    thyme::app::replay_mode::SetFullInject(true);  // pauses every data producer, sink incl.
    for (uint32_t i = 1; i <= 4; ++i) {
      thyme::Frame<Reading> r{};
      thyme::FrameInit(r, 40 + i, 0, 0);
      r.payload.value = 40 + i;
      thyme::FrameSeal(r, 0, 0);  // sealed replay record accepted by Channel::TryRead
      while (!p->event_transports_.sink_readings_pipe.Push(r)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(200));  // spin up
    const uint64_t spins0 = p->sink.dispatch_count();
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    const uint64_t spins1 = p->sink.dispatch_count();
    if (p->sink.got.load() != 40) rc = 11;       // nothing handled while paused
    if (rc == 0 && spins1 - spins0 > 1000) rc = 12;  // bounded wait, no busy spin
    thyme::app::replay_mode::SetFullInject(false);
    thyme::app::replay_mode::SetActive(false);
  }

  // Resume: the queued messages drain and forward through the event chain.
  if (rc == 0) {
    const auto resume = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (p->relay.got.load() < 44 && std::chrono::steady_clock::now() < resume) {
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    if (p->sink.got.load() != 44) rc = 13;
    if (rc == 0 && p->relay.got.load() != 44) rc = 14;
    if (rc == 0 && p->sink.handled_count() != 44) rc = 15;
    if (rc == 0 && p->sink.order_violations.load() != 0) rc = 16;
    if (rc == 0 && p->sink.forward_failures.load() != 0) rc = 17;
    if (rc == 0 && p->sink.cb_work().count() != 44) rc = 18;
    if (rc == 0 && kRecordRegistryCount > 1 && p->relay.cb_work().count() != 44) rc = 19;
  }
  p->RequestStop();
  p->Join();
  if (record) {
    const thyme::RecordMirrorRingPage* page = record_mirror::Page();
    if (page == nullptr || page->struct_count() != kRecordRegistryCount) rc = 21;
    for (uint32_t sid = 0; page != nullptr && sid < kRecordRegistryCount; ++sid) {
      if (page->RingHead(static_cast<uint16_t>(sid)) != 44) rc = 22;
    }
    const uint64_t required = ThymeRecordMirrorRingRequiredBytes();
    if (page == nullptr || sizeof(thyme::RecordMirrorRingPage) + page->arena_bytes() != required) {
      rc = 23;
    }
    std::printf("record_required_bytes=%llu\n", static_cast<unsigned long long>(required));
    record_mirror::Shutdown(true);
  } else if (record_mirror::Page() != nullptr) {
    rc = 24;
  }
  return rc;
}
]==])

execute_process(COMMAND "${CMAKE_COMMAND}" -S "${ROOT}" -B "${BUILD}"
  -DTHYME_SOURCE_DIR=${THYME_ABS} -DTHYME_BUILD_TESTS=OFF RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "cmake configure failed")
endif()
execute_process(COMMAND "${CMAKE_COMMAND}" --build "${BUILD}" --parallel 2 RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "event fixture build failed")
endif()
execute_process(COMMAND "${BUILD}/app" RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "event fixture app exited ${rc}")
endif()
execute_process(COMMAND "${CMAKE_COMMAND}" -E env REC19_RECORD=1 "${BUILD}/app"
  RESULT_VARIABLE rc OUTPUT_VARIABLE record_out ERROR_VARIABLE record_err)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "event fixture record app exited ${rc}\n${record_out}${record_err}")
endif()
string(REGEX MATCH "record_required_bytes=([0-9]+)" _ "${record_out}")
set(record_all_bytes "${CMAKE_MATCH_1}")
if(record_all_bytes STREQUAL "")
  message(FATAL_ERROR "event fixture did not report its generated ring size\n${record_out}")
endif()

# REC-19: regenerate the same real app with forwarded unrecorded. The recorded
# readings path still works, forwarded has no spec/tap/slot, and the SHM arena
# shrinks by exactly the generated RequiredBytes difference.
set(ROOT_MIX "${CMAKE_BINARY_DIR}/event_mixed_record_app")
set(BUILD_MIX "${ROOT_MIX}/build")
file(REMOVE_RECURSE "${ROOT_MIX}")
file(COPY "${ROOT}/" DESTINATION "${ROOT_MIX}")
file(REMOVE_RECURSE "${BUILD_MIX}")
file(READ "${TPL}/graph.yaml" mixed_yaml)
string(REPLACE "    channel: forwarded\n" "    channel: forwarded\n    record: false\n" mixed_yaml "${mixed_yaml}")
set(MIXED_YAML "${CMAKE_BINARY_DIR}/event_mixed_record.yaml")
file(WRITE "${MIXED_YAML}" "${mixed_yaml}")
execute_process(COMMAND "${GEN}" --out "${ROOT_MIX}" --update generated
  --template-dir "${TPL}" --in "${MIXED_YAML}" --no-health-reporter
  RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "gen_graph (mixed record fixture) failed: ${rc}")
endif()
expect_contains("${ROOT_MIX}/generated/record_manifest.h" "kRecordManifestCount = 1")
expect_contains("${ROOT_MIX}/generated/wiring.cpp"
  "sink_readings_pipe.SetRecordTap(record_page")
file(READ "${ROOT_MIX}/generated/wiring.cpp" mixed_wiring)
string(FIND "${mixed_wiring}" "relay_forwarded_pipe.SetRecordTap" mixed_forwarded_tap)
if(NOT mixed_forwarded_tap EQUAL -1)
  message(FATAL_ERROR "record:false event edge must not emit SetRecordTap")
endif()
file(READ "${ROOT_MIX}/generated/record_registry.h" mixed_registry)
string(FIND "${mixed_registry}" "kForwarded" mixed_forwarded_id)
if(NOT mixed_forwarded_id EQUAL -1)
  message(FATAL_ERROR "record:false edge kept a RecordStructId")
endif()
file(WRITE "${ROOT_MIX}/src/nodes/sink.cpp" [==[#include "nodes/sink.h"
void SinkThread::OnReadings(const decltype(readings_)::Sample& v) {
  const uint32_t s = v.sequence;
  if (s <= last_seq.load(std::memory_order_relaxed)) {
    order_violations.fetch_add(1, std::memory_order_relaxed);
  }
  last_seq.store(s, std::memory_order_relaxed);
  got.fetch_add(1, std::memory_order_relaxed);
  if (Publish(forwarded_, *v.payload) != thyme::ChannelStatus::kOk) {
    forward_failures.fetch_add(1, std::memory_order_relaxed);
  }
}
]==])
execute_process(COMMAND "${CMAKE_COMMAND}" -S "${ROOT_MIX}" -B "${BUILD_MIX}"
  -DTHYME_SOURCE_DIR=${THYME_ABS} -DTHYME_BUILD_TESTS=OFF RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "mixed record cmake configure failed")
endif()
execute_process(COMMAND "${CMAKE_COMMAND}" --build "${BUILD_MIX}" --parallel 2 RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "mixed record fixture build failed")
endif()
execute_process(COMMAND "${CMAKE_COMMAND}" -E env REC19_RECORD=1 "${BUILD_MIX}/app"
  RESULT_VARIABLE rc OUTPUT_VARIABLE mixed_out ERROR_VARIABLE mixed_err)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "mixed record fixture app exited ${rc}\n${mixed_out}${mixed_err}")
endif()
string(REGEX MATCH "record_required_bytes=([0-9]+)" _ "${mixed_out}")
set(record_mixed_bytes "${CMAKE_MATCH_1}")
if(record_mixed_bytes STREQUAL "" OR NOT record_mixed_bytes LESS record_all_bytes)
  message(FATAL_ERROR
    "record:false did not shrink ring bytes: all=${record_all_bytes} mixed=${record_mixed_bytes}")
endif()
