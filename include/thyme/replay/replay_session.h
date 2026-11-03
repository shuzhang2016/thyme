#pragma once

// replay_session.h — paced replay driver over ReplayPackage (REP-02).
//
// ReplaySession adds wall-clock pacing to the unified package timeline:
//
//   * timing derives from event mono_ns deltas: after the first event of a
//     playback segment (Open/Seek), every event is delayed by
//     (mono_ns - previous_mono_ns) / rate;
//   * AsFast(true) disables pacing (validation/record→replay→record tests);
//   * Pause() holds NextEvent() without consuming an event; Step() delivers
//     exactly one event immediately and leaves the session paused;
//   * Stop() is safe from another thread and is the only way out of a paused
//     or sleeping NextEvent() besides Resume(). It never blocks forever:
//     pause waits are bounded and every control call notifies the waiter.
//
// Threading: NextEvent()/Step()/Seek()/Open() belong to one playback thread.
// Pause()/Resume()/SetRate()/AsFast()/Stop() may be called from any thread.
// There is no injection and no CLI here (REP-03/REP-04 build on this API).

#include "thyme/replay/replay_reader.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>

namespace thyme {
namespace replay {

enum class ReplaySessionState : uint8_t {
  kClosed = 0,
  kPlaying = 1,
  kPaused = 2,
  kEnded = 3,
  kStopped = 4,
};

const char* ReplaySessionStateName(ReplaySessionState state);

class ReplaySession {
 public:
  ReplaySession();
  ~ReplaySession();
  ReplaySession(const ReplaySession&) = delete;
  ReplaySession& operator=(const ReplaySession&) = delete;

  // Opens the package and resets playback state: rate 1.0, paced (not
  // as-fast), not paused, stop flag cleared.
  ReplayStatus Open(const std::string& dir);
  void Close();

  // Blocks until the next event is due, then returns it. Paused sessions
  // block until Resume()/Step()/Stop(). Returns kEnd at the end of the
  // timeline, kStopped after Stop().
  ReplayStatus NextEvent(ReplayEvent* out);

  // Delivers exactly one event with no pacing and leaves the session paused.
  ReplayStatus Step(ReplayEvent* out);

  // Repositions the unified timeline (see ReplayPackage::SeekMonoNs); the
  // next delivered event is immediate, later events pace from it.
  ReplayStatus Seek(uint64_t mono_ns);

  // playback rate multiplier (>0). 2.0 = half wall time. kInvalidArgument for
  // non-finite or non-positive values.
  ReplayStatus SetRate(double rate);
  double rate() const;

  void Pause();
  void Resume();
  bool paused() const;

  // true = ignore mono_ns deltas (as fast as the reader can go).
  void AsFast(bool fast);
  bool as_fast() const;

  // Terminal for the current Open(): NextEvent() returns kStopped and any
  // in-flight pause/sleep is interrupted.
  void Stop();
  bool stopped() const;

  ReplayStatus status() const;
  ReplaySessionState state() const;
  uint64_t delivered_events() const;

  // Reader access for channel metadata and payload verification.
  const ReplayPackage& package() const { return package_; }

 private:
  mutable std::mutex mu_;
  std::condition_variable cv_;
  ReplayPackage package_;
  bool open_ = false;
  bool paused_ = false;
  bool as_fast_ = false;
  bool first_event_ = true;
  double rate_ = 1.0;
  uint32_t rate_generation_ = 0;
  uint64_t last_mono_ns_ = 0;
  uint64_t delivered_ = 0;
  std::atomic<bool> stop_{false};
  std::atomic<ReplayStatus> status_{ReplayStatus::kOk};
  std::atomic<ReplaySessionState> state_{ReplaySessionState::kClosed};
};

}  // namespace replay
}  // namespace thyme
