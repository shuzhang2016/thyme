#include "nodes/mid.h"

#include "thyme/app/time.h"

void MidThread::OnLoop() {
  uint32_t idx = 0;
  uint32_t gen = 0;
  if (!hop1_r_->try_pop(idx, gen)) return;

  const thyme::Frame<Reading>& in = hop1_->at(idx);
  const uint64_t now = thyme::app::NowMonoNs();
  const uint64_t lat = now >= in.hdr.mono_ns ? now - in.hdr.mono_ns : 0;
  NoteInFrame(in.hdr.sequence & 0xF);
  NoteLatency(lat);
  NoteLatencyEdge(static_cast<uint8_t>(RecordStructId::kHop1), lat,
                  static_cast<uint8_t>(in.hdr.sequence & 0xF));

  bool forwarded = false;
  const uint32_t out_idx = hop2_->acquire();
  if (out_idx != Hop2Pool::kInvalidIndex) {
    hop2_->at(out_idx) = in;
    const uint32_t out_gen = hop2_->generation(out_idx);
    hop2_->publish(out_idx);
    if (hop2_r_->try_push(out_idx, out_gen)) {
      NoteOutFrame(in.hdr.sequence & 0xF);
      forwarded = true;
    } else {
      hop2_->release(out_idx, out_gen);
    }
  }
  hop1_->release(idx, gen);
  if (!forwarded) my_health_->AddDrops(1);
}
