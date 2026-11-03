// CI-05: fixed, cheap core-path benchmark behind the per-runner gate
// (tools/ci/check_bench.py). Unlike bench_atomic_payload (PERF-07), which
// answers a design question and asserts correctness only, this binary emits
// machine-readable numbers so a nightly run can be compared against the
// recorded baseline for the same runner tag.
//
// Measured paths:
//   1. LatestSlot<512 B> Write + Read (small-struct hot path), and the RT-15
//      in-place variant (BeginWrite + fill + CommitWrite + Read) for the
//      copy-in elimination delta
//   2. SpscRing<1024> push_latest + try_pop (metadata ring)
//   3. PinnedSlot 19 MiB: publish + ReadFrame view + CopyPayloadTo, and
//      publish + ReadFrameStable (caller-owned scratch copy)
//   4. trigger centre config load (LoadCenterConfig on the shipped template)
//
// CRC is disabled on the PinnedSlot channel so the copy figures measure the
// payload move, not the hardware CRC pass; the seqlock/footer validation and
// the ReadFrameStable double-read protocol still run.
//
// PERF-06 adds a sampled p99/p999 per metric (individually timed operations;
// the clock read is part of the sample) so the nightly gate can catch tail
// regressions as well as steady-state ones. The run takes roughly a second on
// a normal x86_64 box. Numbers are absolute ns/op for the host that ran them:
// the baseline is keyed by runner tag and host and is never a target-board
// target.

#include "thyme/record/trigger_config.h"
#include "thyme/runtime/pinned_slot_store.h"
#include "thyme/runtime/latest_slot.h"
#include "thyme/runtime/spsc_ring.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <sys/utsname.h>
#include <thread>
#include <unistd.h>
#include <vector>

#ifndef THYME_BENCH_BUILD_TYPE
#define THYME_BENCH_BUILD_TYPE "unknown"
#endif
#ifndef THYME_TRIGGER_CONFIG_PATH
#define THYME_TRIGGER_CONFIG_PATH ""
#endif

namespace {

constexpr std::size_t kMiB = 1024 * 1024;
constexpr std::size_t kPinnedSlotBytes = 19 * kMiB;
constexpr std::size_t kSlotPayload = 512;
constexpr int kRepeats = 3;

struct Metric {
  std::string name;
  std::uint64_t iterations{0};
  double ns_per_op{0.0};
  std::uint64_t bytes_per_op{0};  // 0 = not a payload-mover
  bool ok{true};
  // PERF-06: sampled per-operation tail. The clock read is part of the
  // sample (it dominates the smallest ops), so these are only comparable to
  // the same runner's baseline, never used as an absolute budget.
  double p99_ns_per_op{0.0};
  double p999_ns_per_op{0.0};
};

// Sample counts are a compromise between tail resolution and run time:
// 2048 fast ops is microseconds of work, 128 19-MiB copies is ~0.2 s.
constexpr std::size_t kFastSamples = 2048;
constexpr std::size_t kSlowSamples = 128;
constexpr std::size_t kWakeSamples = 256;

double NowNs() {
  return std::chrono::duration<double, std::nano>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

// Wake deadlines are compared against CLOCK_MONOTONIC inside the ring, so the
// handshake bounds use the same clock rather than steady_clock.
std::uint64_t MonoNs() {
  struct timespec ts {};
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<std::uint64_t>(ts.tv_sec) * 1000000000ULL +
         static_cast<std::uint64_t>(ts.tv_nsec);
}

// Median of kRepeats timed runs; the warm-up pass is not measured. The median
// rejects a single scheduler hiccup without hiding a real, persistent
// regression.
template <typename Fn>
double MedianNsPerOp(std::size_t iters, Fn&& op) {
  const std::size_t warmup = std::min<std::size_t>(iters, 3);
  for (std::size_t i = 0; i < warmup; ++i) op(i);
  std::array<double, kRepeats> samples{};
  for (int r = 0; r < kRepeats; ++r) {
    const double t0 = NowNs();
    for (std::size_t i = 0; i < iters; ++i) op(i);
    const double t1 = NowNs();
    samples[static_cast<std::size_t>(r)] =
        (t1 - t0) / static_cast<double>(iters);
  }
  std::sort(samples.begin(), samples.end());
  return samples[kRepeats / 2];
}

// PERF-06: nearest-rank p99/p999 over individually timed operations. The op
// lambda is the same one the median loop uses, so a correctness flag raised
// during sampling still fails the run.
template <typename Fn>
void PercentileNsPerOp(std::size_t count, Fn&& op, double* p99,
                       double* p999) {
  if (count == 0) {
    *p99 = 0.0;
    *p999 = 0.0;
    return;
  }
  std::vector<double> samples(count);
  for (std::size_t i = 0; i < count; ++i) {
    const double t0 = NowNs();
    op(i);
    const double t1 = NowNs();
    samples[i] = t1 - t0;
  }
  std::sort(samples.begin(), samples.end());
  const auto at = [&](double pct) {
    std::size_t rank =
        static_cast<std::size_t>(pct / 100.0 * static_cast<double>(count) +
                                 0.999999);
    if (rank == 0) rank = 1;
    if (rank > count) rank = count;
    return samples[rank - 1];
  };
  *p99 = at(99.0);
  *p999 = at(99.9);
}

void SetPercentiles(std::vector<Metric>& metrics, double p99, double p999) {
  metrics.back().p99_ns_per_op = p99;
  metrics.back().p999_ns_per_op = p999;
}

std::string Trim(const std::string& s) {
  const auto begin = s.find_first_not_of(" \t\r\n");
  if (begin == std::string::npos) return "";
  const auto end = s.find_last_not_of(" \t\r\n");
  return s.substr(begin, end - begin + 1);
}

std::string ArchName() {
  struct utsname info {};
  if (uname(&info) != 0) return "unknown";
  return info.machine;
}

// The baseline is keyed by runner, never by target board: a developer box is
// "<arch>-local", a CI job sets THYME_BENCH_RUNNER_TAG (e.g. x86_64-ci).
std::string RunnerTag() {
  const char* env = std::getenv("THYME_BENCH_RUNNER_TAG");
  if (env != nullptr && *env != '\0') return env;
  const char* ci = std::getenv("CI");
  return ArchName() + ((ci != nullptr && *ci != '\0') ? "-ci" : "-local");
}

std::string GitSha() {
  FILE* pipe = popen("git rev-parse --short HEAD 2>/dev/null", "r");
  if (pipe == nullptr) return "unknown";
  char buf[64] = {0};
  const char* line = fgets(buf, sizeof(buf), pipe);
  pclose(pipe);
  if (line == nullptr) return "unknown";
  const std::string sha = Trim(buf);
  return sha.empty() ? "unknown" : sha;
}

std::string Toolchain() {
#if defined(__clang__)
  return std::string("Clang ") + __clang_version__;
#elif defined(__GNUC__)
  return std::string("GCC ") + __VERSION__;
#else
  return "unknown";
#endif
}

std::string CpuModel() {
  FILE* f = fopen("/proc/cpuinfo", "r");
  if (f == nullptr) return "unknown";
  char line[512];
  std::string model;
  while (fgets(line, sizeof(line), f) != nullptr) {
    if (std::strncmp(line, "model name", 10) == 0) {
      const char* colon = std::strchr(line, ':');
      if (colon != nullptr) {
        model = Trim(colon + 1);
        break;
      }
    }
  }
  fclose(f);
  return model.empty() ? "unknown" : model;
}

std::string TimestampUtc() {
  const std::time_t now = std::time(nullptr);
  struct tm tm_utc {};
  gmtime_r(&now, &tm_utc);
  char buf[32];
  std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm_utc);
  return buf;
}

std::string JsonEscape(const std::string& s) {
  std::string out;
  out.reserve(s.size() + 8);
  for (const char c : s) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default: out += c;
    }
  }
  return out;
}

void EmitJson(const std::string& path, const std::string& tag,
              const std::string& git_sha, const std::string& host,
              const std::string& cpu, const std::string& toolchain,
              const std::vector<Metric>& metrics, bool all_ok) {
  std::string out = "{\n";
  out += "  \"schema\": 1,\n";
  out += "  \"kind\": \"thyme-bench-core\",\n";
  out += "  \"generated_at\": \"" + TimestampUtc() + "\",\n";
  out += "  \"git_sha\": \"" + JsonEscape(git_sha) + "\",\n";
  out += "  \"runner_tag\": \"" + JsonEscape(tag) + "\",\n";
  out += "  \"hostname\": \"" + JsonEscape(host) + "\",\n";
  out += "  \"host\": {\"hostname\": \"" + JsonEscape(host) +
         "\", \"machine\": \"" + JsonEscape(ArchName()) + "\"},\n";
  out += "  \"cpu\": \"" + JsonEscape(cpu) + "\",\n";
  out += "  \"toolchain\": \"" + JsonEscape(toolchain) + "\",\n";
  out += "  \"build_type\": \"" + std::string(THYME_BENCH_BUILD_TYPE) + "\",\n";
  out += "  \"ok\": " + std::string(all_ok ? "true" : "false") + ",\n";
  out += "  \"benchmarks\": [\n";
  for (std::size_t i = 0; i < metrics.size(); ++i) {
    const Metric& m = metrics[i];
    const std::string gbps =
        m.bytes_per_op != 0 ? std::to_string(static_cast<double>(m.bytes_per_op) / m.ns_per_op)
                            : "null";
    char buf[512];
    std::snprintf(buf, sizeof(buf),
                  "    {\"name\": \"%s\", \"iterations\": %llu, "
                  "\"ns_per_op\": %.4f, \"p99_ns_per_op\": %.4f, "
                  "\"p999_ns_per_op\": %.4f, \"bytes_per_op\": %llu, "
                  "\"gbps\": %s}",
                  m.name.c_str(),
                  static_cast<unsigned long long>(m.iterations), m.ns_per_op,
                  m.p99_ns_per_op, m.p999_ns_per_op,
                  static_cast<unsigned long long>(m.bytes_per_op),
                  gbps.c_str());
    out += buf;
    if (i + 1 < metrics.size()) out += ",";
    out += "\n";
  }
  out += "  ]\n}\n";

  FILE* f = std::fopen(path.c_str(), "w");
  if (f == nullptr) {
    std::fprintf(stderr, "bench_core: cannot write %s\n", path.c_str());
    return;
  }
  std::fwrite(out.data(), 1, out.size(), f);
  std::fclose(f);
}

struct Sample512 {
  std::uint32_t counter;
  std::uint8_t payload[kSlotPayload - sizeof(std::uint32_t)];
};
static_assert(sizeof(Sample512) == kSlotPayload, "512 B sample");

int RunPinnedSlot(std::vector<Metric>& metrics) {
  char name[64];
  std::snprintf(name, sizeof(name), "/thyme_bench_core_%ld",
                static_cast<long>(getpid()));

  thyme::PinnedSlotWriter writer;
  if (!writer.Create(name, static_cast<std::uint32_t>(kPinnedSlotBytes), 2, 0)) {
    std::fprintf(stderr,
                 "bench_core: PinnedSlot Create failed (19 MiB x 2 slots; check "
                 "/dev/shm size)\n");
    return 1;
  }
  writer.SetCrcEnabled(false);  // measure the move, not the CRC pass
  thyme::PinnedSlotReader reader;
  if (!reader.Open(name, 2000000000ull)) {
    std::fprintf(stderr, "bench_core: PinnedSlot reader Open failed\n");
    thyme::ShmRegion::UnlinkName(name);
    thyme::ShmRegion::UnlinkName(thyme::PinnedSlotWriter::LeaseShmName(name).c_str());
    return 1;
  }

  const std::uint32_t len = static_cast<std::uint32_t>(kPinnedSlotBytes);
  for (int slot = 0; slot < 2; ++slot) {
    auto view = writer.Begin(len);
    if (view.payload == nullptr) {
      std::fprintf(stderr, "bench_core: PinnedSlot Begin failed\n");
      return 1;
    }
    std::memset(view.payload, static_cast<int>(0xA5 + slot), len);
    writer.Commit(static_cast<std::uint64_t>(slot + 1));
  }

  std::vector<std::uint8_t> dst(kPinnedSlotBytes);
  std::vector<std::uint8_t> scratch(kPinnedSlotBytes);

  std::uint64_t next_index = 2;  // slot 0 and 1 were filled above
  std::uint64_t last_copy_index = 0;
  std::uint64_t last_stable_index = 0;
  bool copy_ok = true;
  const auto copy_op = [&](std::size_t) {
    if (!copy_ok) return;
    const std::uint64_t index = next_index++;
    auto begin = writer.Begin(len);
    if (begin.payload == nullptr) {
      copy_ok = false;
      return;
    }
    writer.Commit();
    thyme::PinnedSlotReader::View view;
    if (reader.ReadFrame(index, view) !=
        thyme::PinnedSlotReader::ReadResult::kOk) {
      copy_ok = false;
      return;
    }
    if (view.data_len != len || !view.CopyPayloadTo(dst.data(), dst.size())) {
      copy_ok = false;
      return;
    }
    last_copy_index = index;
  };
  const double copy_ns = MedianNsPerOp(8, copy_op);

  bool stable_ok = true;
  const auto stable_op = [&](std::size_t) {
    if (!stable_ok) return;
    const std::uint64_t index = next_index++;
    auto begin = writer.Begin(len);
    if (begin.payload == nullptr) {
      stable_ok = false;
      return;
    }
    writer.Commit();
    thyme::PinnedSlotReader::View view;
    if (reader.ReadFrameStable(index, scratch.data(),
                               static_cast<std::uint32_t>(scratch.size()),
                               view) !=
            thyme::PinnedSlotReader::ReadResult::kOk ||
        view.data_len != len) {
      stable_ok = false;
      return;
    }
    last_stable_index = index;
  };
  const double stable_ns = MedianNsPerOp(5, stable_op);
  double copy_p99 = 0.0;
  double copy_p999 = 0.0;
  double stable_p99 = 0.0;
  double stable_p999 = 0.0;
  PercentileNsPerOp(kSlowSamples, copy_op, &copy_p99, &copy_p999);
  PercentileNsPerOp(kSlowSamples, stable_op, &stable_p99, &stable_p999);

  // Slots were filled once above with 0xA5 + slot and Commit never touches
  // payload bytes, so each buffer must still hold its source slot's pattern.
  const auto matches = [](const std::vector<std::uint8_t>& buf,
                           std::uint64_t index) {
    const auto expected = static_cast<std::uint8_t>(0xA5 + (index % 2));
    return std::all_of(buf.begin(), buf.end(), [expected](std::uint8_t b) {
      return b == expected;
    });
  };
  const bool copy_matches = copy_ok && matches(dst, last_copy_index);
  const bool stable_matches = stable_ok && matches(scratch, last_stable_index);

  metrics.push_back({"pinned_slot_copy_payload_to_19mib", 8, copy_ns,
                     kPinnedSlotBytes, copy_matches});
  SetPercentiles(metrics, copy_p99, copy_p999);
  metrics.push_back({"pinned_slot_read_frame_stable_19mib", 5, stable_ns,
                     kPinnedSlotBytes, stable_matches});
  SetPercentiles(metrics, stable_p99, stable_p999);

  reader.Close();
  writer.Close();
  thyme::ShmRegion::UnlinkName(name);
  thyme::ShmRegion::UnlinkName(thyme::PinnedSlotWriter::LeaseShmName(name).c_str());
  return (copy_matches && stable_matches) ? 0 : 1;
}

int RunAll(std::vector<Metric>& metrics, std::string* error) {
  // 1. LatestSlot write + read of a 512-byte struct.
  {
    thyme::LatestSlot<Sample512> slot;
    Sample512 in{};
    in.counter = 1;
    std::memset(in.payload, 0x5A, sizeof(in.payload));
    Sample512 out{};
    bool ok = true;
    const auto op = [&](std::size_t i) {
      in.counter = static_cast<std::uint32_t>(i);
      slot.Write(in);
      if (!slot.Read(out) || out.counter != in.counter) ok = false;
    };
    const double ns = MedianNsPerOp(1000000, op);
    double p99 = 0.0;
    double p999 = 0.0;
    PercentileNsPerOp(kFastSamples, op, &p99, &p999);
    metrics.push_back({"latest_slot_write_read", 1000000, ns,
                       2 * kSlotPayload, ok});
    SetPercentiles(metrics, p99, p999);
  }

  // 1b. RT-15 in-place variant of the same 512 B slot: BeginWrite + plain
  // fill + CommitWrite + Read. Compared against latest_slot_write_read this
  // measures the copy-in elimination on the small-struct hot path (RT-17).
  {
    thyme::LatestSlot<Sample512> slot;
    Sample512 out{};
    bool ok = true;
    const auto op = [&](std::size_t i) {
      Sample512* dst = slot.BeginWrite();
      dst->counter = static_cast<std::uint32_t>(i);
      std::memset(dst->payload, 0x5A, sizeof(dst->payload));
      slot.CommitWrite();
      if (!slot.Read(out) || out.counter != static_cast<std::uint32_t>(i)) {
        ok = false;
      }
    };
    const double ns = MedianNsPerOp(1000000, op);
    double p99 = 0.0;
    double p999 = 0.0;
    PercentileNsPerOp(kFastSamples, op, &p99, &p999);
    metrics.push_back({"latest_slot_inplace_write_read", 1000000, ns,
                       2 * kSlotPayload, ok});
    SetPercentiles(metrics, p99, p999);
  }

  // 2. SPSC ring push/pop of the packed (index, generation) word.
  {
    thyme::SpscRing<1024> ring;
    std::uint32_t idx = 0;
    std::uint32_t gen = 0;
    bool ok = true;
    const auto op = [&](std::size_t i) {
      const auto value = static_cast<std::uint32_t>(i);
      ring.push_latest(value, 0);
      if (!ring.try_pop(idx, gen) || idx != value) ok = false;
    };
    const double ns = MedianNsPerOp(1000000, op);
    double p99 = 0.0;
    double p999 = 0.0;
    PercentileNsPerOp(kFastSamples, op, &p99, &p999);
    metrics.push_back({"spsc_ring_push_pop", 1000000, ns, 0, ok});
    SetPercentiles(metrics, p99, p999);
  }

  // 2b. RT-06 wake-path costs.
  //
  //   * notify with no registered waiter: the whole no-waiter Notify body (the
  //     store-load edge that makes the zero trustworthy, plus the relaxed
  //     probe). This is the per-publish price of the skip fast path.
  //   * one publish per blocked waiter: a wake is a cross-thread handoff, so
  //     this is a round-trip (publish -> broadcast -> pop), not a
  //     self-contained op. It is the cost the no-waiter path avoids.
  //   * two reference encodings of the same publish -> probe edge on bare
  //     atomics, so the choice documented in spsc_ring.h stays reproducible:
  //     a release store + seq_cst fence, and a seq_cst store with no fence
  //     (sound too, but it serialises the producer's next head access here).
  //
  // Only delivery correctness is asserted; the numbers are never gated.
  {
    thyme::SpscRing<1024> ring;
    const auto op = [&](std::size_t) { ring.Notify(); };
    const double ns = MedianNsPerOp(1000000, op);
    double p99 = 0.0;
    double p999 = 0.0;
    PercentileNsPerOp(kFastSamples, op, &p99, &p999);
    metrics.push_back({"spsc_ring_notify_no_waiter", 1000000, ns, 0,
                       ring.waiter_count() == 0});
    SetPercentiles(metrics, p99, p999);
  }

  {
    constexpr std::uint32_t kWakes = 500;
    constexpr std::uint64_t kHandshakeNs = 5000000000ULL;
    thyme::SpscRing<64> ring;
    std::atomic<bool> stop{false};
    std::atomic<std::uint32_t> popped{0};
    std::thread waiter([&] {
      std::uint32_t v = 0;
      std::uint32_t g = 0;
      while (!stop.load(std::memory_order_acquire)) {
        while (ring.try_pop(v, g)) {
          popped.fetch_add(1, std::memory_order_release);
        }
        if (stop.load(std::memory_order_acquire)) break;
        ring.WaitUntil(MonoNs() + 20000000ULL);  // park; 20 ms bound
      }
    });
    bool ok = true;
    const auto op = [&](std::size_t i) {
      const std::uint64_t deadline = MonoNs() + kHandshakeNs;
      // Publish only while the waiter is parked, otherwise this would measure
      // the no-waiter path instead of a wake. The pop counter target is read
      // before the publish: a fast waiter can consume the entry before this
      // thread looks at `popped` again.
      while (ring.waiter_count() == 0 && MonoNs() < deadline) {
      }
      const std::uint32_t want = popped.load(std::memory_order_acquire) + 1;
      if (!ring.try_push(static_cast<std::uint32_t>(i), 0)) {
        ok = false;
        return;
      }
      while (popped.load(std::memory_order_acquire) < want &&
             MonoNs() < deadline) {
      }
      if (popped.load(std::memory_order_acquire) < want) ok = false;
    };
    const double ns = MedianNsPerOp(kWakes, op);
    double p99 = 0.0;
    double p999 = 0.0;
    PercentileNsPerOp(kWakeSamples, op, &p99, &p999);
    stop.store(true, std::memory_order_release);
    ring.Notify();  // wake the parked waiter; don't wait out the park bound
    waiter.join();
    metrics.push_back({"spsc_ring_wake_one_waiter", kWakes, ns, 0, ok});
    SetPercentiles(metrics, p99, p999);
  }

  {
    std::atomic<std::uint32_t> head{0};
    std::atomic<std::uint32_t> waiters{0};
    const auto fence_op = [&](std::size_t i) {
      head.store(static_cast<std::uint32_t>(i), std::memory_order_release);
      std::atomic_thread_fence(std::memory_order_seq_cst);
      if (waiters.load(std::memory_order_relaxed) != 0) head.load();
    };
    const auto sc_op = [&](std::size_t i) {
      head.store(static_cast<std::uint32_t>(i), std::memory_order_seq_cst);
      if (waiters.load(std::memory_order_relaxed) != 0) head.load();
    };
    const double fence_ns = MedianNsPerOp(500000, fence_op);
    const double sc_ns = MedianNsPerOp(500000, sc_op);
    double fence_p99 = 0.0;
    double fence_p999 = 0.0;
    double sc_p99 = 0.0;
    double sc_p999 = 0.0;
    PercentileNsPerOp(kFastSamples, fence_op, &fence_p99, &fence_p999);
    PercentileNsPerOp(kFastSamples, sc_op, &sc_p99, &sc_p999);
    const bool ok = waiters.load(std::memory_order_relaxed) == 0;
    metrics.push_back({"ref_publish_release_fence", 500000, fence_ns, 0, ok});
    SetPercentiles(metrics, fence_p99, fence_p999);
    metrics.push_back({"ref_publish_sc_store", 500000, sc_ns, 0, ok});
    SetPercentiles(metrics, sc_p99, sc_p999);
  }

  // 3. PinnedSlot 19 MiB copy paths.
  if (RunPinnedSlot(metrics) != 0) {
    *error = "PinnedSlot bench failed (see stderr)";
    return 1;
  }

  // 4. Trigger-centre config load (open + read + parse of the shipped
  //    minimal template, i.e. the startup path).
  {
    const char* path = THYME_TRIGGER_CONFIG_PATH;
    thyme::trigger_cfg::CenterConfig cfg;
    std::string err;
    if (path == nullptr || *path == '\0') {
      *error = "trigger config path not configured";
      return 1;
    }
    if (!thyme::trigger_cfg::LoadCenterConfig(path, &cfg, &err)) {
      *error = "trigger config load failed: " + err;
      return 1;
    }
    const std::size_t expected_sources = cfg.sources.size();
    bool ok = true;
    const auto op = [&](std::size_t) {
      thyme::trigger_cfg::CenterConfig loaded;
      std::string load_err;
      if (!thyme::trigger_cfg::LoadCenterConfig(path, &loaded, &load_err)) {
        ok = false;
        return;
      }
      if (loaded.sources.size() != expected_sources) ok = false;
    };
    const double ns = MedianNsPerOp(300, op);
    double p99 = 0.0;
    double p999 = 0.0;
    PercentileNsPerOp(256, op, &p99, &p999);
    metrics.push_back({"trigger_config_load", 300, ns, 0, ok});
    SetPercentiles(metrics, p99, p999);
  }
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  std::string json_path;
  std::string tag;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--json" && i + 1 < argc) {
      json_path = argv[++i];
    } else if (arg == "--tag" && i + 1 < argc) {
      tag = argv[++i];
    } else if (arg == "-h" || arg == "--help") {
      std::printf("usage: bench_core [--json PATH] [--tag RUNNER_TAG]\n"
                  "  THYME_BENCH_RUNNER_TAG sets the default tag\n");
      return 0;
    } else {
      std::fprintf(stderr, "bench_core: unknown argument %s\n", arg.c_str());
      return 2;
    }
  }
  if (tag.empty()) tag = RunnerTag();

  std::vector<Metric> metrics;
  std::string error;
  const int rc = RunAll(metrics, &error);
  bool all_ok = rc == 0;
  for (const Metric& m : metrics) all_ok = all_ok && m.ok;

  std::printf("bench_core: runner=%s git=%s toolchain=%s build=%s\n",
              tag.c_str(), GitSha().c_str(), Toolchain().c_str(),
              THYME_BENCH_BUILD_TYPE);
  std::printf("%-36s %10s %12s %10s  %s\n", "metric", "iters", "ns/op",
              "GB/s", "check");
  for (const Metric& m : metrics) {
    if (m.bytes_per_op != 0) {
      std::printf("%-36s %10llu %12.3f %10.2f  %s\n", m.name.c_str(),
                  static_cast<unsigned long long>(m.iterations), m.ns_per_op,
                  static_cast<double>(m.bytes_per_op) / m.ns_per_op,
                  m.ok ? "OK" : "BAD");
    } else {
      std::printf("%-36s %10llu %12.3f %10s  %s\n", m.name.c_str(),
                  static_cast<unsigned long long>(m.iterations), m.ns_per_op,
                  "-", m.ok ? "OK" : "BAD");
    }
  }

  if (!json_path.empty()) {
    char host[256] = {0};
    gethostname(host, sizeof(host) - 1);
    EmitJson(json_path, tag, GitSha(), host, CpuModel(), Toolchain(), metrics,
             all_ok);
  }

  if (!all_ok) {
    std::fprintf(stderr, "bench_core: %s\n",
                 error.empty() ? "a measured path produced wrong data"
                               : error.c_str());
    return 1;
  }
  return 0;
}
