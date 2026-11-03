#pragma once

#include "thyme/runtime/notif_pipe.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace thyme {
namespace app {
namespace replay_mode {

// Global replay-mode switches read by generated node code
// (ReplayProducerShouldPause etc.) and written by the replay runner.
void SetActive(bool on);
bool Active();

// When true, replay injects every recorded topic and pauses all data
// producers.
void SetFullInject(bool on);
bool FullInject();

// When non-null, replay injects topics for this consumer stage
// (--inject-stage). SINGLE WRITER: call from the replay runner thread only;
// readers (InjectStage) copy under a seqlock and never observe a torn name.
void SetInjectStage(const char* stage);
const char* InjectStage();

// M6.1: cooperative pause — no inject, consumers should not drain (M6.2).
void SetPaused(bool on);
bool Paused();

// M6.1: abort PushOrWait waits and stop a replay inject loop.
void RequestStop();
void ClearStop();
bool StopRequested();

// PushWaitGate.aborted callback for replay injectors (NotifPipe::PushOrWait).
bool PushWaitAborted(void* ctx);

// Default gate for replay-side PushOrWait (checks StopRequested / !Active).
PushWaitGate MakePushWaitGate(uint64_t slice_ns = 100000000ULL);

}  // namespace replay_mode
}  // namespace app
}  // namespace thyme
