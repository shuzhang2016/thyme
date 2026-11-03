# Build-and-run smoke for delivery: broadcast (1 producer + 2 consumer processes).
# yaml+runtime stay on mr_gate; this fixture needs SHM + compile (timeout 180).
if(NOT GEN OR NOT THYME)
  message(FATAL_ERROR "GEN and THYME required")
endif()
get_filename_component(THYME_ABS "${THYME}" ABSOLUTE)
set(TPL "${THYME_ABS}/tools/gen/gen_graph/testdata/broadcast_tpl")
set(ROOT "${CMAKE_BINARY_DIR}/broadcast_app")
set(BUILD "${ROOT}/build")
file(REMOVE_RECURSE "${ROOT}")

execute_process(COMMAND "${GEN}" --out "${ROOT}" --update all
  --template-dir "${TPL}" --in "${TPL}/graph.yaml" --no-health-reporter
  RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "gen_graph (broadcast fixture) failed: ${rc}")
endif()

function(expect_contains file needle)
  file(READ "${file}" body)
  string(FIND "${body}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "broadcast: ${file} missing expected content: ${needle}")
  endif()
endfunction()

function(expect_missing file needle)
  file(READ "${file}" body)
  string(FIND "${body}" "${needle}" pos)
  if(NOT pos EQUAL -1)
    message(FATAL_ERROR "broadcast: ${file} must not contain: ${needle}")
  endif()
endfunction()

expect_contains("${ROOT}/generated/graph_ir.h" "THYME_GRAPH_HAS_BROADCAST")
expect_contains("${ROOT}/generated/cyc_generated.h" "FrameChannelRef")
expect_contains("${ROOT}/generated/cyc_generated.h" "latest_tick_copy = std::move(next);")
expect_contains("${ROOT}/generated/ev_generated.h" "void Dispatch() override final")
expect_contains("${ROOT}/generated/ev_generated.h" "while (tick_.TryRead(&v) == thyme::ChannelStatus::kOk)")
expect_missing("${ROOT}/generated/cyc_generated.h" "TryPop")
expect_missing("${ROOT}/generated/cyc_generated.h" "ReadLatest")
file(READ "${ROOT}/generated/cyc_generated.h" cyc_body)
string(FIND "${cyc_body}" "void Loop() override final" loop_pos)
string(FIND "${cyc_body}" "OnTick(in, &out);" onloop_pos)
if(loop_pos EQUAL -1 OR onloop_pos EQUAL -1 OR NOT loop_pos LESS onloop_pos)
  message(FATAL_ERROR "broadcast: cyclic Loop() must call OnTick()")
endif()

expect_contains("${ROOT}/generated/prod_generated.h" "bool OnTick(const TickIn& in, TickOut* out) = 0")
expect_contains("${ROOT}/generated/prod_generated.h" "tick_w_.CommitWrite(&out_tick)")
expect_contains("${ROOT}/generated/ev_generated.h" "while (tick_copy_.TryRead(&v) == thyme::ChannelStatus::kOk)")

# Publish identical payloads through the copy path and the generated facade.
file(WRITE "${ROOT}/src/nodes/prod.cpp" [==[#include "nodes/prod.h"
bool ProdThread::OnTick(const TickIn&, TickOut* out) {
  Tick value{};
  value.value = static_cast<uint32_t>(loop_count() + 1);
  if (out->tick != nullptr) *out->tick = value;
  if (out->tick_copy != nullptr) *out->tick_copy = value;
  return true;
}
]==])
file(WRITE "${ROOT}/src/nodes/cyc.cpp" [==[#include "nodes/cyc.h"
bool CycThread::OnTick(const TickIn& in, TickOut*) {
  if (in.tick == nullptr || in.tick->payload.value != in.tick->hdr.sequence) {
    my_health_->SetError(1);
  }
  if (in.tick_copy != nullptr &&
      in.tick_copy->payload.value != in.tick_copy->hdr.sequence) {
    my_health_->SetError(1);
  }
  return true;
}
]==])
file(WRITE "${ROOT}/src/main.cpp" [==[#include "generated/wiring.h"
#include "mp/mp_entry.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <unistd.h>
int main(int argc, char** argv) {
  if (argc > 1 && std::strcmp(argv[1], "--mp") == 0) {
    return RunSupervisor(argv[0], 1, false, nullptr, nullptr);
  }
  const char* role = nullptr;
  const char* prefix = nullptr;
  int proc_idx = -1;
  for (int i = 1; i + 1 < argc; ++i) {
    if (std::strcmp(argv[i], "--role") == 0) role = argv[i + 1];
    if (std::strcmp(argv[i], "--shm-prefix") == 0) prefix = argv[i + 1];
    if (std::strcmp(argv[i], "--proc-idx") == 0) proc_idx = std::atoi(argv[i + 1]);
  }
  if (role != nullptr && prefix != nullptr) {
    return RunRole(prefix, role, nullptr, proc_idx);
  }
  char name_a[64];
  char name_b[64];
  std::snprintf(name_a, sizeof(name_a), "/c9_helper_%d", static_cast<int>(getpid()));
  std::snprintf(name_b, sizeof(name_b), "/c9_copy_%d", static_cast<int>(getpid()));
  thyme::ShmRegion::UnlinkName(name_a);
  thyme::ShmRegion::UnlinkName(name_b);
  int rc = 0;
  {
    thyme::ShmSpmcRing<thyme::Frame<Tick>, 8> in_place;
    thyme::ShmSpmcRing<thyme::Frame<Tick>, 8> copy;
    if (!in_place.Create(name_a) || !copy.Create(name_b)) return 1;
    thyme::HealthSlot health{};
    ProdThread prod(ProdPorts{&in_place, &copy}, MpContext{&health});
    thyme::SpmcReader<thyme::Frame<Tick>, 8> reader_a;
    thyme::SpmcReader<thyme::Frame<Tick>, 8> reader_b;
    if (!reader_a.Bind(&in_place, thyme::SpmcRead::Follow) ||
        !reader_b.Bind(&copy, thyme::SpmcRead::Follow)) return 2;
    if (!prod.Start()) return 3;
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
    prod.RequestStop();
    prod.Join();
    bool any = false;
    for (;;) {
      thyme::Frame<Tick> a{};
      thyme::Frame<Tick> b{};
      const bool got_a = reader_a.TryPop(a);
      const bool got_b = reader_b.TryPop(b);
      if (got_a != got_b) { rc = 4; break; }
      if (!got_a) break;
      any = true;
      if (a.payload.value != b.payload.value || a.hdr.sequence != b.hdr.sequence) {
        rc = 5;
        break;
      }
    }
    if (!any) rc = 6;
    in_place.Close();
    copy.Close();
  }
  thyme::ShmRegion::UnlinkName(name_a);
  thyme::ShmRegion::UnlinkName(name_b);
  return rc;
}
]==])

execute_process(COMMAND "${CMAKE_COMMAND}" -S "${ROOT}" -B "${BUILD}"
  -DTHYME_SOURCE_DIR=${THYME_ABS} RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "broadcast app configure failed")
endif()
# Build only `app` so Darwin is not blocked by scaffold eval_chains (timerfd).
execute_process(COMMAND "${CMAKE_COMMAND}" --build "${BUILD}" --target app --parallel 4
  RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "broadcast app build failed")
endif()

set(PERF_PREFIX "${CMAKE_BINARY_DIR}/broadcast_perf")
file(GLOB OLD_PERF "${PERF_PREFIX}_*.bin")
if(OLD_PERF)
  file(REMOVE ${OLD_PERF})
endif()
execute_process(COMMAND "${CMAKE_COMMAND}" -E env "T9_PERF_LOG=${PERF_PREFIX}"
  "${BUILD}/app" --mp --seconds 1 RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "broadcast app --mp exited ${rc}")
endif()
file(GLOB CBWORK_LOGS "${PERF_PREFIX}_cbwork_*.bin")
set(SAW_CBWORK_SAMPLES FALSE)
foreach(LOG IN LISTS CBWORK_LOGS)
  file(SIZE "${LOG}" LOG_SIZE)
  if(LOG_SIZE GREATER 128)
    set(SAW_CBWORK_SAMPLES TRUE)
  endif()
endforeach()
if(NOT SAW_CBWORK_SAMPLES)
  message(FATAL_ERROR "broadcast MP did not flush callback-work samples")
endif()
execute_process(COMMAND "${BUILD}/app" RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "broadcast SP copy/in-place equivalence exited ${rc}")
endif()
