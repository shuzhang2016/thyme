if(NOT GEN OR NOT THYME)
  message(FATAL_ERROR "GEN and THYME required")
endif()
get_filename_component(THYME_ABS "${THYME}" ABSOLUTE)
set(TPL "${THYME_ABS}/tools/gen/gen_graph/testdata/pinned_slot_tpl")
set(ROOT "${CMAKE_BINARY_DIR}/pinned_slot_app_test")
set(BUILD "${ROOT}/build")
file(REMOVE_RECURSE "${ROOT}")

execute_process(COMMAND "${GEN}" --out "${ROOT}" --update all
  --template-dir "${TPL}" --in "${TPL}/graph.yaml"
  RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "gen_graph (PinnedSlot fixture) failed: ${rc}")
endif()

function(expect_contains file needle)
  file(READ "${file}" body)
  string(FIND "${body}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "PinnedSlot: ${file} missing expected content: ${needle}")
  endif()
endfunction()

function(expect_not_contains file needle)
  file(READ "${file}" body)
  string(FIND "${body}" "${needle}" pos)
  if(NOT pos EQUAL -1)
    message(FATAL_ERROR "PinnedSlot: ${file} contains unexpected SPMC wiring: ${needle}")
  endif()
endfunction()

expect_not_contains("${ROOT}/generated/mp_unknown.h" "SpmcReader<thyme::Frame<Tick>")
expect_contains("${ROOT}/generated/channel_capabilities.json" "\"transport_kind\": \"pinned_slot\"")
expect_contains("${ROOT}/generated/trigger.base.ini" "[channel./c9_%pid_tick]")
expect_contains("${ROOT}/generated/mp_unknown.h" "OpenEventShmChannels")
expect_contains("${ROOT}/generated/mp_unknown.h" "PrepareEventShmChannels")
expect_contains("${ROOT}/generated/mp_unknown.h" "AddWakeWord")
expect_contains("${ROOT}/generated/ev_generated.h" "ChannelStatus::kOk")
expect_contains("${ROOT}/generated/cyc_generated.h" "const uint8_t* tick = nullptr;")
expect_contains("${ROOT}/generated/cyc_generated.h" "uint32_t size_tick = 0;")
expect_contains("${ROOT}/generated/cyc_generated.h" "pinned_tick_follow = std::move(next);")
expect_contains("${ROOT}/generated/cyc_generated.h" "pinned_tick.Reset();")
expect_contains("${ROOT}/generated/cyc_generated.h" "pinned_tick_follow.Reset();")
expect_contains("${ROOT}/generated/prod_generated.h" "PrepareOutput(in, &request);")
expect_contains("${ROOT}/generated/prod_generated.h" "tick_w_.BeginWriteAt(request.size_tick, 0)")

file(WRITE "${ROOT}/src/nodes/prod.cpp" [==[#include "nodes/prod.h"
#include <cstdlib>
bool ProdThread::OnTick(const TickIn&, TickOut* out) {
  Tick value{};
  value.value = static_cast<uint32_t>(loop_count() + 1);
  value.payload[0] = static_cast<uint8_t>(value.value);
  if (out->tick != nullptr) {
    if (out->size_tick != sizeof(Tick)) std::abort();
    *reinterpret_cast<Tick*>(out->tick) = value;
  }
  if (out->tick_follow != nullptr) {
    if (out->size_tick_follow != sizeof(Tick)) std::abort();
    *reinterpret_cast<Tick*>(out->tick_follow) = value;
  }
  if (out->tick_copy != nullptr) {
    if (out->size_tick_copy != sizeof(Tick)) std::abort();
    *reinterpret_cast<Tick*>(out->tick_copy) = value;
  }
  return true;
}
]==])
file(WRITE "${ROOT}/src/nodes/cyc.cpp" [==[#include "nodes/cyc.h"
#include <cstdlib>
bool CycThread::OnTick(const TickIn& in, TickOut*) {
  const auto check = [](const uint8_t* data, uint32_t size) {
    if (data == nullptr) return;
    if (size != sizeof(Tick)) std::abort();
    const Tick* tick = reinterpret_cast<const Tick*>(data);
    if (tick->value == 0 || tick->payload[0] != static_cast<uint8_t>(tick->value)) {
      std::abort();
    }
  };
  check(in.tick, in.size_tick);
  check(in.tick_follow, in.size_tick_follow);
  return true;
}
]==])
file(WRITE "${ROOT}/src/nodes/ev.cpp" [==[#include "nodes/ev.h"
#include <cstdlib>
void EvThread::OnTick(const decltype(tick_)::Sample& sample) {
  if (!sample.still_valid() || reinterpret_cast<const Tick*>(sample.data)->value == 0 ||
      reinterpret_cast<const Tick*>(sample.data)->payload[0] != static_cast<uint8_t>(reinterpret_cast<const Tick*>(sample.data)->value)) {
    std::abort();
  }
}
void EvThread::OnTickCopy(const decltype(tick_copy_)::Sample& sample) {
  if (!sample.still_valid() || reinterpret_cast<const Tick*>(sample.data)->value == 0 ||
      reinterpret_cast<const Tick*>(sample.data)->payload[0] != static_cast<uint8_t>(reinterpret_cast<const Tick*>(sample.data)->value)) {
    std::abort();
  }
}
]==])

execute_process(COMMAND "${CMAKE_COMMAND}" -S "${ROOT}" -B "${BUILD}"
  -DTHYME_SOURCE_DIR=${THYME_ABS} -DTHYME_BUILD_TESTS=OFF RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "PinnedSlot app configure failed")
endif()
execute_process(COMMAND "${CMAKE_COMMAND}" --build "${BUILD}" --target app --parallel 2
  RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "PinnedSlot app build failed")
endif()

execute_process(COMMAND "${BUILD}/app" --seconds 1
  RESULT_VARIABLE rc OUTPUT_VARIABLE sp_out ERROR_VARIABLE sp_err)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "PinnedSlot SP exited ${rc}: ${sp_out}\n${sp_err}")
endif()
if(NOT sp_out MATCHES "ev[ ]+handled=[1-9][0-9]*")
  message(FATAL_ERROR "PinnedSlot SP delivered no event payload: ${sp_out}\n${sp_err}")
endif()

execute_process(COMMAND "${BUILD}/app" --mp --seconds 1
  RESULT_VARIABLE rc OUTPUT_VARIABLE mp_out ERROR_VARIABLE mp_err)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "PinnedSlot MP exited ${rc}: ${mp_out}\n${mp_err}")
endif()
if(mp_out MATCHES "err=[1-9][0-9]*")
  message(FATAL_ERROR "PinnedSlot MP reported a payload/publish error: ${mp_out}\n${mp_err}")
endif()
if(NOT mp_out MATCHES "health\\[2\\] alive=[1-9][0-9]*")
  message(FATAL_ERROR "PinnedSlot MP event role did not consume payloads: ${mp_out}\n${mp_err}")
endif()

# Variable-size SHM output: PrepareOutput runs before BeginWriteAt. The local
# PinnedSlot backend is fixed-size by design, so this variant is MP-only.
file(WRITE "${ROOT}/src/nodes/prod.h" [==[#pragma once
#include "generated/prod_generated.h"
class ProdThread final : public ProdThreadGenerated {
 public:
  using ProdThreadGenerated::ProdThreadGenerated;
 protected:
  void PrepareOutput(const TickIn& in, TickRequest* request) override;
  bool OnTick(const TickIn& in, TickOut* out) override;
};
]==])
file(WRITE "${ROOT}/src/nodes/prod.cpp" [==[#include "nodes/prod.h"
#include <cstdlib>
void ProdThread::PrepareOutput(const TickIn&, TickRequest* request) {
  request->size_tick_copy = sizeof(Tick) - 1;
}
bool ProdThread::OnTick(const TickIn&, TickOut* out) {
  Tick value{};
  value.value = static_cast<uint32_t>(loop_count() + 1);
  value.payload[0] = static_cast<uint8_t>(value.value);
  if (out->tick != nullptr) *reinterpret_cast<Tick*>(out->tick) = value;
  if (out->tick_follow != nullptr) *reinterpret_cast<Tick*>(out->tick_follow) = value;
  if (out->tick_copy != nullptr) {
    if (out->size_tick_copy != sizeof(Tick) - 1) std::abort();
    *reinterpret_cast<Tick*>(out->tick_copy) = value;
  }
  return true;
}
]==])
file(WRITE "${ROOT}/src/nodes/ev.cpp" [==[#include "nodes/ev.h"
#include <cstdlib>
void EvThread::OnTick(const decltype(tick_)::Sample& sample) {
  if (!sample.still_valid() || sample.size != sizeof(Tick)) std::abort();
}
void EvThread::OnTickCopy(const decltype(tick_copy_)::Sample& sample) {
  if (!sample.still_valid() || sample.size != sizeof(Tick) - 1) std::abort();
  const Tick* tick = reinterpret_cast<const Tick*>(sample.data);
  if (tick->value == 0 || tick->payload[0] != static_cast<uint8_t>(tick->value)) {
    std::abort();
  }
}
]==])
execute_process(COMMAND "${CMAKE_COMMAND}" --build "${BUILD}" --target app --parallel 2
  RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "PinnedSlot variable app build failed")
endif()
execute_process(COMMAND "${BUILD}/app" --mp --seconds 1
  RESULT_VARIABLE rc OUTPUT_VARIABLE variable_out ERROR_VARIABLE variable_err)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "PinnedSlot variable MP exited ${rc}: ${variable_out}\n${variable_err}")
endif()
if(variable_out MATCHES "err=[1-9][0-9]*")
  message(FATAL_ERROR "PinnedSlot variable MP reported an error: ${variable_out}\n${variable_err}")
endif()
