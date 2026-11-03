#pragma once

// replay_status.h — REP-08 live status surface for `thyme-replay play`.
//
// `play --status-file <path>` publishes the playback state as JSON to <path>
// (atomic temp+rename, so a reader never sees a partial document). The same
// document is the final summary when the run ends (`state` becomes
// finished/failed/stopped), and `thyme-replay status --status-file <path>`
// renders or echoes it. Nothing here is on the replay hot path: the runner
// publishes at most once per --status-interval-ms (default 200 ms) and on
// state transitions; each publish is one small write.

#include <cstdint>
#include <ostream>
#include <string>
#include <vector>

namespace thyme {
namespace replaycli {

struct StatusChannel {
  uint32_t id = 0;
  std::string name;
  std::string kind;
  uint64_t events = 0;
  uint64_t injected = 0;
  uint64_t skipped = 0;
  uint64_t ignored = 0;
  uint64_t failed = 0;
  uint64_t unmapped = 0;
  uint64_t bytes = 0;
  uint64_t last_mono_ns = 0;
};

// A point-in-time view of one playback. `state` is one of
// running | paused | finished | failed | stopped.
struct StatusSnapshot {
  std::string state;
  std::string package;
  std::string mode;  // paced | as-fast | step
  std::string timeline;
  double rate = 1.0;
  uint64_t pid = 0;
  uint64_t events = 0;
  uint64_t injected = 0;
  uint64_t skipped = 0;
  uint64_t ignored = 0;
  uint64_t failed = 0;
  uint64_t unmapped = 0;
  uint64_t position_mono_ns = 0;  // last delivered event (or the seek target)
  uint64_t first_event_ns = 0;
  uint64_t last_event_ns = 0;
  uint64_t package_first_mono_ns = 0;
  uint64_t package_last_mono_ns = 0;
  uint64_t slice_joins = 0;
  uint64_t join_duplicates = 0;
  uint64_t join_holes = 0;
  bool has_window = false;
  uint64_t from_ns = 0;
  uint64_t to_ns = 0;
  uint64_t setup_ns = 0;
  uint64_t elapsed_ns = 0;
  bool stopped = false;
  bool window_closed = false;
  bool step_done = false;
  std::string error;
  std::vector<std::string> warnings;
  std::vector<StatusChannel> channels;
};

void RenderStatusJson(std::ostream& out, const StatusSnapshot& snapshot);

// Throttled atomic publisher. Not thread-safe: the runner's playback thread is
// the only writer (the tty/control thread only sets requests).
class StatusFile {
 public:
  bool enabled() const { return enabled_; }
  bool Open(const std::string& path, uint32_t interval_ms, std::string* err);
  // True when the next Publish(state, false) would actually write. Lets the
  // caller skip building the snapshot on the playback fast path.
  bool Due(const std::string& state) const;
  // force=false writes only when the interval elapsed or the state changed.
  void Publish(const StatusSnapshot& snapshot, bool force);
  // Last publish error (empty while healthy). A failed write disables further
  // attempts; the caller reports it once.
  const std::string& error() const { return error_; }
  const std::string& path() const { return path_; }

 private:
  std::string path_;
  std::string tmp_path_;
  std::string error_;
  uint64_t interval_ns_ = 0;
  uint64_t last_write_ns_ = 0;
  std::string last_state_;
  bool enabled_ = false;
  bool wrote_once_ = false;
};

// `thyme-replay status`: renders the status file as text or echoes its JSON.
// Exit 0 when the file exists and looks like a thyme-replay status document,
// 1 when it is missing/unreadable/malformed.
int RunStatus(const std::string& path, bool as_json, std::ostream& out);

// Extracts a top-level string/number field from a status document (used by the
// text renderer; returns an empty string when absent).
std::string StatusField(const std::string& json, const char* key);

}  // namespace replaycli
}  // namespace thyme
