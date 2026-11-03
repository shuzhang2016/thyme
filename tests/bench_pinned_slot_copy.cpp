// PERF-15: real PinnedSlot copy-body, lease pin, and producer-pressure evidence.

#include <sys/utsname.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <thread>
#include <vector>

#include "thyme/runtime/pinned_slot_store.h"
#include "thyme/runtime/detail/atomic_payload.h"
#include "thyme/runtime/shm_region.h"

#ifndef THYME_BENCH_BUILD_TYPE
#define THYME_BENCH_BUILD_TYPE "unknown"
#endif

namespace {

constexpr std::size_t kRepeats = 3;
constexpr std::size_t kWarmup = 2;
constexpr std::size_t kAlignment = 64;
constexpr std::uint32_t kSlotCount = 4;
constexpr std::uint32_t kPressureFrames = 24;
constexpr std::uint64_t kOpenTimeoutNs = 1000000000ULL;
constexpr std::uint64_t kStealTimeoutNs = 60000000000ULL;

struct SizeCase {
  const char* label;
  std::uint32_t bytes;
  std::size_t iterations;
};

constexpr std::array<SizeCase, 3> kSizeCases{{
    {"240KiB", 240 * 1024, 24},
    {"1MiB", 1024 * 1024, 12},
    {"19MiB", 19 * 1024 * 1024, 3},
}};

enum class CopyPath : std::uint8_t {
  kMemcpy,
  kAtomic,
  kLease,
};

const char* PathName(CopyPath path) {
  switch(path) {
    case CopyPath::kMemcpy:
      return "memcpy";
    case CopyPath::kAtomic:
      return "atomic";
    case CopyPath::kLease:
      return "lease";
  }
  return "unknown";
}

class Buffer {
 public:
  Buffer() = default;
  Buffer(const Buffer&) = delete;
  Buffer& operator=(const Buffer&) = delete;
  ~Buffer() { std::free(data_); }

  bool Allocate(std::size_t bytes) {
    void* raw = nullptr;
    if(posix_memalign(&raw, kAlignment, bytes) != 0) {
      return false;
    }
    data_ = static_cast<std::uint8_t*>(raw);
    return true;
  }

  std::uint8_t* data() { return data_; }
  const std::uint8_t* data() const { return data_; }

 private:
  std::uint8_t* data_{nullptr};
};

class Channel {
 public:
  Channel() = default;
  Channel(const Channel&) = delete;
  Channel& operator=(const Channel&) = delete;
  ~Channel() { Reset(); }

  bool Open(const char* purpose, std::uint32_t bytes) {
    std::snprintf(name_, sizeof(name_), "/thyme_perf15_%s_%d", purpose, static_cast<int>(getpid()));
    thyme::ShmRegion::UnlinkName(name_);
    thyme::ShmRegion::UnlinkName(thyme::PinnedSlotWriter::LeaseShmName(name_).c_str());
    if(!writer_.Create(name_, bytes, kSlotCount, 0, kStealTimeoutNs) || !reader_.Open(name_, kOpenTimeoutNs)) {
      Reset();
      return false;
    }
    return true;
  }

  void Reset() {
    reader_.Close();
    writer_.Close();
    if(name_[0] != '\0') {
      thyme::ShmRegion::UnlinkName(name_);
      thyme::ShmRegion::UnlinkName(thyme::PinnedSlotWriter::LeaseShmName(name_).c_str());
      name_[0] = '\0';
    }
  }

  thyme::PinnedSlotWriter& writer() { return writer_; }
  thyme::PinnedSlotReader& reader() { return reader_; }

 private:
  char name_[128]{};
  thyme::PinnedSlotWriter writer_;
  thyme::PinnedSlotReader reader_;
};

struct Trial {
  std::array<double, kRepeats> ns_per_op{};
  bool ok{true};
};

struct CopyMetric {
  const SizeCase* size{nullptr};
  CopyPath path{CopyPath::kMemcpy};
  std::size_t iterations{0};
  double ns_per_op{0.0};
  double ratio_to_memcpy{1.0};
  bool ok{false};
};

struct PinMetric {
  const SizeCase* size{nullptr};
  CopyPath path{CopyPath::kAtomic};
  std::size_t operations{0};
  double pin_ns_per_op{0.0};
  bool ok{false};
};

struct PressureMetric {
  const SizeCase* size{nullptr};
  CopyPath path{CopyPath::kAtomic};
  std::uint64_t write_attempts{0};
  std::uint64_t frames_written{0};
  std::uint64_t pinned_skip{0};
  std::uint64_t lease_steal{0};
  std::uint64_t frames_copied{0};
  std::uint64_t copy_failures{0};
  std::uint64_t dropped_frames{0};
  std::uint64_t payload_mismatches{0};
  double pin_ns_per_op{0.0};
  bool ok{false};
};

std::uint64_t NowNs() {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
          .count());
}

double Median(const Trial& trial) {
  std::array<double, kRepeats> values = trial.ns_per_op;
  std::sort(values.begin(), values.end());
  return values[kRepeats / 2];
}

double MbPerSec(std::uint32_t bytes, double ns_per_op) {
  return ns_per_op > 0.0 ? static_cast<double>(bytes) * 1000.0 / ns_per_op : 0.0;
}

void FillPattern(std::uint8_t* data, std::size_t bytes, std::uint64_t frame_index) {
  const std::uint8_t tag = static_cast<std::uint8_t>(frame_index % 251u);
  std::memset(data, tag, bytes);
  if(bytes >= sizeof(frame_index)) {
    std::memcpy(data, &frame_index, sizeof(frame_index));
  }
}

bool PatternMatches(const std::uint8_t* data, std::size_t bytes, std::uint64_t frame_index) {
  const std::uint8_t tag = static_cast<std::uint8_t>(frame_index % 251u);
  const std::size_t begin = std::min(bytes, sizeof(frame_index));
  if(begin == sizeof(frame_index)) {
    std::uint64_t actual = 0;
    std::memcpy(&actual, data, sizeof(actual));
    if(actual != frame_index) {
      return false;
    }
  }
  for(std::size_t i = begin; i < bytes; ++i) {
    if(data[i] != tag) {
      return false;
    }
  }
  return true;
}

bool PublishFrame(thyme::PinnedSlotWriter& writer, std::uint32_t bytes, std::uint64_t frame_index,
                  bool* skipped = nullptr) {
  if(skipped != nullptr) {
    *skipped = false;
  }
  thyme::PinnedSlotWriter::BeginView view = writer.Begin(bytes);
  if(view.status != thyme::PinnedSlotWriter::BeginStatus::kOk || view.payload == nullptr) {
    if(skipped != nullptr) {
      *skipped = writer.pending_skipped();
    }
    writer.Commit();
    return false;
  }
  FillPattern(view.payload, bytes, frame_index);
  writer.Commit(frame_index * 1000ULL);
  return true;
}

bool CopyOnce(CopyPath path, const thyme::PinnedSlotReader::Lease& lease, const thyme::PinnedSlotReader::View& view,
              std::uint8_t* destination, std::size_t bytes) {
  switch(path) {
    case CopyPath::kMemcpy:
      std::memcpy(destination, view.payload, bytes);
      return true;
    case CopyPath::kAtomic:
      thyme::detail::AtomicPayloadCopyOut(destination, view.payload, bytes);
      return true;
    case CopyPath::kLease:
      return lease.CopyPayloadTo(destination, bytes);
  }
  return false;
}

Trial MeasureCopy(CopyPath path, const SizeCase& size_case) {
  Trial trial;
  Channel channel;
  Buffer destination;
  if(!channel.Open(PathName(path), size_case.bytes) || !destination.Allocate(size_case.bytes) ||
     !PublishFrame(channel.writer(), size_case.bytes, 7)) {
    trial.ok = false;
    return trial;
  }

  thyme::PinnedSlotReader::Lease lease = channel.reader().Acquire(0);
  if(!lease.valid() || lease.view().data_len != size_case.bytes) {
    trial.ok = false;
    return trial;
  }

  for(std::size_t round = 0; round < kRepeats; ++round) {
    std::uint64_t timed_ns = 0;
    for(std::size_t i = 0; i < kWarmup + size_case.iterations; ++i) {
      const bool timed = i >= kWarmup;
      const std::uint64_t begin = timed ? NowNs() : 0;
      const bool copied = CopyOnce(path, lease, lease.view(), destination.data(), size_case.bytes);
      const std::uint64_t end = timed ? NowNs() : 0;
      trial.ok = trial.ok && copied;
      if(timed) {
        timed_ns += end - begin;
      }
    }
    trial.ok = trial.ok && PatternMatches(destination.data(), size_case.bytes, 7);
    trial.ns_per_op[round] = static_cast<double>(timed_ns) / static_cast<double>(size_case.iterations);
  }
  lease.Reset();
  return trial;
}

PinMetric MeasurePin(CopyPath path, const SizeCase& size_case) {
  PinMetric metric;
  metric.size = &size_case;
  metric.path = path;
  metric.operations = size_case.iterations;
  Channel channel;
  Buffer destination;
  if(!channel.Open((std::string("pin_") + PathName(path)).c_str(), size_case.bytes) ||
     !destination.Allocate(size_case.bytes) || !PublishFrame(channel.writer(), size_case.bytes, 11)) {
    return metric;
  }

  std::uint64_t total_ns = 0;
  bool ok = true;
  for(std::size_t i = 0; i < size_case.iterations; ++i) {
    const std::uint64_t begin = NowNs();
    thyme::PinnedSlotReader::ReadResult status = thyme::PinnedSlotReader::ReadResult::kTorn;
    thyme::PinnedSlotReader::Lease lease = channel.reader().Acquire(0, &status);
    bool copied = false;
    if(lease.valid()) {
      copied = CopyOnce(path, lease, lease.view(), destination.data(), size_case.bytes);
    }
    const thyme::LeaseReleaseResult released = lease.Release();
    total_ns += NowNs() - begin;
    ok = ok && status == thyme::PinnedSlotReader::ReadResult::kOk && copied &&
         released == thyme::LeaseReleaseResult::kOk && PatternMatches(destination.data(), size_case.bytes, 11);
  }
  metric.pin_ns_per_op = static_cast<double>(total_ns) / static_cast<double>(metric.operations);
  metric.ok = ok;
  return metric;
}

PressureMetric MeasurePressure(CopyPath path, const SizeCase& size_case) {
  PressureMetric metric;
  metric.size = &size_case;
  metric.path = path;
  Channel channel;
  Buffer destination;
  Buffer expected;
  if(!channel.Open((std::string("pressure_") + PathName(path)).c_str(), size_case.bytes) ||
     !destination.Allocate(size_case.bytes) || !expected.Allocate(size_case.bytes)) {
    return metric;
  }

  std::atomic<bool> writer_ok{true};
  std::thread producer([&]() {
    for(std::uint64_t index = 0; index < kPressureFrames; ++index) {
      bool skipped = false;
      const bool written = PublishFrame(channel.writer(), size_case.bytes, index, &skipped);
      writer_ok.store(writer_ok.load(std::memory_order_relaxed) && (written || skipped), std::memory_order_relaxed);
    }
  });

  thyme::PinnedSlotReader& reader = channel.reader();
  std::uint64_t next = 0;
  std::uint64_t pin_ns = 0;
  std::uint64_t pin_operations = 0;
  std::uint64_t idle_rounds = 0;
  while(next < kPressureFrames && idle_rounds < 1000000) {
    const std::uint64_t head = reader.head();
    if(next >= head) {
      ++idle_rounds;
      std::this_thread::yield();
      continue;
    }
    idle_rounds = 0;
    if(head - next > kSlotCount) {
      const std::uint64_t lost = head - kSlotCount - next;
      metric.dropped_frames += lost;
      next += lost;
    }
    const std::uint64_t index = next++;
    ++pin_operations;
    const std::uint64_t begin = NowNs();
    thyme::PinnedSlotReader::ReadResult status = thyme::PinnedSlotReader::ReadResult::kTorn;
    thyme::PinnedSlotReader::Lease lease = reader.Acquire(index, &status);
    bool copied = false;
    if(lease.valid()) {
      copied = CopyOnce(path, lease, lease.view(), destination.data(), size_case.bytes);
    }
    const thyme::LeaseReleaseResult released = lease.Release();
    pin_ns += NowNs() - begin;
    if(status == thyme::PinnedSlotReader::ReadResult::kOk) {
      if(copied && released == thyme::LeaseReleaseResult::kOk) {
        ++metric.frames_copied;
        FillPattern(expected.data(), size_case.bytes, index);
        if(std::memcmp(destination.data(), expected.data(), size_case.bytes) != 0) {
          ++metric.payload_mismatches;
        }
      } else {
        ++metric.copy_failures;
      }
    } else if(status == thyme::PinnedSlotReader::ReadResult::kNotPublished) {
      --next;
    } else {
      ++metric.dropped_frames;
    }
  }
  producer.join();

  metric.write_attempts = kPressureFrames;
  metric.frames_written = kPressureFrames - channel.writer().pinned_skip_count();
  metric.pinned_skip = channel.writer().pinned_skip_count();
  metric.lease_steal = channel.writer().lease()->lease_steal.load(std::memory_order_relaxed);
  metric.pin_ns_per_op = pin_operations != 0 ? static_cast<double>(pin_ns) / static_cast<double>(pin_operations) : 0.0;
  metric.ok = writer_ok.load(std::memory_order_relaxed) && next == kPressureFrames && metric.copy_failures == 0 &&
              metric.payload_mismatches == 0;
  return metric;
}

std::string Trim(const std::string& value) {
  const std::size_t begin = value.find_first_not_of(" \t\r\n");
  if(begin == std::string::npos) {
    return "";
  }
  const std::size_t end = value.find_last_not_of(" \t\r\n");
  return value.substr(begin, end - begin + 1);
}

std::string ArchName() {
  struct utsname info {};
  if(uname(&info) != 0) {
    return "unknown";
  }
  return info.machine;
}

std::string RunnerTag() {
  const char* env = std::getenv("THYME_BENCH_RUNNER_TAG");
  if(env != nullptr && *env != '\0') {
    return env;
  }
  const char* ci = std::getenv("CI");
  return ArchName() + ((ci != nullptr && *ci != '\0') ? "-ci" : "-local");
}

std::string GitSha() {
  FILE* pipe_file = popen("git rev-parse --short HEAD 2>/dev/null", "r");
  if(pipe_file == nullptr) {
    return "unknown";
  }
  char buffer[64] = {0};
  const char* line = fgets(buffer, sizeof(buffer), pipe_file);
  pclose(pipe_file);
  if(line == nullptr) {
    return "unknown";
  }
  const std::string sha = Trim(buffer);
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
  FILE* file = std::fopen("/proc/cpuinfo", "r");
  if(file == nullptr) {
    return "unknown";
  }
  char line[512];
  std::string model;
  while(fgets(line, sizeof(line), file) != nullptr) {
    if(std::strncmp(line, "model name", 10) == 0) {
      const char* colon = std::strchr(line, ':');
      if(colon != nullptr) {
        model = Trim(colon + 1);
        break;
      }
    }
  }
  std::fclose(file);
  return model.empty() ? "unknown" : model;
}

std::string TimestampUtc() {
  const std::time_t now = std::time(nullptr);
  struct tm tm_utc {};
  gmtime_r(&now, &tm_utc);
  char buffer[32];
  std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &tm_utc);
  return buffer;
}

std::string JsonEscape(const std::string& value) {
  std::string out;
  out.reserve(value.size() + 8);
  for(const char c : value) {
    switch(c) {
      case '"':
        out += "\\\"";
        break;
      case '\\':
        out += "\\\\";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\r':
        out += "\\r";
        break;
      case '\t':
        out += "\\t";
        break;
      default:
        out += c;
    }
  }
  return out;
}

void AppendJsonRow(std::string* out, const char* row, bool comma) {
  *out += row;
  *out += comma ? ",\n" : "\n";
}

bool EmitJson(const std::string& path, const std::string& tag, const std::vector<CopyMetric>& copies,
              const std::vector<PinMetric>& pins, const std::vector<PressureMetric>& pressures, bool all_ok) {
  char hostname[256] = {0};
  gethostname(hostname, sizeof(hostname) - 1);
  std::string out = "{\n";
  out += "  \"schema\": 1,\n";
  out += "  \"kind\": \"thyme-bench-pinned_slot-copy\",\n";
  out += "  \"generated_at\": \"" + TimestampUtc() + "\",\n";
  out += "  \"git_sha\": \"" + JsonEscape(GitSha()) + "\",\n";
  out += "  \"runner_tag\": \"" + JsonEscape(tag) + "\",\n";
  out += "  \"host\": {\"hostname\": \"" + JsonEscape(hostname) + "\", \"machine\": \"" + JsonEscape(ArchName()) +
         "\"},\n";
  out += "  \"cpu\": \"" + JsonEscape(CpuModel()) + "\",\n";
  out += "  \"toolchain\": \"" + JsonEscape(Toolchain()) + "\",\n";
  out += "  \"build_type\": \"" + std::string(THYME_BENCH_BUILD_TYPE) + "\",\n";
  out += "  \"repeats\": 3,\n";
  out += "  \"statistic\": \"median_of_batch_ns_per_op\",\n";
  out += "  \"ok\": " + std::string(all_ok ? "true" : "false") + ",\n";
  out += "  \"copy_benchmarks\": [\n";
  for(std::size_t i = 0; i < copies.size(); ++i) {
    const CopyMetric& metric = copies[i];
    char row[1024];
    std::snprintf(row, sizeof(row),
                  "    {\"size\": \"%s\", \"bytes_per_op\": %u, \"path\": \"%s\", "
                  "\"iterations\": %llu, \"ns_per_op\": %.4f, \"mb_per_sec\": %.4f, "
                  "\"ratio_to_memcpy\": %.6f, \"scope\": \"copy_body_only\", \"ok\": %s}",
                  metric.size->label, metric.size->bytes, PathName(metric.path),
                  static_cast<unsigned long long>(metric.iterations), metric.ns_per_op,
                  MbPerSec(metric.size->bytes, metric.ns_per_op), metric.ratio_to_memcpy, metric.ok ? "true" : "false");
    AppendJsonRow(&out, row, i + 1 < copies.size());
  }
  out += "  ],\n  \"pin_benchmarks\": [\n";
  for(std::size_t i = 0; i < pins.size(); ++i) {
    const PinMetric& metric = pins[i];
    char row[768];
    std::snprintf(row, sizeof(row),
                  "    {\"size\": \"%s\", \"path\": \"%s\", \"operations\": %llu, "
                  "\"pin_ns_per_op\": %.4f, \"scope\": \"acquire_copy_release\", \"ok\": "
                  "%s}",
                  metric.size->label, PathName(metric.path), static_cast<unsigned long long>(metric.operations),
                  metric.pin_ns_per_op, metric.ok ? "true" : "false");
    AppendJsonRow(&out, row, i + 1 < pins.size());
  }
  out += "  ],\n  \"producer_pressure\": [\n";
  for(std::size_t i = 0; i < pressures.size(); ++i) {
    const PressureMetric& metric = pressures[i];
    char row[1400];
    std::snprintf(
        row, sizeof(row),
        "    {\"size\": \"%s\", \"path\": \"%s\", \"write_attempts\": %llu, "
        "\"frames_written\": %llu, \"pinned_skip\": %llu, \"lease_steal\": "
        "%llu, "
        "\"frames_copied\": %llu, \"copy_failures\": %llu, \"dropped_frames\": "
        "%llu, "
        "\"payload_mismatches\": %llu, \"pin_ns_per_op\": %.4f, \"ok\": %s}",
        metric.size->label, PathName(metric.path), static_cast<unsigned long long>(metric.write_attempts),
        static_cast<unsigned long long>(metric.frames_written), static_cast<unsigned long long>(metric.pinned_skip),
        static_cast<unsigned long long>(metric.lease_steal), static_cast<unsigned long long>(metric.frames_copied),
        static_cast<unsigned long long>(metric.copy_failures), static_cast<unsigned long long>(metric.dropped_frames),
        static_cast<unsigned long long>(metric.payload_mismatches), metric.pin_ns_per_op, metric.ok ? "true" : "false");
    AppendJsonRow(&out, row, i + 1 < pressures.size());
  }
  out += "  ]\n}\n";
  FILE* file = std::fopen(path.c_str(), "w");
  if(file == nullptr) {
    return false;
  }
  const bool write_ok = std::fwrite(out.data(), 1, out.size(), file) == out.size();
  const bool close_ok = std::fclose(file) == 0;
  return write_ok && close_ok;
}

void PrintCopyTable(const std::vector<CopyMetric>& metrics) {
  std::printf(
      "\nCopy body only (median of 3 batches; ratio is throughput / "
      "memcpy throughput)\n");
  std::printf("%-10s %-10s %10s %14s %12s %10s  %s\n", "size", "path", "iters", "ns/op", "MB/s", "vs memcpy", "check");
  for(const CopyMetric& metric : metrics) {
    std::printf("%-10s %-10s %10llu %14.3f %12.2f %9.2f%%  %s\n", metric.size->label, PathName(metric.path),
                static_cast<unsigned long long>(metric.iterations), metric.ns_per_op,
                MbPerSec(metric.size->bytes, metric.ns_per_op), metric.ratio_to_memcpy * 100.0,
                metric.ok ? "OK" : "BAD");
  }
}

void PrintPinTable(const std::vector<PinMetric>& metrics) {
  std::printf(
      "\nLease pin duration (AcquireLease + copy + ReleaseLease; not a "
      "copy-body metric)\n");
  std::printf("%-10s %-10s %10s %16s  %s\n", "size", "path", "ops", "pin ns/op", "check");
  for(const PinMetric& metric : metrics) {
    std::printf("%-10s %-10s %10llu %16.3f  %s\n", metric.size->label, PathName(metric.path),
                static_cast<unsigned long long>(metric.operations), metric.pin_ns_per_op, metric.ok ? "OK" : "BAD");
  }
}

void PrintPressureTable(const std::vector<PressureMetric>& metrics) {
  std::printf(
      "\nProducer pressure (24 attempted frames, 4 slots; counters are "
      "observations, not timing gates)\n");
  std::printf("%-8s %-7s %6s %6s %6s %6s %7s %6s %6s %12s  %s\n", "size", "path", "write", "skip", "steal", "copy",
              "copybad", "drop", "mismatch", "pin ns/op", "check");
  for(const PressureMetric& metric : metrics) {
    std::printf(
        "%-8s %-7s %6llu %6llu %6llu %6llu %7llu %6llu %6llu %12.1f  %s\n", metric.size->label, PathName(metric.path),
        static_cast<unsigned long long>(metric.frames_written), static_cast<unsigned long long>(metric.pinned_skip),
        static_cast<unsigned long long>(metric.lease_steal), static_cast<unsigned long long>(metric.frames_copied),
        static_cast<unsigned long long>(metric.copy_failures), static_cast<unsigned long long>(metric.dropped_frames),
        static_cast<unsigned long long>(metric.payload_mismatches), metric.pin_ns_per_op, metric.ok ? "OK" : "BAD");
  }
}

}  // namespace

int main(int argc, char** argv) {
  std::string json_path;
  std::string tag;
  for(int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if(arg == "--json" && i + 1 < argc) {
      json_path = argv[++i];
    } else if(arg == "--tag" && i + 1 < argc) {
      tag = argv[++i];
    } else if(arg == "-h" || arg == "--help") {
      std::printf(
          "usage: bench_pinned_slot_copy [--json PATH] [--tag RUNNER_TAG]\n"
          "  THYME_BENCH_RUNNER_TAG sets the default tag\n");
      return 0;
    } else {
      std::fprintf(stderr, "bench_pinned_slot_copy: unknown or incomplete argument %s\n", arg.c_str());
      return 2;
    }
  }
  if(tag.empty()) {
    tag = RunnerTag();
  }

  std::vector<CopyMetric> copies;
  std::vector<PinMetric> pins;
  std::vector<PressureMetric> pressures;
  bool ok = true;
  for(const SizeCase& size_case : kSizeCases) {
    const std::array<CopyPath, 3> paths{{CopyPath::kMemcpy, CopyPath::kAtomic, CopyPath::kLease}};
    for(const CopyPath path : paths) {
      const Trial trial = MeasureCopy(path, size_case);
      copies.push_back({&size_case, path, size_case.iterations, Median(trial), 1.0, trial.ok});
      ok = ok && trial.ok;
    }
    const double memcpy_ns = copies[copies.size() - 3].ns_per_op;
    for(std::size_t i = copies.size() - 3; i < copies.size(); ++i) {
      copies[i].ratio_to_memcpy = copies[i].ns_per_op > 0.0 ? memcpy_ns / copies[i].ns_per_op : 0.0;
    }
    for(const CopyPath path : {CopyPath::kAtomic, CopyPath::kLease}) {
      const PinMetric pin = MeasurePin(path, size_case);
      ok = ok && pin.ok;
      pins.push_back(pin);
    }
    for(const CopyPath path : {CopyPath::kAtomic, CopyPath::kLease}) {
      const PressureMetric pressure = MeasurePressure(path, size_case);
      ok = ok && pressure.ok;
      pressures.push_back(pressure);
    }
  }

  std::printf("bench_pinned_slot_copy: runner=%s git=%s toolchain=%s build=%s\n", tag.c_str(), GitSha().c_str(),
              Toolchain().c_str(), THYME_BENCH_BUILD_TYPE);
  PrintCopyTable(copies);
  PrintPinTable(pins);
  PrintPressureTable(pressures);
  if(!json_path.empty() && !EmitJson(json_path, tag, copies, pins, pressures, ok)) {
    std::fprintf(stderr, "bench_pinned_slot_copy: cannot write %s\n", json_path.c_str());
    ok = false;
  }
  if(!ok) {
    std::fprintf(stderr, "bench_pinned_slot_copy: a measured path failed correctness\n");
    return 1;
  }
  return 0;
}
