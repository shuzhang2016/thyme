# GEN-15: one cyclic producer publishes through raw pool/IndexPipe latest,
# FrameChannelRef event queue, and FrameChannelRef broadcast in one tick.
if(NOT GEN OR NOT THYME)
  message(FATAL_ERROR "GEN and THYME required")
endif()
get_filename_component(THYME_ABS "${THYME}" ABSOLUTE)
set(SRC_TPL "${THYME_ABS}/tools/gen/gen_graph/testdata/layout_tpl")
set(TPL "${CMAKE_BINARY_DIR}/tick_transports_tpl")
set(ROOT "${CMAKE_BINARY_DIR}/tick_transports_app")
set(BUILD "${ROOT}/build")
file(REMOVE_RECURSE "${ROOT}" "${TPL}")
file(COPY "${SRC_TPL}/" DESTINATION "${TPL}")
file(READ "${TPL}/graph.yaml" graph)
string(REPLACE "cpp_type: Reading" "cpp_type: Reading\n    record: false" graph "${graph}")
file(WRITE "${TPL}/graph.yaml" "${graph}")

execute_process(COMMAND "${GEN}" --out "${ROOT}" --update all
  --template-dir "${TPL}" --in "${TPL}/graph.yaml" --no-health-reporter
  RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "tick transports generation failed: ${rc}")
endif()

function(expect_contains file needle)
  file(READ "${file}" body)
  string(FIND "${body}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "tick transports: ${file} missing: ${needle}")
  endif()
endfunction()

expect_contains("${ROOT}/generated/prod_generated.h" "latest_out_r_->push_latest")
expect_contains("${ROOT}/generated/prod_generated.h" "queue_out_.CommitWrite(&out_queue_out)")
expect_contains("${ROOT}/generated/prod_generated.h" "bcast_w_.CommitWrite(&out_bcast)")
expect_contains("${ROOT}/generated/cons_generated.h" "latest_out_r_->try_pop(next_index, next_generation)")
# OBS-15: every tick input carries skipped_<name>; queue/IndexPipe drains count
# the older entries they release, and TickIn is populated from that counter.
expect_contains("${ROOT}/generated/cons_generated.h" "uint32_t skipped_")
expect_contains("${ROOT}/generated/cons_generated.h" "++skipped_")
expect_contains("${ROOT}/generated/cons_generated.h" "in.skipped_")

file(WRITE "${ROOT}/src/nodes/prod.cpp" [==[#include "nodes/prod.h"
#include "thyme/app/time.h"
bool ProducerThread::OnTick(const TickIn&, TickOut* out) {
  const uint32_t sequence = static_cast<uint32_t>(loop_count() + 1);
  if (out->latest_out != nullptr) {
    thyme::FrameInit(*out->latest_out, sequence, thyme::app::NowMonoNs(), 0);
    out->latest_out->payload.value = sequence;
    thyme::FrameSeal(*out->latest_out, 0, 0);
  }
  if (out->queue_out != nullptr) out->queue_out->value = sequence;
  if (out->bcast != nullptr) out->bcast->value = sequence;
  return true;
}
]==])
file(WRITE "${ROOT}/src/nodes/cons.cpp" [==[#include "nodes/cons.h"
#include <cstdio>
#include <cstdlib>
bool ConsumerThread::OnTick(const TickIn& in, TickOut*) {
  if (in.latest_out == nullptr) return true;
  if (!thyme::FrameVerify(*in.latest_out, 0, 0) ||
      in.latest_out->payload.value != in.latest_out->hdr.sequence) {
    std::abort();
  }
  static bool first = true;
  if (first) {
    std::printf("cons value=%u\n", in.latest_out->payload.value);
    first = false;
  }
  return true;
}
]==])
file(WRITE "${ROOT}/src/nodes/sink.cpp" [==[#include "nodes/sink.h"
#include <cstdio>
#include <cstdlib>
void SinkThread::OnQueueOut(const decltype(queue_out_)::Sample& sample) {
  if (sample.payload == nullptr || sample.payload->value != sample.sequence) std::abort();
  static bool first = true;
  if (first) {
    std::printf("sink value=%u\n", sample.payload->value);
    first = false;
  }
}
]==])
file(WRITE "${ROOT}/src/nodes/br.cpp" [==[#include "nodes/br.h"
#include <cstdio>
#include <cstdlib>
void BrThread::OnBcast(const decltype(bcast_)::Sample& sample) {
  if (sample.payload == nullptr || sample.payload->value != sample.sequence) std::abort();
  static bool first = true;
  if (first) {
    std::printf("br value=%u\n", sample.payload->value);
    first = false;
  }
}
]==])

execute_process(COMMAND "${CMAKE_COMMAND}" -S "${ROOT}" -B "${BUILD}"
  -DTHYME_SOURCE_DIR=${THYME_ABS} RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "tick transports configure failed")
endif()
execute_process(COMMAND "${CMAKE_COMMAND}" --build "${BUILD}" --target app --parallel 4
  RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "tick transports build failed")
endif()
execute_process(COMMAND "${BUILD}/app" --mp --seconds 1
  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "tick transports MP exited ${rc}: ${out}\n${err}")
endif()
foreach(expected "cons value=" "sink value=" "br value=")
  string(FIND "${out}" "${expected}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "tick transports missing ${expected}: ${out}\n${err}")
  endif()
endforeach()
if(out MATCHES "err=[1-9][0-9]*")
  message(FATAL_ERROR "tick transports reported a runtime error: ${out}\n${err}")
endif()
