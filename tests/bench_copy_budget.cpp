// PERF-07: reproducible CRC and real in-process/cross-process/tap copy budget
// evidence. Every path checks payload and metadata correctness; wall-clock
// magnitude is never a pass/fail condition. Pipe round trips synchronize the
// two processes but sit outside the timed publish/read regions.

#include "thyme/contracts/frame_header.h"
#include "thyme/runtime/notif_pipe.h"
#include "thyme/runtime/record_mirror_ring.h"
#include "thyme/runtime/shm_region.h"
#include "thyme/runtime/shm_spmc_ring.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <sys/types.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

#ifndef THYME_BENCH_BUILD_TYPE
#define THYME_BENCH_BUILD_TYPE "unknown"
#endif

namespace {

constexpr std::size_t kRepeats = 3;
constexpr std::size_t kWarmup = 4;
constexpr std::size_t kCrcIterations = 30;
constexpr std::size_t kCrcBytes = 240 * 1024;

struct Trial {
  std::array<double, kRepeats> ns_per_op{};
  bool ok{true};
};

struct ProtocolResult {
  Trial writer;
  Trial reader;
  bool ok{true};
};

struct Metric {
  std::string name;
  std::string path;
  std::string process;
  std::string includes;
  std::size_t iterations{0};
  std::size_t bytes_per_op{0};
  double ns_per_op{0.0};
  bool ok{true};
};

double NowNs() {
  return std::chrono::duration<double, std::nano>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

double Median(const Trial &trial) {
  std::array<double, kRepeats> values = trial.ns_per_op;
  std::sort(values.begin(), values.end());
  return values[kRepeats / 2];
}

bool WriteFull(int fd, const void *data, std::size_t bytes) {
  const auto *p = static_cast<const std::uint8_t *>(data);
  std::size_t done = 0;
  while (done < bytes) {
    const ssize_t n = write(fd, p + done, bytes - done);
    if (n < 0 && errno == EINTR) {
      continue;
    }
    if (n <= 0) {
      return false;
    }
    done += static_cast<std::size_t>(n);
  }
  return true;
}

bool ReadFull(int fd, void *data, std::size_t bytes) {
  auto *p = static_cast<std::uint8_t *>(data);
  std::size_t done = 0;
  while (done < bytes) {
    const ssize_t n = read(fd, p + done, bytes - done);
    if (n < 0 && errno == EINTR) {
      continue;
    }
    if (n <= 0) {
      return false;
    }
    done += static_cast<std::size_t>(n);
  }
  return true;
}

template <std::size_t N> struct alignas(8) Payload {
  static_assert(N > sizeof(std::uint64_t), "payload needs a body");
  std::uint64_t generation{0};
  std::array<std::uint8_t, N - sizeof(std::uint64_t)> body{};
};

template <typename T> T MakePayload() {
  T value{};
  for (std::size_t i = 0; i < value.body.size(); ++i) {
    value.body[i] = static_cast<std::uint8_t>((i * 31u + 7u) & 0xFFu);
  }
  return value;
}

template <typename T>
bool PayloadMatches(const T &actual, const T &expected,
                    std::uint64_t generation) {
  return actual.generation == generation &&
         std::memcmp(actual.body.data(), expected.body.data(),
                     expected.body.size()) == 0;
}

// One in-process operation publishes and pops before the next begins. There is
// no waiter, cross-thread handoff, or backpressure in the timed region; full
// payload validation stays outside it.
template <typename T, typename RoundTripFn>
Trial MeasureNotifPipe(const T &expected, std::size_t iterations,
                       RoundTripFn &&round_trip) {
  thyme::NotifPipe<T, 2> pipe;
  T source = expected;
  T output{};
  Trial trial;
  std::uint64_t sequence = 0;
  for (std::size_t round = 0; round < kRepeats; ++round) {
    double timed_ns = 0.0;
    for (std::size_t i = 0; i < kWarmup + iterations; ++i) {
      source.generation = ++sequence;
      const bool timed = i >= kWarmup;
      const double t0 = timed ? NowNs() : 0.0;
      const bool round_trip_ok = round_trip(pipe, source, output);
      const double t1 = timed ? NowNs() : 0.0;
      trial.ok = trial.ok && round_trip_ok &&
                 PayloadMatches(output, expected, sequence);
      if (timed) {
        timed_ns += t1 - t0;
      }
    }
    trial.ns_per_op[round] = timed_ns / static_cast<double>(iterations);
  }
  return trial;
}

// The child publishes into its inherited writer mapping. The parent consumes
// through a separately opened mapping. One acknowledged item at a time avoids
// overwrite drops without putting IPC latency in either reported operation.
template <typename T, typename WriteFn, typename ReadFn>
ProtocolResult RunProtocol(const T &expected, std::size_t iterations,
                           WriteFn &&write_op, ReadFn &&read_op) {
  ProtocolResult result;
  int published[2] = {-1, -1};
  int consumed[2] = {-1, -1};
  if (pipe(published) != 0 || pipe(consumed) != 0) {
    result.ok = false;
    return result;
  }

  const pid_t pid = fork();
  if (pid < 0) {
    result.ok = false;
    return result;
  }
  if (pid == 0) {
    close(published[0]);
    close(consumed[1]);
    T source = expected;
    Trial trial;
    std::uint64_t sequence = 0;
    for (std::size_t round = 0; round < kRepeats; ++round) {
      double timed_ns = 0.0;
      for (std::size_t i = 0; i < kWarmup + iterations; ++i) {
        source.generation = ++sequence;
        const bool timed = i >= kWarmup;
        const double t0 = timed ? NowNs() : 0.0;
        const bool write_ok = write_op(source, sequence);
        const double t1 = timed ? NowNs() : 0.0;
        trial.ok = trial.ok && write_ok;
        if (timed) {
          timed_ns += t1 - t0;
        }
        const char token = 'P';
        if (!WriteFull(published[1], &token, sizeof(token))) {
          _exit(2);
        }
        char ack = 0;
        if (!ReadFull(consumed[0], &ack, sizeof(ack)) || ack != 'C') {
          _exit(2);
        }
      }
      trial.ns_per_op[round] = timed_ns / static_cast<double>(iterations);
    }
    if (!WriteFull(published[1], &trial, sizeof(trial))) {
      _exit(2);
    }
    _exit(trial.ok ? 0 : 1);
  }

  close(published[1]);
  close(consumed[0]);
  T output{};
  std::uint64_t sequence = 0;
  for (std::size_t round = 0; round < kRepeats; ++round) {
    double timed_ns = 0.0;
    for (std::size_t i = 0; i < kWarmup + iterations; ++i) {
      char token = 0;
      if (!ReadFull(published[0], &token, sizeof(token)) || token != 'P') {
        result.ok = false;
        break;
      }
      ++sequence;
      const bool timed = i >= kWarmup;
      const double t0 = timed ? NowNs() : 0.0;
      const bool read_ok = read_op(output, sequence);
      const double t1 = timed ? NowNs() : 0.0;
      result.reader.ok = result.reader.ok && read_ok &&
                         PayloadMatches(output, expected, sequence);
      if (timed) {
        timed_ns += t1 - t0;
      }
      const char ack = 'C';
      if (!WriteFull(consumed[1], &ack, sizeof(ack))) {
        result.ok = false;
        break;
      }
    }
    if (!result.ok) {
      break;
    }
    result.reader.ns_per_op[round] = timed_ns / static_cast<double>(iterations);
  }

  if (result.ok &&
      !ReadFull(published[0], &result.writer, sizeof(result.writer))) {
    result.ok = false;
  }
  close(published[0]);
  close(consumed[1]);
  int status = 0;
  if (waitpid(pid, &status, 0) != pid || !WIFEXITED(status) ||
      WEXITSTATUS(status) != 0) {
    result.ok = false;
  }
  result.ok = result.ok && result.writer.ok && result.reader.ok;
  return result;
}

void AddMetric(std::vector<Metric> &metrics, const std::string &name,
               const std::string &path, const std::string &process,
               const std::string &includes, std::size_t iterations,
               std::size_t bytes, const Trial &trial, bool protocol_ok) {
  metrics.push_back({name, path, process, includes, iterations, bytes,
                     Median(trial), protocol_ok && trial.ok});
}

template <typename T>
bool RunNotifPipeCase(const char *size_name, std::size_t iterations,
                      std::vector<Metric> &metrics) {
  const T expected = MakePayload<T>();
  const Trial copy = MeasureNotifPipe<T>(
      expected, iterations,
      [](thyme::NotifPipe<T, 2> &pipe, const T &source, T &output) {
        return pipe.Push(source) && pipe.TryPop(output);
      });
  const Trial inplace = MeasureNotifPipe<T>(
      expected, iterations,
      [](thyme::NotifPipe<T, 2> &pipe, const T &source, T &output) {
        T *slot = pipe.BeginWrite();
        if (slot == nullptr) {
          return false;
        }
        slot->generation = source.generation;
        std::memcpy(slot->body.data(), source.body.data(), source.body.size());
        pipe.CommitWrite();
        return pipe.TryPop(output);
      });

  AddMetric(metrics, std::string("notif_pipe_copy_round_trip_") + size_name,
            "NotifPipe::Push + TryPop", "single process",
            "Push copy-in; CommitWrite; TryPop copy-out; no Notify target/wait",
            iterations, sizeof(T), copy, true);
  AddMetric(metrics, std::string("notif_pipe_inplace_round_trip_") + size_name,
            "NotifPipe::BeginWrite/fill/CommitWrite + TryPop", "single process",
            "BeginWrite; caller payload fill; CommitWrite; TryPop copy-out; no "
            "Notify target/wait",
            iterations, sizeof(T), inplace, true);
  return copy.ok && inplace.ok;
}

template <typename T>
bool RunTransportCase(const char *size_name, std::size_t iterations,
                      std::vector<Metric> &metrics) {
  char name[96];
  std::snprintf(name, sizeof(name), "/thyme_perf07_spmc_%s_%d", size_name,
                static_cast<int>(getpid()));
  thyme::ShmRegion::UnlinkName(name);
  thyme::ShmSpmcRing<T, 2> writer;
  thyme::ShmSpmcRing<T, 2> reader_ring;
  thyme::SpmcReader<T, 2> reader;
  if (!writer.Create(name) || !reader_ring.Open(name, 1000000000ULL) ||
      !reader.Bind(&reader_ring, thyme::SpmcRead::Latest)) {
    thyme::ShmRegion::UnlinkName(name);
    return false;
  }

  const T expected = MakePayload<T>();
  const ProtocolResult result = RunProtocol<T>(
      expected, iterations,
      [&](const T &source, std::uint64_t) {
        writer.Push(source);
        return true;
      },
      [&](T &output, std::uint64_t) { return reader.ReadLatest(output); });

  AddMetric(metrics, std::string("shm_spmc_push_") + size_name,
            "ShmSpmcRing::Push", "child",
            "AtomicPayloadCopyIn; seq/head publish; Notify; no tap; no CRC",
            iterations, sizeof(T), result.writer, result.ok);
  AddMetric(
      metrics, std::string("shm_spmc_read_latest_") + size_name,
      "SpmcReader::ReadLatest", "parent",
      "head/seq validation; AtomicPayloadCopyOut; seq recheck; no pipe wait",
      iterations, sizeof(T), result.reader, result.ok);
  reader_ring.Close();
  writer.Close();
  thyme::ShmRegion::UnlinkName(name);
  return result.ok;
}

template <typename T>
bool RunTapCase(const char *size_name, std::size_t iterations,
                std::vector<Metric> &metrics) {
  char name[96];
  std::snprintf(name, sizeof(name), "/thyme_perf07_tap_%s_%d", size_name,
                static_cast<int>(getpid()));
  thyme::ShmRegion::UnlinkName(name);
  thyme::ThymeStructDesc desc{};
  desc.contract_version = 1;
  desc.payload_size = sizeof(T);
  std::snprintf(desc.name, sizeof(desc.name), "Perf07%s", size_name);
  const thyme::RecordMirrorRingSpec spec{sizeof(T), 4};
  const std::uint64_t bytes =
      thyme::RecordMirrorRingPage::RequiredBytes(&desc, &spec, 1);
  thyme::ShmRegion writer_region;
  thyme::ShmRegion reader_region;
  if (bytes == 0 || !writer_region.Create(name, bytes) ||
      !reader_region.OpenRetry(name, bytes, 1000000000ULL, true)) {
    thyme::ShmRegion::UnlinkName(name);
    return false;
  }
  auto *writer =
      static_cast<thyme::RecordMirrorRingPage *>(writer_region.data());
  const auto *reader =
      static_cast<const thyme::RecordMirrorRingPage *>(reader_region.data());
  if (!writer->Init(7, &desc, &spec, 1) || !reader->Validate()) {
    writer_region.Unmap();
    reader_region.Unmap();
    thyme::ShmRegion::UnlinkName(name);
    return false;
  }

  const T expected = MakePayload<T>();
  const ProtocolResult result = RunProtocol<T>(
      expected, iterations,
      [&](const T &source, std::uint64_t sequence) {
        return writer->TryWriteRing(0, &source, sizeof(T), sequence * 10,
                                    static_cast<std::uint32_t>(sequence));
      },
      [&](T &output, std::uint64_t sequence) {
        std::uint32_t size = 0;
        std::uint64_t mono_ns = 0;
        std::uint32_t source_sequence = 0;
        return reader->ReadRingSample(
                   0, sequence - 1, reinterpret_cast<std::uint8_t *>(&output),
                   sizeof(T), size, mono_ns, source_sequence) &&
               size == sizeof(T) && mono_ns == sequence * 10 &&
               source_sequence == static_cast<std::uint32_t>(sequence);
      });

  AddMetric(metrics, std::string("record_tap_try_write_") + size_name,
            "RecordMirrorRingPage::TryWriteRing", "child",
            "CAS seqlock; metadata; AtomicPayloadCopyIn; ring_head publish",
            iterations, sizeof(T), result.writer, result.ok);
  AddMetric(metrics, std::string("record_tap_read_") + size_name,
            "RecordMirrorRingPage::ReadRingSample", "parent read-only SHM",
            "index/capacity checks; AtomicPayloadCopyOut; seq recheck; no "
            "harvest/serialization",
            iterations, sizeof(T), result.reader, result.ok);
  writer_region.Unmap();
  reader_region.Unmap();
  thyme::ShmRegion::UnlinkName(name);
  return result.ok;
}

Trial MeasureCrc(std::uint32_t (*fn)(std::uint32_t, const std::uint8_t *,
                                     std::size_t),
                 const std::vector<std::uint8_t> &data,
                 std::uint32_t *final_crc) {
  Trial trial;
  for (std::size_t round = 0; round < kRepeats; ++round) {
    std::uint32_t crc = 0xFFFFFFFFu;
    for (std::size_t i = 0; i < kWarmup; ++i) {
      crc = fn(crc, data.data(), data.size());
    }
    const double t0 = NowNs();
    for (std::size_t i = 0; i < kCrcIterations; ++i) {
      crc = fn(crc, data.data(), data.size());
    }
    const double t1 = NowNs();
    trial.ns_per_op[round] = (t1 - t0) / static_cast<double>(kCrcIterations);
    *final_crc = crc;
  }
  return trial;
}

bool RunCrc(std::vector<Metric> &metrics) {
  std::vector<std::uint8_t> frame(kCrcBytes);
  for (std::size_t i = 0; i < frame.size(); ++i) {
    frame[i] = static_cast<std::uint8_t>((i * 17u + 23u) & 0xFFu);
  }
  const std::uint32_t reference = thyme::detail::Crc32UpdateBitwise(
                                      0xFFFFFFFFu, frame.data(), frame.size()) ^
                                  0xFFFFFFFFu;
  const bool correct = thyme::Crc32(frame.data(), frame.size()) == reference;
  std::uint32_t dispatched_final = 0;
  std::uint32_t bitwise_final = 0;
  const Trial dispatched =
      MeasureCrc(thyme::Crc32Update, frame, &dispatched_final);
  const Trial bitwise =
      MeasureCrc(thyme::detail::Crc32UpdateBitwise, frame, &bitwise_final);
  const bool ok = correct && dispatched_final == bitwise_final;
  AddMetric(metrics, "crc32_240kib_dispatched", "Crc32Update", "single process",
            "CRC only; no frame publish or copy", kCrcIterations, kCrcBytes,
            dispatched, ok);
  AddMetric(metrics, "crc32_240kib_bitwise", "Crc32UpdateBitwise",
            "single process", "reference CRC only; no frame publish or copy",
            kCrcIterations, kCrcBytes, bitwise, ok);
  return ok;
}

std::string Trim(const std::string &value) {
  const auto begin = value.find_first_not_of(" \t\r\n");
  if (begin == std::string::npos) {
    return "";
  }
  const auto end = value.find_last_not_of(" \t\r\n");
  return value.substr(begin, end - begin + 1);
}

std::string ArchName() {
  struct utsname info {};
  if (uname(&info) != 0) {
    return "unknown";
  }
  return info.machine;
}

std::string RunnerTag() {
  const char *env = std::getenv("THYME_BENCH_RUNNER_TAG");
  if (env != nullptr && *env != '\0') {
    return env;
  }
  const char *ci = std::getenv("CI");
  return ArchName() + ((ci != nullptr && *ci != '\0') ? "-ci" : "-local");
}

std::string GitSha() {
  FILE *pipe_file = popen("git rev-parse --short HEAD 2>/dev/null", "r");
  if (pipe_file == nullptr) {
    return "unknown";
  }
  char buffer[64] = {0};
  const char *line = fgets(buffer, sizeof(buffer), pipe_file);
  pclose(pipe_file);
  if (line == nullptr) {
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
  FILE *file = std::fopen("/proc/cpuinfo", "r");
  if (file == nullptr) {
    return "unknown";
  }
  char line[512];
  std::string model;
  while (fgets(line, sizeof(line), file) != nullptr) {
    if (std::strncmp(line, "model name", 10) == 0) {
      const char *colon = std::strchr(line, ':');
      if (colon != nullptr) {
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

std::string JsonEscape(const std::string &value) {
  std::string out;
  out.reserve(value.size() + 8);
  for (const char c : value) {
    switch (c) {
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

bool EmitJson(const std::string &path, const std::string &tag,
              const std::vector<Metric> &metrics, bool all_ok) {
  char hostname[256] = {0};
  gethostname(hostname, sizeof(hostname) - 1);
  std::string out = "{\n";
  out += "  \"schema\": 1,\n";
  out += "  \"kind\": \"thyme-bench-copy-budget\",\n";
  out += "  \"generated_at\": \"" + TimestampUtc() + "\",\n";
  out += "  \"git_sha\": \"" + JsonEscape(GitSha()) + "\",\n";
  out += "  \"runner_tag\": \"" + JsonEscape(tag) + "\",\n";
  out += "  \"host\": {\"hostname\": \"" + JsonEscape(hostname) +
         "\", \"machine\": \"" + JsonEscape(ArchName()) + "\"},\n";
  out += "  \"cpu\": \"" + JsonEscape(CpuModel()) + "\",\n";
  out += "  \"toolchain\": \"" + JsonEscape(Toolchain()) + "\",\n";
  out += "  \"build_type\": \"" + std::string(THYME_BENCH_BUILD_TYPE) + "\",\n";
  out += "  \"repeats\": 3,\n";
  out += "  \"statistic\": \"median_of_batch_ns_per_op\",\n";
  out += "  \"crc32_hw_accelerated\": " +
         std::string(thyme::detail::Crc32HwAccelerated() ? "true" : "false") +
         ",\n";
  out += "  \"ok\": " + std::string(all_ok ? "true" : "false") + ",\n";
  out += "  \"benchmarks\": [\n";
  for (std::size_t i = 0; i < metrics.size(); ++i) {
    const Metric &metric = metrics[i];
    char row[2048];
    std::snprintf(
        row, sizeof(row),
        "    {\"name\": \"%s\", \"path\": \"%s\", \"process\": \"%s\", "
        "\"iterations\": %llu, \"repeats\": 3, \"bytes_per_op\": %llu, "
        "\"ns_per_op\": %.4f, \"mb_per_sec\": %.4f, "
        "\"includes\": \"%s\", \"ok\": %s}",
        JsonEscape(metric.name).c_str(), JsonEscape(metric.path).c_str(),
        JsonEscape(metric.process).c_str(),
        static_cast<unsigned long long>(metric.iterations),
        static_cast<unsigned long long>(metric.bytes_per_op), metric.ns_per_op,
        static_cast<double>(metric.bytes_per_op) * 1000.0 / metric.ns_per_op,
        JsonEscape(metric.includes).c_str(), metric.ok ? "true" : "false");
    out += row;
    if (i + 1 < metrics.size()) {
      out += ",";
    }
    out += "\n";
  }
  out += "  ]\n}\n";
  FILE *file = std::fopen(path.c_str(), "w");
  if (file == nullptr) {
    return false;
  }
  const bool write_ok =
      std::fwrite(out.data(), 1, out.size(), file) == out.size();
  const bool close_ok = std::fclose(file) == 0;
  return write_ok && close_ok;
}

} // namespace

int main(int argc, char **argv) {
  std::string json_path;
  std::string tag;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--json" && i + 1 < argc) {
      json_path = argv[++i];
    } else if (arg == "--tag" && i + 1 < argc) {
      tag = argv[++i];
    } else if (arg == "-h" || arg == "--help") {
      std::printf("usage: bench_copy_budget [--json PATH] [--tag RUNNER_TAG]\n"
                  "  THYME_BENCH_RUNNER_TAG sets the default tag\n");
      return 0;
    } else {
      std::fprintf(stderr, "bench_copy_budget: unknown argument %s\n",
                   arg.c_str());
      return 2;
    }
  }
  if (tag.empty()) {
    tag = RunnerTag();
  }

  std::vector<Metric> metrics;
  bool ok = RunCrc(metrics);
  ok = RunNotifPipeCase<Payload<512>>("512b", 2000, metrics) && ok;
  ok = RunNotifPipeCase<Payload<16 * 1024>>("16kib", 800, metrics) && ok;
  ok = RunNotifPipeCase<Payload<240 * 1024>>("240kib", 1000, metrics) && ok;
  ok = RunTransportCase<Payload<512>>("512b", 2000, metrics) && ok;
  ok = RunTransportCase<Payload<16 * 1024>>("16kib", 800, metrics) && ok;
  ok = RunTransportCase<Payload<240 * 1024>>("240kib", 1000, metrics) && ok;
  ok = RunTapCase<Payload<512>>("512b", 2000, metrics) && ok;
  ok = RunTapCase<Payload<16 * 1024>>("16kib", 800, metrics) && ok;
  ok = RunTapCase<Payload<240 * 1024>>("240kib", 1000, metrics) && ok;

  std::printf("bench_copy_budget: runner=%s git=%s toolchain=%s build=%s\n",
              tag.c_str(), GitSha().c_str(), Toolchain().c_str(),
              THYME_BENCH_BUILD_TYPE);
  std::printf("%-36s %10s %14s %12s  %s\n", "metric", "iters", "ns/op", "MB/s",
              "check");
  for (const Metric &metric : metrics) {
    std::printf(
        "%-36s %10llu %14.3f %12.2f  %s\n", metric.name.c_str(),
        static_cast<unsigned long long>(metric.iterations), metric.ns_per_op,
        static_cast<double>(metric.bytes_per_op) * 1000.0 / metric.ns_per_op,
        metric.ok ? "OK" : "BAD");
  }
  if (!json_path.empty() && !EmitJson(json_path, tag, metrics, ok)) {
    std::fprintf(stderr, "bench_copy_budget: cannot write %s\n",
                 json_path.c_str());
    ok = false;
  }
  if (!ok) {
    std::fprintf(stderr,
                 "bench_copy_budget: a measured path failed correctness\n");
    return 1;
  }
  return 0;
}
