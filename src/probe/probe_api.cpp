// probe_api.cpp — ProbeApi implementation: discovery, manifests, sampling.
//
// Only PROT_READ mappings and load-only reads happen here. No function in
// this file calls Push/Write/Commit/TryPop/arm/lease-acquire or shm_unlink.

#include "thyme/probe/probe.h"
#include "thyme/runtime/shared_health.h"

#include "latency.h"
#include "page_reader.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <fstream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace thyme {
namespace probe {
namespace {

constexpr uint64_t kMinPeriodNs = 1000000ULL;    // 1 ms
constexpr uint64_t kMaxPeriodNs = 60000000000ULL;  // 60 s
constexpr uint32_t kMaxEchoBytes = 1u << 20;     // 1 MiB per sample

uint64_t NowNs() {
  struct timespec ts {};
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<uint64_t>(ts.tv_sec) * 1000000000ULL +
         static_cast<uint64_t>(ts.tv_nsec);
}

void SleepUntil(uint64_t abs_ns) {
  struct timespec ts {};
  ts.tv_sec = static_cast<time_t>(abs_ns / 1000000000ULL);
  ts.tv_nsec = static_cast<long>(abs_ns % 1000000000ULL);
  while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, nullptr) ==
         EINTR) {
  }
}

uint64_t ClampPeriod(uint64_t ns) {
  if (ns < kMinPeriodNs) return kMinPeriodNs;
  if (ns > kMaxPeriodNs) return kMaxPeriodNs;
  return ns;
}

std::string NormalizeName(const std::string& name) {
  if (name.empty()) return name;
  if (name.front() == '/') return name;
  return "/" + name;
}

bool EndsWith(const std::string& s, const char* suffix) {
  const std::size_t n = std::strlen(suffix);
  return s.size() >= n && s.compare(s.size() - n, n, suffix) == 0;
}

bool ParseU32(const std::string& s, uint32_t* out) {
  if (s.empty()) return false;
  char* end = nullptr;
  errno = 0;
  const unsigned long v = std::strtoul(s.c_str(), &end, 0);
  if (errno != 0 || end == nullptr || *end != '\0' || v > 0xFFFFFFFFUL) {
    return false;
  }
  *out = static_cast<uint32_t>(v);
  return true;
}

bool ParseU16(const std::string& s, uint16_t* out) {
  uint32_t v = 0;
  if (!ParseU32(s, &v) || v > 0xFFFFu) return false;
  *out = static_cast<uint16_t>(v);
  return true;
}

std::string Trim(const std::string& s) {
  std::size_t b = 0;
  while (b < s.size() &&
         std::isspace(static_cast<unsigned char>(s[b])) != 0) {
    ++b;
  }
  std::size_t e = s.size();
  while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1])) != 0) {
    --e;
  }
  return s.substr(b, e - b);
}

struct RateTick {
  uint64_t t_ns = 0;
  uint64_t sequence = 0;
  uint64_t bytes = 0;  // bytes published since the previous tick
};

double ToHz(uint64_t count, uint64_t window_ns) {
  if (window_ns == 0) return 0.0;
  return static_cast<double>(count) * 1e9 / static_cast<double>(window_ns);
}

double ToBytesPerS(uint64_t bytes, uint64_t window_ns) {
  if (window_ns == 0) return 0.0;
  return static_cast<double>(bytes) * 1e9 / static_cast<double>(window_ns);
}

}  // namespace

const char* KindName(Kind kind) {
  switch (kind) {
    case Kind::kLatestSlot:
      return "latest_slot";
    case Kind::kSpscRing:
      return "spsc_ring";
    case Kind::kSpmcRing:
      return "spmc_ring";
    case Kind::kPinnedSlot:
      return "pinned_slot";
    case Kind::kSharedHealth:
      return "shared_health";
    case Kind::kMpControl:
      return "mp_control";
    case Kind::kRecordMirrorRing:
      return "record_mirror_ring";
    case Kind::kRecordMirror:
      return "record_mirror";
    case Kind::kUnknown:
      break;
  }
  return "unknown";
}

Kind KindFromName(const char* label) {
  if (label == nullptr) return Kind::kUnknown;
  const std::string s(label);
  if (s == "latest_slot") return Kind::kLatestSlot;
  if (s == "spsc_ring") return Kind::kSpscRing;
  if (s == "spmc_ring") return Kind::kSpmcRing;
  if (s == "pinned_slot") return Kind::kPinnedSlot;
  if (s == "shared_health") return Kind::kSharedHealth;
  if (s == "mp_control") return Kind::kMpControl;
  if (s == "record_mirror_ring") return Kind::kRecordMirrorRing;
  if (s == "record_mirror") return Kind::kRecordMirror;
  return Kind::kUnknown;
}

const char* StatusName(Status status) {
  switch (status) {
    case Status::kOk:
      return "ok";
    case Status::kUsage:
      return "usage";
    case Status::kNotFound:
      return "not_found";
    case Status::kBadGeometry:
      return "bad_geometry";
    case Status::kNotReadable:
      return "not_readable";
    case Status::kNoData:
      return "no_data";
  }
  return "unknown";
}

int ExitCode(Status status) {
  switch (status) {
    case Status::kOk:
      return 0;
    case Status::kUsage:
      return 1;
    case Status::kNotFound:
      return 2;
    case Status::kBadGeometry:
      return 3;
    case Status::kNotReadable:
      return 4;
    case Status::kNoData:
      return 5;
  }
  return 1;
}

struct ProbeApi::Impl {
  Options options;
  std::vector<ManifestEntry> manifest;

  const ManifestEntry* Find(const std::string& name) const {
    for (const auto& e : manifest) {
      if (e.name == name) return &e;
    }
    return nullptr;
  }

  Status Prepare(const std::string& name, detail::MappedPage* page,
                 detail::Geometry* geometry) const {
    const std::string norm = NormalizeName(name);
    return detail::OpenGeometry(norm, Find(norm), page, geometry);
  }

  RegionInfo ToInfo(const std::string& name,
                    const detail::Geometry& g) const {
    RegionInfo info;
    info.name = NormalizeName(name);
    info.kind = g.kind;
    info.abi_version = g.abi_version;
    info.compatible = g.compatible;
    info.slots = g.slots;
    info.slot_bytes = g.slot_bytes;
    info.stride = g.stride;
    info.region_bytes = g.region_bytes;
    info.head = g.head;
    info.tail = g.tail;
    info.epoch = g.epoch;
    info.drop_count = g.drop;
    info.reader_drop_slots = g.reader_drop_slots;
    info.skip_count = g.skip;
    info.steal_count = g.steal;
    info.session_id = g.session;
    info.has_session = g.has_session;
    info.workers = g.workers;
    info.structs = g.structs;
    info.frame_shaped = g.frame_shaped;
    info.geometry_source = g.source;
    info.note = g.note;
    return info;
  }

  // One polling round. Returns true when a usable sample was produced.
  static bool ReadOne(const detail::MappedPage& page,
                      const detail::Geometry& g, detail::RawSample* raw) {
    return detail::ReadNewest(page, g, raw) && raw->has && !raw->torn;
  }

  Status CollectRateTicks(const detail::MappedPage& page,
                          const detail::Geometry& g, uint64_t window_ns,
                          uint64_t period_ns, bool exact, bool need_bytes,
                          std::vector<RateTick>* ticks,
                          uint64_t* total_bytes,
                          bool* exact_ok) const;
};

Status ProbeApi::Impl::CollectRateTicks(
    const detail::MappedPage& page, const detail::Geometry& g,
    uint64_t window_ns, uint64_t period_ns, bool exact, bool need_bytes,
    std::vector<RateTick>* ticks, uint64_t* total_bytes,
    bool* exact_ok) const {
  ticks->clear();
  *total_bytes = 0;
  *exact_ok = exact;
  if (need_bytes && g.slot_bytes == 0) {
    return Status::kBadGeometry;  // bw needs a known slot size
  }
  const uint64_t start = NowNs();
  const uint64_t deadline = start + window_ns;
  const uint64_t period = ClampPeriod(period_ns);

  RateTick baseline;
  baseline.t_ns = start;
  detail::RawSample raw;
  if (ReadOne(page, g, &raw)) baseline.sequence = raw.sequence;
  ticks->push_back(baseline);

  uint64_t last_seq = baseline.sequence;
  uint64_t next_tick = start + period;
  while (true) {
    const uint64_t now = NowNs();
    if (now >= deadline) break;
    if (ReadOne(page, g, &raw) && raw.sequence != last_seq) {
      RateTick t;
      t.t_ns = now;
      t.sequence = raw.sequence;
      const uint64_t dseq = raw.sequence - last_seq;
      if (exact && g.kind == Kind::kPinnedSlot) {
        // Sum the real data_len of the frames published since the last tick.
        // If more than one ring of slots elapsed the older lengths are gone;
        // fall back to slot_bytes and mark the run non-exact.
        if (dseq <= g.slots) {
          uint64_t sum = 0;
          bool ok = true;
          for (uint64_t seq = last_seq + 1u; seq <= raw.sequence; ++seq) {
            uint32_t len = 0;
            if (!detail::ReadFrameSize(page, g, seq - 1u, &len)) {
              ok = false;
              break;
            }
            sum += len;
          }
          if (ok) {
            t.bytes = sum;
          } else {
            t.bytes = dseq * g.slot_bytes;
            *exact_ok = false;
          }
        } else {
          t.bytes = dseq * g.slot_bytes;
          *exact_ok = false;
        }
      } else {
        t.bytes = g.slot_bytes != 0 ? dseq * g.slot_bytes : 0;
      }
      *total_bytes += t.bytes;
      ticks->push_back(t);
      last_seq = raw.sequence;
    }
    const uint64_t now2 = NowNs();
    if (now2 >= deadline) break;
    next_tick += period;
    if (next_tick <= now2) next_tick = now2 + period;
    if (next_tick > deadline) next_tick = deadline;
    SleepUntil(next_tick);
  }
  // A page with no new publishes is a valid 0 Hz/0 B/s observation; only an
  // unreadable page is an error.
  return Status::kOk;
}

ProbeApi::ProbeApi() : impl_(new Impl()) {}

ProbeApi::~ProbeApi() = default;

void ProbeApi::SetOptions(const Options& options) {
  impl_->options = options;
}

const Options& ProbeApi::options() const { return impl_->options; }

void ProbeApi::AddManifestEntry(const ManifestEntry& entry) {
  ManifestEntry e = entry;
  e.name = NormalizeName(e.name);
  impl_->manifest.push_back(e);
}

Status ProbeApi::LoadManifest(const std::string& path) {
  std::ifstream in(path);
  if (!in) return Status::kNotFound;
  impl_->options.manifest_path = path;
  std::string line;
  while (std::getline(in, line)) {
    const std::size_t hash = line.find('#');
    if (hash != std::string::npos) line = line.substr(0, hash);
    line = Trim(line);
    if (line.empty()) continue;
    std::istringstream iss(line);
    std::vector<std::string> tok;
    std::string t;
    while (iss >> t) tok.push_back(t);
    if (tok.size() < 4) return Status::kUsage;
    ManifestEntry e;
    e.name = NormalizeName(tok[0]);
    e.kind = KindFromName(tok[1].c_str());
    if (e.kind == Kind::kUnknown) return Status::kUsage;
    if (!ParseU32(tok[2], &e.slot_bytes)) return Status::kUsage;
    if (!ParseU32(tok[3], &e.depth)) return Status::kUsage;
    std::size_t i = 4;
    if (i < tok.size() && ParseU16(tok[i], &e.struct_id)) ++i;
    if (i < tok.size() && tok[i] != "frame_header" && tok[i] != "raw") {
      e.struct_name = tok[i];
      ++i;
    }
    for (; i < tok.size(); ++i) {
      if (tok[i] == "frame_header") e.frame_shaped = true;
    }
    impl_->manifest.push_back(e);
  }
  return Status::kOk;
}

Status ProbeApi::List(std::vector<RegionInfo>& out) {
  out.clear();
  DIR* dir = opendir("/dev/shm");
  if (dir == nullptr) return Status::kNotFound;
  std::vector<std::string> names;
  while (dirent* de = readdir(dir)) {
    const std::string entry(de->d_name);
    if (entry == "." || entry == "..") continue;
    if (EndsWith(entry, ".lease")) continue;  // attached to its parent page
    bool match = false;
    for (const auto& prefix : impl_->options.prefixes) {
      if (!prefix.empty() && entry.compare(0, prefix.size(), prefix) == 0) {
        match = true;
        break;
      }
    }
    if (match) names.push_back("/" + entry);
  }
  closedir(dir);
  std::sort(names.begin(), names.end());
  for (const auto& name : names) {
    detail::MappedPage page;
    detail::Geometry g;
    const Status st = impl_->Prepare(name, &page, &g);
    if (st == Status::kNotFound) continue;  // not a thyme page
    RegionInfo info = impl_->ToInfo(name, g);
    if (st != Status::kOk) {
      info.compatible = false;
      if (info.note.empty()) info.note = StatusName(st);
    }
    out.push_back(info);
  }
  return out.empty() ? Status::kNotFound : Status::kOk;
}

Status ProbeApi::Info(const std::string& name, RegionInfo& out) {
  detail::MappedPage page;
  detail::Geometry g;
  const std::string norm = NormalizeName(name);
  const Status st = impl_->Prepare(norm, &page, &g);
  if (st == Status::kOk || st == Status::kBadGeometry) {
    out = impl_->ToInfo(norm, g);
  } else {
    out = RegionInfo{};
    out.name = norm;
  }
  return st;
}

Status ProbeApi::Newest(const std::string& name, Sample* out) {
  detail::MappedPage page;
  detail::Geometry g;
  const Status st = impl_->Prepare(name, &page, &g);
  if (st != Status::kOk) return st;
  if (!detail::StreamKind(g.kind)) return Status::kBadGeometry;
  detail::RawSample raw;
  if (!Impl::ReadOne(page, g, &raw)) return Status::kNoData;
  out->sequence = raw.sequence;
  out->producer_ns = raw.producer_ns;
  out->observed_ns = NowNs();
  out->size = raw.size;
  out->rate_hz = 0.0;
  out->torn = false;
  return Status::kOk;
}

Status ProbeApi::Watch(const std::string& name, const SampleOptions& options,
                       const std::function<bool(const Sample&)>& on_sample,
                       uint64_t* emitted) {
  uint64_t n = 0;
  detail::MappedPage page;
  detail::Geometry g;
  const Status st = impl_->Prepare(name, &page, &g);
  if (st != Status::kOk) return st;
  if (!detail::StreamKind(g.kind)) return Status::kBadGeometry;

  const uint64_t period = ClampPeriod(options.period_ns);
  const uint64_t start = NowNs();
  const uint64_t deadline =
      options.max_duration_ns != 0 ? start + options.max_duration_ns : 0;
  uint64_t prev_seq = kNone;
  uint64_t prev_ns = 0;
  // Baseline from the value already published when watch starts, so the first
  // emitted sample carries a rate instead of 0.
  detail::RawSample baseline;
  if (Impl::ReadOne(page, g, &baseline)) {
    prev_seq = baseline.sequence;
    prev_ns = start;
  }
  uint64_t next_tick = start;
  while (true) {
    detail::RawSample raw;
    const uint64_t now = NowNs();
    if (Impl::ReadOne(page, g, &raw) && raw.sequence != prev_seq) {
      Sample s;
      s.sequence = raw.sequence;
      s.producer_ns = raw.producer_ns;
      s.observed_ns = now;
      s.size = raw.size;
      s.torn = false;
      if (prev_seq != kNone && now > prev_ns) {
        s.rate_hz = ToHz(raw.sequence - prev_seq, now - prev_ns);
      }
      prev_seq = raw.sequence;
      prev_ns = now;
      ++n;
      if (!on_sample(s)) break;
      if (options.count > 0 && n >= static_cast<uint64_t>(options.count)) {
        break;
      }
    }
    if (deadline != 0 && NowNs() >= deadline) break;
    next_tick += period;
    const uint64_t now2 = NowNs();
    if (next_tick <= now2) next_tick = now2 + period;
    if (deadline != 0 && next_tick > deadline) next_tick = deadline;
    SleepUntil(next_tick);
  }
  if (emitted != nullptr) *emitted = n;
  return n == 0 ? Status::kNoData : Status::kOk;
}

Status ProbeApi::Hz(const std::string& name, uint64_t window_ns,
                    uint64_t period_ns, RateStats* out) {
  detail::MappedPage page;
  detail::Geometry g;
  const Status st = impl_->Prepare(name, &page, &g);
  if (st != Status::kOk) return st;
  if (!detail::StreamKind(g.kind)) return Status::kBadGeometry;
  std::vector<RateTick> ticks;
  uint64_t total_bytes = 0;
  bool exact = false;
  const Status cst = impl_->CollectRateTicks(page, g, window_ns, period_ns,
                                             false, false, &ticks,
                                             &total_bytes, &exact);
  if (cst != Status::kOk) return cst;
  const RateTick& a = ticks.front();
  const RateTick& b = ticks.back();
  const uint64_t span = ticks.size() >= 2 ? b.t_ns - a.t_ns : window_ns;
  const uint64_t published = ticks.size() >= 2 ? b.sequence - a.sequence : 0;
  out->window_ns = span;
  out->published = published;
  out->window_s = static_cast<double>(span) / 1e9;
  out->hz_avg = ToHz(published, span);
  out->hz_min = out->hz_avg;
  out->hz_max = out->hz_avg;
  out->samples = ticks.size() - 1;
  out->bytes_per_s = 0.0;
  out->bytes_per_s_min = 0.0;
  out->bytes_per_s_max = 0.0;
  out->exact_bytes = false;
  if (published == 0 || span == 0) return Status::kOk;
  const double avg_period =
      static_cast<double>(span) / static_cast<double>(published);
  detail::LatencyRecorder jitter;
  bool first = true;
  for (std::size_t i = 1; i < ticks.size(); ++i) {
    const uint64_t dt = ticks[i].t_ns - ticks[i - 1].t_ns;
    const uint64_t dseq = ticks[i].sequence - ticks[i - 1].sequence;
    if (dt == 0 || dseq == 0) continue;
    const double hz = ToHz(dseq, dt);
    if (first || hz < out->hz_min) out->hz_min = hz;
    if (first || hz > out->hz_max) out->hz_max = hz;
    first = false;
    const double period_i =
        static_cast<double>(dt) / static_cast<double>(dseq);
    const double diff =
        period_i > avg_period ? period_i - avg_period : avg_period - period_i;
    jitter.Add(static_cast<uint64_t>(diff));
  }
  const LatencyStats js = jitter.Summarize(span);
  out->jitter_p50_ns = js.p50_ns;
  out->jitter_p99_ns = js.p99_ns;
  out->jitter_max_ns = js.max_ns;
  return Status::kOk;
}

Status ProbeApi::Bw(const std::string& name, uint64_t window_ns,
                    uint64_t period_ns, bool exact_bytes, RateStats* out) {
  detail::MappedPage page;
  detail::Geometry g;
  const Status st = impl_->Prepare(name, &page, &g);
  if (st != Status::kOk) return st;
  if (!detail::StreamKind(g.kind)) return Status::kBadGeometry;
  if (g.slot_bytes == 0) {
    return Status::kBadGeometry;  // slot size comes from the manifest
  }
  std::vector<RateTick> ticks;
  uint64_t total_bytes = 0;
  bool exact = false;
  const Status cst = impl_->CollectRateTicks(page, g, window_ns, period_ns,
                                             exact_bytes, true, &ticks,
                                             &total_bytes, &exact);
  if (cst != Status::kOk) return cst;
  const RateTick& a = ticks.front();
  const RateTick& b = ticks.back();
  const uint64_t span = ticks.size() >= 2 ? b.t_ns - a.t_ns : window_ns;
  const uint64_t published = ticks.size() >= 2 ? b.sequence - a.sequence : 0;
  if (span == 0) return Status::kOk;
  out->window_ns = span;
  out->published = published;
  out->window_s = static_cast<double>(span) / 1e9;
  out->hz_avg = ToHz(published, span);
  out->hz_min = out->hz_avg;
  out->hz_max = out->hz_avg;
  out->samples = ticks.size() - 1;
  out->bytes_per_s = ToBytesPerS(total_bytes, span);
  out->bytes_per_s_min = out->bytes_per_s;
  out->bytes_per_s_max = out->bytes_per_s;
  out->exact_bytes = exact;
  bool first = true;
  const double avg_period =
      published != 0 ? static_cast<double>(span) /
                           static_cast<double>(published)
                     : 0.0;
  detail::LatencyRecorder jitter;
  for (std::size_t i = 1; i < ticks.size(); ++i) {
    const uint64_t dt = ticks[i].t_ns - ticks[i - 1].t_ns;
    const uint64_t dseq = ticks[i].sequence - ticks[i - 1].sequence;
    if (dt == 0 || dseq == 0) continue;
    const double hz = ToHz(dseq, dt);
    if (first || hz < out->hz_min) out->hz_min = hz;
    if (first || hz > out->hz_max) out->hz_max = hz;
    const double bw = ToBytesPerS(ticks[i].bytes, dt);
    if (first || bw < out->bytes_per_s_min) out->bytes_per_s_min = bw;
    if (first || bw > out->bytes_per_s_max) out->bytes_per_s_max = bw;
    if (avg_period > 0.0) {
      const double period_i =
          static_cast<double>(dt) / static_cast<double>(dseq);
      const double diff =
          period_i > avg_period ? period_i - avg_period : avg_period - period_i;
      jitter.Add(static_cast<uint64_t>(diff));
    }
    first = false;
  }
  const LatencyStats js = jitter.Summarize(span);
  out->jitter_p50_ns = js.p50_ns;
  out->jitter_p99_ns = js.p99_ns;
  out->jitter_max_ns = js.max_ns;
  return Status::kOk;
}

Status ProbeApi::Delay(const std::string& name, uint64_t window_ns,
                       uint64_t period_ns, LatencyStats* out) {
  detail::MappedPage page;
  detail::Geometry g;
  const Status st = impl_->Prepare(name, &page, &g);
  if (st != Status::kOk) return st;
  if (!detail::StreamKind(g.kind)) return Status::kBadGeometry;
  const bool pinned_slot = g.kind == Kind::kPinnedSlot;
  if (!pinned_slot && !g.frame_shaped) {
    return Status::kBadGeometry;  // no producer timestamp on this page
  }
  const uint64_t start = NowNs();
  const uint64_t deadline = start + window_ns;
  const uint64_t period = ClampPeriod(period_ns);
  detail::LatencyRecorder rec;
  uint64_t observations = 0;
  uint64_t next_tick = start;
  while (true) {
    const uint64_t now = NowNs();
    detail::RawSample raw;
    if (Impl::ReadOne(page, g, &raw) && raw.has_stamp) {
      const uint64_t age = now > raw.producer_ns ? now - raw.producer_ns : 0;
      rec.Add(age);
      ++observations;
    }
    if (now >= deadline) break;
    next_tick += period;
    const uint64_t now2 = NowNs();
    if (next_tick <= now2) next_tick = now2 + period;
    if (next_tick > deadline) next_tick = deadline;
    SleepUntil(next_tick);
  }
  if (observations == 0) return Status::kNoData;
  *out = rec.Summarize(window_ns);
  out->frame_shaped = g.frame_shaped;
  out->stamp_source = pinned_slot ? "pinned_slot_slot" : "frame_header";
  return Status::kOk;
}

Status ProbeApi::Echo(const std::string& name, int count, uint32_t max_bytes,
                      uint64_t timeout_ns, std::vector<EchoSample>* out) {
  out->clear();
  if (count <= 0) count = 1;
  if (max_bytes == 0) max_bytes = 4096;
  if (max_bytes > kMaxEchoBytes) max_bytes = kMaxEchoBytes;
  detail::MappedPage page;
  detail::Geometry g;
  const std::string norm = NormalizeName(name);
  const Status st = impl_->Prepare(norm, &page, &g);
  if (st != Status::kOk) return st;
  if (g.kind == Kind::kSharedHealth) {
    const auto* health = reinterpret_cast<const SharedHealthPage*>(page.base());
    for (uint32_t i = 0;
         i < g.workers && static_cast<int>(out->size()) < count; ++i) {
      const HealthSlot& slot = health->slots[i];
      EchoSample s;
      s.sequence = slot.alive_counter.load(std::memory_order_acquire);
      s.producer_ns = slot.last_finish_mono_ns.load(std::memory_order_acquire);
      s.observed_ns = NowNs();
      s.size = sizeof(HealthSlot);
      s.drop_count = slot.drop_cnt.load(std::memory_order_acquire);
      s.worker = i;
      s.has_worker = true;
      s.memory_flags = slot.memory_flags.load(std::memory_order_acquire);
      s.has_memory_flags = true;
      const uint32_t copied = max_bytes < s.size ? max_bytes : s.size;
      s.truncated = copied < s.size;
      s.bytes.resize(copied);
      if (copied != 0) std::memcpy(s.bytes.data(), &slot, copied);
      out->push_back(std::move(s));
    }
    return out->empty() ? Status::kNoData : Status::kOk;
  }
  if (!detail::StreamKind(g.kind)) return Status::kBadGeometry;
  const ManifestEntry* entry = impl_->Find(norm);
  const uint64_t deadline = NowNs() + timeout_ns;
  std::vector<uint8_t> buf(max_bytes);
  uint64_t last_seq = kNone;
  while (static_cast<int>(out->size()) < count) {
    detail::RawSample raw;
    uint32_t copied = 0;
    if (detail::ReadStablePayload(page, g, max_bytes, buf.data(), &raw,
                                 &copied)) {
      if (raw.sequence != last_seq) {
        EchoSample s;
        s.sequence = raw.sequence;
        s.producer_ns = raw.producer_ns;
        s.observed_ns = NowNs();
        s.size = raw.size;
        s.drop_count = g.drop;
        s.truncated = raw.size > copied;
        s.bytes.assign(buf.begin(),
                       buf.begin() + static_cast<std::ptrdiff_t>(copied));
        if (entry != nullptr) {
          s.struct_id = entry->struct_id;
          s.struct_name = entry->struct_name;
        }
        if (g.frame_shaped && copied >= sizeof(FrameHeader)) {
          FrameHeader h{};
          std::memcpy(&h, buf.data(), sizeof(FrameHeader));
          if (detail::FrameHeaderMetadataOk(h, g.slot_bytes)) {
            s.header = h;
            s.has_header = true;
          }
        }
        out->push_back(std::move(s));
        last_seq = raw.sequence;
      }
    }
    if (NowNs() >= deadline) break;
    const uint64_t now = NowNs();
    const uint64_t step = now + kMinPeriodNs * 2;
    SleepUntil(step < deadline ? step : deadline);
  }
  return out->empty() ? Status::kNoData : Status::kOk;
}

}  // namespace probe
}  // namespace thyme
