#include "nodes/head.h"

#include "thyme/app/time.h"
#include "thyme/contracts/frame.h"

void HeadThread::OnLoop() {
  thyme::Frame<Reading> v{};
  thyme::FrameInit(v, ++seq_, thyme::app::NowMonoNs(), 0);
  v.payload.value = seq_;
  thyme::FrameMarkValid(v);

  const uint32_t idx = hop1_->acquire();
  if (idx == Hop1Pool::kInvalidIndex) {
    my_health_->AddDrops(1);
    return;
  }
  hop1_->at(idx) = v;
  const uint32_t gen = hop1_->generation(idx);
  hop1_->publish(idx);
  if (!hop1_r_->try_push(idx, gen)) {
    hop1_->release(idx, gen);
    my_health_->AddDrops(1);
    return;
  }
  NoteOutFrame(seq_ & 0xF);
}
