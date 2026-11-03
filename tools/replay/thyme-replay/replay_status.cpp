// replay_status.cpp — REP-08 status document (live snapshot + final summary).
//
// Format: one JSON object, rewritten atomically by `play --status-file`.
//
//   {"tool":"thyme-replay","command":"play","version":1,
//    "state":"running|paused|finished|failed|stopped",
//    "package":..., "pid":N, "mode":"paced|as-fast|step",
//    "timeline":"absolute", "rate":1.000,
//    "events":N,"injected":N,"skipped":N,"ignored":N,"failed":N,
//    "unmapped":N,"position_mono_ns":N,"first_mono_ns":N,"last_mono_ns":N,
//    "package_first_mono_ns":N,"package_last_mono_ns":N,
//    "slice_joins":N,"join_duplicates":N,"join_holes":N,
//    "window":{"from_ns":N,"to_ns":N} | null,
//    "setup_ns":N,"elapsed_ns":N,"stopped":bool,"window_closed":bool,
//    "step_done":bool,"error":null|"...", "warnings":[...],
//    "updated_realtime_ns":N,"updated_mono_ns":N,
//    "channels":[{"id","name","kind","events","injected","skipped",
//                 "ignored","failed","unmapped","bytes","last_mono_ns"}]}
//
// The writer never formats a whole-file buffer: the object is streamed into the
// temp file and renamed. Readers must treat a missing `state` or a foreign
// `tool` field as "not a status document".

#include "replay_status.h"

#include "replay_cli.h"
#include "replay_json.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <sstream>
#include <sys/stat.h>
#include <unistd.h>

namespace thyme {
namespace replaycli {

namespace {

constexpr std::size_t kMaxStatusBytes = 1024u * 1024u;

uint64_t NowSteadyNs() {
  struct timespec ts {};
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<uint64_t>(ts.tv_sec) * 1000000000ULL +
         static_cast<uint64_t>(ts.tv_nsec);
}

uint64_t NowRealtimeNs() {
  struct timespec ts {};
  clock_gettime(CLOCK_REALTIME, &ts);
  return static_cast<uint64_t>(ts.tv_sec) * 1000000000ULL +
         static_cast<uint64_t>(ts.tv_nsec);
}

}  // namespace

void RenderStatusJson(std::ostream& out, const StatusSnapshot& s) {
  Json j(out);
  j.BeginObject();
  j.KeyStr("tool", "thyme-replay");
  j.KeyStr("command", "play");
  j.KeyU64("version", 1);
  j.KeyStr("state", s.state);
  j.KeyStr("package", s.package);
  j.KeyU64("pid", s.pid);
  j.KeyStr("mode", s.mode);
  j.KeyStr("timeline", s.timeline);
  j.KeyDouble("rate", s.rate);
  j.KeyU64("events", s.events);
  j.KeyU64("injected", s.injected);
  j.KeyU64("skipped", s.skipped);
  j.KeyU64("ignored", s.ignored);
  j.KeyU64("failed", s.failed);
  j.KeyU64("unmapped", s.unmapped);
  j.KeyU64("position_mono_ns", s.position_mono_ns);
  j.KeyU64("first_mono_ns", s.first_event_ns);
  j.KeyU64("last_mono_ns", s.last_event_ns);
  j.KeyU64("package_first_mono_ns", s.package_first_mono_ns);
  j.KeyU64("package_last_mono_ns", s.package_last_mono_ns);
  j.KeyU64("slice_joins", s.slice_joins);
  j.KeyU64("join_duplicates", s.join_duplicates);
  j.KeyU64("join_holes", s.join_holes);
  if (s.has_window) {
    j.Key("window");
    j.BeginObject();
    j.KeyU64("from_ns", s.from_ns);
    j.KeyU64("to_ns", s.to_ns);
    j.EndObject();
  } else {
    j.KeyNull("window");
  }
  j.KeyU64("setup_ns", s.setup_ns);
  j.KeyU64("elapsed_ns", s.elapsed_ns);
  j.KeyBool("stopped", s.stopped);
  j.KeyBool("window_closed", s.window_closed);
  j.KeyBool("step_done", s.step_done);
  if (s.error.empty()) {
    j.KeyNull("error");
  } else {
    j.KeyStr("error", s.error);
  }
  j.Key("warnings");
  j.BeginArray();
  for (const std::string& w : s.warnings) {
    j.Str(w);
  }
  j.EndArray();
  j.KeyU64("updated_realtime_ns", NowRealtimeNs());
  j.KeyU64("updated_mono_ns", NowSteadyNs());
  j.Key("channels");
  j.BeginArray();
  for (const StatusChannel& c : s.channels) {
    j.BeginObject();
    j.KeyU64("id", c.id);
    j.KeyStr("name", c.name);
    j.KeyStr("kind", c.kind);
    j.KeyU64("events", c.events);
    j.KeyU64("injected", c.injected);
    j.KeyU64("skipped", c.skipped);
    j.KeyU64("ignored", c.ignored);
    j.KeyU64("failed", c.failed);
    j.KeyU64("unmapped", c.unmapped);
    j.KeyU64("bytes", c.bytes);
    j.KeyU64("last_mono_ns", c.last_mono_ns);
    j.EndObject();
  }
  j.EndArray();
  j.EndObject();
  out << '\n';
}

bool StatusFile::Open(const std::string& path, uint32_t interval_ms,
                      std::string* err) {
  path_ = path;
  tmp_path_ = path + ".tmp";
  interval_ns_ = static_cast<uint64_t>(interval_ms) * 1000000ULL;
  last_write_ns_ = 0;
  last_state_.clear();
  wrote_once_ = false;
  error_.clear();
  enabled_ = !path_.empty();
  if (!enabled_) {
    if (err != nullptr) {
      *err = "--status-file needs a path";
    }
    return false;
  }
  if (interval_ms == 0) {
    enabled_ = false;
    if (err != nullptr) {
      *err = "--status-interval-ms must be > 0";
    }
    return false;
  }
  return true;
}

bool StatusFile::Due(const std::string& state) const {
  if (!enabled_) {
    return false;
  }
  if (!wrote_once_ || state != last_state_) {
    return true;
  }
  return NowSteadyNs() - last_write_ns_ >= interval_ns_;
}

void StatusFile::Publish(const StatusSnapshot& snapshot, bool force) {
  if (!enabled_) {
    return;
  }
  const uint64_t now = NowSteadyNs();
  if (!force && wrote_once_ && snapshot.state == last_state_ &&
      now - last_write_ns_ < interval_ns_) {
    return;
  }
  std::ostringstream body;
  RenderStatusJson(body, snapshot);
  std::FILE* f = std::fopen(tmp_path_.c_str(), "wb");
  if (f == nullptr) {
    error_ = "cannot open " + tmp_path_ + ": " + std::strerror(errno);
    enabled_ = false;
    return;
  }
  const std::string text = body.str();
  const bool wrote = std::fwrite(text.data(), 1, text.size(), f) == text.size();
  const bool closed = std::fclose(f) == 0;
  if (!wrote || !closed) {
    error_ = "cannot write " + tmp_path_ + ": " + std::strerror(errno);
    std::remove(tmp_path_.c_str());
    enabled_ = false;
    return;
  }
  if (std::rename(tmp_path_.c_str(), path_.c_str()) != 0) {
    error_ = "cannot rename " + tmp_path_ + " to " + path_ + ": " +
             std::strerror(errno);
    std::remove(tmp_path_.c_str());
    enabled_ = false;
    return;
  }
  wrote_once_ = true;
  last_write_ns_ = now;
  last_state_ = snapshot.state;
}

std::string StatusField(const std::string& json, const char* key) {
  const std::string needle = std::string("\"") + key + "\":";
  std::size_t p = json.find(needle);
  if (p == std::string::npos) {
    return std::string();
  }
  p += needle.size();
  while (p < json.size() && (json[p] == ' ' || json[p] == '\t')) {
    ++p;
  }
  if (p >= json.size()) {
    return std::string();
  }
  if (json[p] == '"') {
    ++p;
    std::string out;
    while (p < json.size() && json[p] != '"') {
      if (json[p] == '\\' && p + 1 < json.size()) {
        ++p;
      }
      out += json[p++];
    }
    return out;
  }
  const std::size_t start = p;
  while (p < json.size() && json[p] != ',' && json[p] != '}' &&
         json[p] != '\n') {
    ++p;
  }
  std::string raw = json.substr(start, p - start);
  while (!raw.empty() && (raw.back() == ' ' || raw.back() == '\t')) {
    raw.pop_back();
  }
  return raw;
}

int RunStatus(const std::string& path, bool as_json, std::ostream& out) {
  if (path.empty()) {
    out << "thyme-replay: status needs --status-file <path>\n";
    return kExitUsage;
  }
  struct stat st {};
  if (stat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) {
    out << "thyme-replay: no status file at " << path
        << " (run play --status-file <path>)\n";
    return kExitFailure;
  }
  if (static_cast<uint64_t>(st.st_size) > kMaxStatusBytes) {
    out << "thyme-replay: status file " << path << " is implausibly large\n";
    return kExitFailure;
  }
  std::ifstream in(path, std::ios::binary);
  std::ostringstream buf;
  buf << in.rdbuf();
  const std::string text = buf.str();
  if (text.empty() || text.find("\"tool\":\"thyme-replay\"") ==
                           std::string::npos ||
      text.find("\"state\":\"") == std::string::npos) {
    out << "thyme-replay: " << path
        << " is not a thyme-replay status document\n";
    return kExitFailure;
  }
  if (as_json) {
    out << text;
    if (text.back() != '\n') {
      out << '\n';
    }
    return kExitOk;
  }
  const std::string state = StatusField(text, "state");
  const std::string package = StatusField(text, "package");
  const std::string mode = StatusField(text, "mode");
  const std::string timeline = StatusField(text, "timeline");
  const std::string rate = StatusField(text, "rate");
  const std::string slice_joins = StatusField(text, "slice_joins");
  const std::string join_duplicates = StatusField(text, "join_duplicates");
  const std::string join_holes = StatusField(text, "join_holes");
  const std::string events = StatusField(text, "events");
  const std::string injected = StatusField(text, "injected");
  const std::string failed = StatusField(text, "failed");
  const std::string position = StatusField(text, "position_mono_ns");
  const std::string elapsed = StatusField(text, "elapsed_ns");
  const std::string error = StatusField(text, "error");
  out << "status  : " << state << '\n';
  out << "package : " << package << '\n';
  out << "mode    : " << mode << "  rate=" << rate
      << "  timeline=" << timeline << '\n';
  out << "joins   : " << slice_joins << "  duplicates=" << join_duplicates
      << "  holes=" << join_holes << '\n';
  out << "position: mono_ns " << position << "  events=" << events
      << "  injected=" << injected << "  failed=" << failed << '\n';
  out << "elapsed : " << elapsed << " ns\n";
  if (!error.empty() && error != "null") {
    out << "error   : " << error << '\n';
  }
  return kExitOk;
}

}  // namespace replaycli
}  // namespace thyme
