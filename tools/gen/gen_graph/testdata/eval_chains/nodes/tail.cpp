#include "nodes/tail.h"

#include "thyme/app/time.h"

void TailThread::OnLoop() {
  uint32_t idx = 0;
  uint32_t gen = 0;
  if (!hop2_r_->try_pop(idx, gen)) return;
  const thyme::Frame<Reading>& in = hop2_->at(idx);
  const uint64_t now = thyme::app::NowMonoNs();
  const uint64_t lat = now >= in.hdr.mono_ns ? now - in.hdr.mono_ns : 0;
  NoteInFrame(in.hdr.sequence & 0xF);
  NoteLatency(lat);
  NoteLatencyEdge(static_cast<uint8_t>(RecordStructId::kHop2), lat,
                  static_cast<uint8_t>(in.hdr.sequence & 0xF));
  hop2_->release(idx, gen);
}
