#include "thyme/replay/replay_session.h"

#include <chrono>
#include <cmath>
#include <utility>

namespace thyme {
namespace replay {

namespace {

// Long waits are sliced so a pause/stop/rate change is observed promptly even
// if a notification were lost; the slice is short enough to stay below one
// typical frame period.
constexpr uint64_t kMaxWaitSliceNs = 10000000ull;  // 10 ms
constexpr std::chrono::milliseconds kPausePoll(50);

}  // namespace

const char* ReplaySessionStateName(ReplaySessionState state) {
  switch (state) {
    case ReplaySessionState::kClosed:
      return "closed";
    case ReplaySessionState::kPlaying:
      return "playing";
    case ReplaySessionState::kPaused:
      return "paused";
    case ReplaySessionState::kEnded:
      return "ended";
    case ReplaySessionState::kStopped:
      return "stopped";
  }
  return "unknown";
}

ReplaySession::ReplaySession() = default;

ReplaySession::~ReplaySession() = default;

ReplayStatus ReplaySession::Open(const std::string& dir) {
  std::lock_guard<std::mutex> lock(mu_);
  stop_.store(false, std::memory_order_relaxed);
  paused_ = false;
  as_fast_ = false;
  rate_ = 1.0;
  rate_generation_ += 1;
  first_event_ = true;
  last_mono_ns_ = 0;
  delivered_ = 0;
  const ReplayStatus st = package_.Open(dir);
  if (st != ReplayStatus::kOk) {
    open_ = false;
    status_.store(st, std::memory_order_relaxed);
    state_.store(ReplaySessionState::kClosed, std::memory_order_relaxed);
    return st;
  }
  open_ = true;
  status_.store(ReplayStatus::kOk, std::memory_order_relaxed);
  state_.store(ReplaySessionState::kPlaying, std::memory_order_relaxed);
  return ReplayStatus::kOk;
}

void ReplaySession::Close() {
  std::lock_guard<std::mutex> lock(mu_);
  package_.Close();
  open_ = false;
  paused_ = false;
  state_.store(ReplaySessionState::kClosed, std::memory_order_relaxed);
}

ReplayStatus ReplaySession::NextEvent(ReplayEvent* out) {
  std::unique_lock<std::mutex> lock(mu_);
  if (out == nullptr || !open_) {
    status_.store(ReplayStatus::kInvalidArgument, std::memory_order_relaxed);
    return ReplayStatus::kInvalidArgument;
  }
  if (stop_.load(std::memory_order_relaxed)) {
    status_.store(ReplayStatus::kStopped, std::memory_order_relaxed);
    state_.store(ReplaySessionState::kStopped, std::memory_order_relaxed);
    return ReplayStatus::kStopped;
  }
  while (paused_ && !stop_.load(std::memory_order_relaxed)) {
    state_.store(ReplaySessionState::kPaused, std::memory_order_relaxed);
    cv_.wait_for(lock, kPausePoll);
  }
  if (stop_.load(std::memory_order_relaxed)) {
    status_.store(ReplayStatus::kStopped, std::memory_order_relaxed);
    state_.store(ReplaySessionState::kStopped, std::memory_order_relaxed);
    return ReplayStatus::kStopped;
  }

  ReplayEvent ev;
  const ReplayStatus st = package_.NextEvent(&ev);
  if (st != ReplayStatus::kOk) {
    status_.store(st, std::memory_order_relaxed);
    state_.store(st == ReplayStatus::kEnd ? ReplaySessionState::kEnded
                                          : ReplaySessionState::kPlaying,
                 std::memory_order_relaxed);
    return st;
  }

  if (!first_event_ && !as_fast_ && ev.mono_ns > last_mono_ns_) {
    // Pace on the mono_ns delta: pause adds wall time (it does not compress
    // the timeline) and a rate change applies to the remaining delta.
    double remaining_mono = static_cast<double>(ev.mono_ns - last_mono_ns_);
    while (remaining_mono > 0.0) {
      if (as_fast_) {
        break;
      }
      if (stop_.load(std::memory_order_relaxed)) {
        status_.store(ReplayStatus::kStopped, std::memory_order_relaxed);
        state_.store(ReplaySessionState::kStopped, std::memory_order_relaxed);
        return ReplayStatus::kStopped;
      }
      while (paused_ && !stop_.load(std::memory_order_relaxed)) {
        state_.store(ReplaySessionState::kPaused, std::memory_order_relaxed);
        cv_.wait_for(lock, kPausePoll);
      }
      if (stop_.load(std::memory_order_relaxed)) {
        status_.store(ReplayStatus::kStopped, std::memory_order_relaxed);
        state_.store(ReplaySessionState::kStopped, std::memory_order_relaxed);
        return ReplayStatus::kStopped;
      }
      const double rate = rate_;
      const double wall_ns = remaining_mono / rate;
      if (!(wall_ns >= 1.0)) {
        break;
      }
      const uint64_t slice_ns = wall_ns > static_cast<double>(kMaxWaitSliceNs)
                                    ? kMaxWaitSliceNs
                                    : static_cast<uint64_t>(wall_ns);
      const uint32_t generation = rate_generation_;
      const auto begin = std::chrono::steady_clock::now();
      cv_.wait_for(lock, std::chrono::nanoseconds(slice_ns), [this, generation] {
        return stop_.load(std::memory_order_relaxed) || paused_ ||
               rate_generation_ != generation;
      });
      const auto elapsed = std::chrono::steady_clock::now() - begin;
      const double elapsed_ns = static_cast<double>(
          std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count());
      remaining_mono -= elapsed_ns * rate;
    }
  }

  last_mono_ns_ = ev.mono_ns;
  first_event_ = false;
  delivered_ += 1;
  *out = ev;
  status_.store(ReplayStatus::kOk, std::memory_order_relaxed);
  state_.store(paused_ ? ReplaySessionState::kPaused
                       : ReplaySessionState::kPlaying,
               std::memory_order_relaxed);
  return ReplayStatus::kOk;
}

ReplayStatus ReplaySession::Step(ReplayEvent* out) {
  std::lock_guard<std::mutex> lock(mu_);
  if (out == nullptr || !open_) {
    status_.store(ReplayStatus::kInvalidArgument, std::memory_order_relaxed);
    return ReplayStatus::kInvalidArgument;
  }
  if (stop_.load(std::memory_order_relaxed)) {
    status_.store(ReplayStatus::kStopped, std::memory_order_relaxed);
    state_.store(ReplaySessionState::kStopped, std::memory_order_relaxed);
    return ReplayStatus::kStopped;
  }
  ReplayEvent ev;
  const ReplayStatus st = package_.NextEvent(&ev);
  if (st != ReplayStatus::kOk) {
    status_.store(st, std::memory_order_relaxed);
    state_.store(st == ReplayStatus::kEnd ? ReplaySessionState::kEnded
                                          : ReplaySessionState::kPaused,
                 std::memory_order_relaxed);
    return st;
  }
  last_mono_ns_ = ev.mono_ns;
  first_event_ = false;
  delivered_ += 1;
  paused_ = true;  // stepping implies a paused session
  *out = ev;
  status_.store(ReplayStatus::kOk, std::memory_order_relaxed);
  state_.store(ReplaySessionState::kPaused, std::memory_order_relaxed);
  return ReplayStatus::kOk;
}

ReplayStatus ReplaySession::Seek(uint64_t mono_ns) {
  std::lock_guard<std::mutex> lock(mu_);
  if (!open_) {
    status_.store(ReplayStatus::kInvalidArgument, std::memory_order_relaxed);
    return ReplayStatus::kInvalidArgument;
  }
  const ReplayStatus st = package_.SeekMonoNs(mono_ns);
  if (st != ReplayStatus::kOk) {
    status_.store(st, std::memory_order_relaxed);
    return st;
  }
  first_event_ = true;
  last_mono_ns_ = 0;
  status_.store(ReplayStatus::kOk, std::memory_order_relaxed);
  state_.store(paused_ ? ReplaySessionState::kPaused
                       : ReplaySessionState::kPlaying,
               std::memory_order_relaxed);
  return ReplayStatus::kOk;
}

ReplayStatus ReplaySession::SetRate(double rate) {
  if (!(rate > 0.0) || !std::isfinite(rate)) {
    status_.store(ReplayStatus::kInvalidArgument, std::memory_order_relaxed);
    return ReplayStatus::kInvalidArgument;
  }
  std::lock_guard<std::mutex> lock(mu_);
  rate_ = rate;
  rate_generation_ += 1;
  cv_.notify_all();
  return ReplayStatus::kOk;
}

double ReplaySession::rate() const {
  std::lock_guard<std::mutex> lock(mu_);
  return rate_;
}

void ReplaySession::Pause() {
  std::lock_guard<std::mutex> lock(mu_);
  paused_ = true;
  const ReplaySessionState s = state_.load(std::memory_order_relaxed);
  if (open_ && (s == ReplaySessionState::kPlaying ||
                s == ReplaySessionState::kPaused)) {
    state_.store(ReplaySessionState::kPaused, std::memory_order_relaxed);
  }
  cv_.notify_all();
}

void ReplaySession::Resume() {
  std::lock_guard<std::mutex> lock(mu_);
  paused_ = false;
  if (open_ && state_.load(std::memory_order_relaxed) ==
                   ReplaySessionState::kPaused) {
    state_.store(ReplaySessionState::kPlaying, std::memory_order_relaxed);
  }
  cv_.notify_all();
}

bool ReplaySession::paused() const {
  std::lock_guard<std::mutex> lock(mu_);
  return paused_;
}

void ReplaySession::AsFast(bool fast) {
  std::lock_guard<std::mutex> lock(mu_);
  as_fast_ = fast;
  rate_generation_ += 1;
  cv_.notify_all();
}

bool ReplaySession::as_fast() const {
  std::lock_guard<std::mutex> lock(mu_);
  return as_fast_;
}

void ReplaySession::Stop() {
  stop_.store(true, std::memory_order_relaxed);
  cv_.notify_all();
}

bool ReplaySession::stopped() const {
  return stop_.load(std::memory_order_relaxed);
}

ReplayStatus ReplaySession::status() const {
  return status_.load(std::memory_order_relaxed);
}

ReplaySessionState ReplaySession::state() const {
  return state_.load(std::memory_order_relaxed);
}

uint64_t ReplaySession::delivered_events() const {
  std::lock_guard<std::mutex> lock(mu_);
  return delivered_;
}

}  // namespace replay
}  // namespace thyme
