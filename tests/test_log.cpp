// test_log.cpp — OBS-08: leveled logger — filtering, structured fields,
// escaping, rate limiting, bounded-record truncation, OPS-05 rotation,
// concurrent writes, and zero allocation on the emit path.
//
// The allocation check uses the same process-global operator new replacement
// as TST-13 (test_core_no_alloc): the counter is armed only around the emit
// loop, after the sink/level setup, and asserts exactly zero allocations.

#include "thyme/check.h"
#include "thyme/runtime/log.h"
#include "thyme/runtime/rotating_file.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <string>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

using namespace thyme;

namespace {

// ---------------------------------------------------------------------------
// Allocation sentinel (TST-13 style): count operator new calls only while the
// window is armed.
// ---------------------------------------------------------------------------
std::atomic<bool> g_armed{false};
std::atomic<std::uint64_t> g_allocs{0};

void* Allocate(std::size_t size) {
  return std::malloc(size == 0 ? 1 : size);
}

}  // namespace

void* operator new(std::size_t size) {
  if (g_armed.load(std::memory_order_acquire)) {
    g_allocs.fetch_add(1, std::memory_order_relaxed);
  }
  void* const p = Allocate(size);
  if (p == nullptr) throw std::bad_alloc();
  return p;
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void* operator new(std::size_t size, const std::nothrow_t&) noexcept {
  if (g_armed.load(std::memory_order_acquire)) {
    g_allocs.fetch_add(1, std::memory_order_relaxed);
  }
  return Allocate(size);
}
void* operator new[](std::size_t size, const std::nothrow_t&) noexcept {
  return ::operator new(size, std::nothrow);
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

namespace {

std::string TempPath(const char* name) {
  char buf[128];
  std::snprintf(buf, sizeof(buf), "/tmp/thyme_log_test_%d_%s",
                static_cast<int>(getpid()), name);
  return std::string(buf);
}

int CaptureBegin(const char* path) {
  const int saved = dup(STDERR_FILENO);
  CHECK(saved >= 0);
  const int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
  CHECK(fd >= 0);
  CHECK(dup2(fd, STDERR_FILENO) == STDERR_FILENO);
  close(fd);
  return saved;
}

void CaptureEnd(int saved) {
  std::fflush(stderr);
  CHECK(dup2(saved, STDERR_FILENO) == STDERR_FILENO);
  close(saved);
}

std::string Slurp(const char* path) {
  std::FILE* f = std::fopen(path, "rb");
  CHECK(f != nullptr);
  std::string body;
  char buf[4096];
  std::size_t n = 0;
  while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) body.append(buf, n);
  std::fclose(f);
  return body;
}

bool Contains(const std::string& haystack, const char* needle) {
  return haystack.find(needle) != std::string::npos;
}

int CountLines(const std::string& body) {
  int n = 0;
  for (char c : body) {
    if (c == '\n') ++n;
  }
  return n;
}

void TestLevelFiltering() {
  log::SetLevel(log::Level::kInfo);
  CHECK(log::GetLevel() == log::Level::kInfo);
  CHECK(log::CompileEnabled(log::Level::kDebug));
  CHECK(log::CompileEnabled(log::Level::kError));

  const std::string path = TempPath("levels.log");
  const int saved = CaptureBegin(path.c_str());
  log::Emit({__FILE__, static_cast<std::uint32_t>(__LINE__)},
            log::Level::kDebug, "debug-hidden");
  log::Emit({__FILE__, static_cast<std::uint32_t>(__LINE__)},
            log::Level::kInfo, "info-visible");
  log::Emit({__FILE__, static_cast<std::uint32_t>(__LINE__)},
            log::Level::kError, "error-visible");
  CaptureEnd(saved);

  const std::string body = Slurp(path.c_str());
  CHECK(!Contains(body, "debug-hidden"));
  CHECK(Contains(body, "level=info"));
  CHECK(Contains(body, "info-visible"));
  CHECK(Contains(body, "level=error"));
  CHECK(Contains(body, "error-visible"));
  CHECK(CountLines(body) == 2);

  // Runtime filter: only errors pass.
  log::SetLevel(log::Level::kError);
  CHECK(!log::Enabled(log::Level::kWarn));
  CHECK(log::Enabled(log::Level::kError));
  log::SetLevel(log::Level::kDebug);
  CHECK(log::Enabled(log::Level::kDebug));
  log::SetLevel(log::Level::kInfo);
  std::remove(path.c_str());
}

void TestFormatAndFields() {
  const std::string path = TempPath("fields.log");
  const int saved = CaptureBegin(path.c_str());
  log::Fields fields;
  fields.U64("trigger_id", 7)
      .Str("dir", "/tmp/a b")
      .Bool("sealed", true)
      .I64("delta_ns", -42)
      .Hex("mask", 0x2au);
  CHECK(!fields.truncated());
  log::EmitFields({__FILE__, static_cast<std::uint32_t>(__LINE__)},
                  log::Level::kWarn, "capture sealed", fields);
  CaptureEnd(saved);

  const std::string body = Slurp(path.c_str());
  CHECK(Contains(body, "level=warn"));
  CHECK(Contains(body, "site=test_log.cpp:"));
  CHECK(Contains(body, "msg=\"capture sealed\""));
  CHECK(Contains(body, "trigger_id=7"));
  CHECK(Contains(body, "dir=\"/tmp/a b\""));
  CHECK(Contains(body, "sealed=1"));
  CHECK(Contains(body, "delta_ns=-42"));
  CHECK(Contains(body, "mask=0x2a"));
  CHECK(Contains(body, "ts=20"));
  CHECK(Contains(body, "Z level=warn"));
  CHECK(CountLines(body) == 1);
  std::remove(path.c_str());
}

void TestEscaping() {
  const std::string path = TempPath("escape.log");
  const int saved = CaptureBegin(path.c_str());
  log::Emit({__FILE__, static_cast<std::uint32_t>(__LINE__)},
            log::Level::kInfo, "line1\nline2\t\"q\"\\end");
  CaptureEnd(saved);

  const std::string body = Slurp(path.c_str());
  CHECK(CountLines(body) == 1);  // embedded newline is escaped, not raw
  CHECK(Contains(body, "line1\\nline2\\t\\\"q\\\"\\\\end"));
  std::remove(path.c_str());
}

void TestRateLimiting() {
  log::RateSite site;
  const log::RatePolicy policy{3, 1000};
  int emitted = 0;
  std::vector<std::uint64_t> suppressed_at_emit;
  for (std::uint32_t i = 0; i < 2500; ++i) {
    std::uint64_t suppressed = 0;
    if (log::ShouldEmit(&site, policy, &suppressed)) {
      ++emitted;
      suppressed_at_emit.push_back(suppressed);
    }
  }
  CHECK(site.seen.load() == 2500);
  CHECK(emitted == 5);  // 1,2,3 then 1003 and 2003
  CHECK(site.suppressed.load() == 2495);
  CHECK(suppressed_at_emit.size() == 5);
  CHECK(suppressed_at_emit[0] == 0);
  CHECK(suppressed_at_emit[1] == 0);
  CHECK(suppressed_at_emit[2] == 0);
  CHECK(suppressed_at_emit[3] == 999);
  CHECK(suppressed_at_emit[4] == 999);

  // The macro keeps one site per expansion: 1003 calls -> 4 records, the
  // last one carrying the exact suppression count.
  const std::string path = TempPath("rate.log");
  log::ResetStatsForTest();
  const int saved = CaptureBegin(path.c_str());
  for (std::uint32_t i = 0; i < 1003; ++i) {
    THYME_LOG_INFO_RL("rate limited record i=%u", static_cast<unsigned>(i));
  }
  CaptureEnd(saved);
  const std::string body = Slurp(path.c_str());
  CHECK(CountLines(body) == 4);
  CHECK(Contains(body, "suppressed=0"));
  CHECK(Contains(body, "suppressed=999"));
  CHECK(log::records_suppressed() == 999);
  CHECK(log::records_written() == 4);
  std::remove(path.c_str());

  // Reset helper restores a site to its pristine state.
  log::ResetRateSiteForTest(&site);
  CHECK(site.seen.load() == 0);
  CHECK(site.suppressed.load() == 0);
}

void TestBoundedMessage() {
  const std::string exact(log::kMessageMax - 1, 'a');
  const std::string path = TempPath("exact.log");
  int saved = CaptureBegin(path.c_str());
  log::Emit({__FILE__, static_cast<std::uint32_t>(__LINE__)},
            log::Level::kInfo, "%s", exact.c_str());
  CaptureEnd(saved);
  std::string body = Slurp(path.c_str());
  CHECK(Contains(body, exact.c_str()));
  CHECK(!Contains(body, "truncated=1"));
  std::remove(path.c_str());

  const std::string oversized(2000, 'b');
  const std::string path2 = TempPath("trunc.log");
  saved = CaptureBegin(path2.c_str());
  log::Emit({__FILE__, static_cast<std::uint32_t>(__LINE__)},
            log::Level::kInfo, "%s", oversized.c_str());
  CaptureEnd(saved);
  body = Slurp(path2.c_str());
  CHECK(Contains(body, "...\""));
  CHECK(Contains(body, "truncated=1"));
  CHECK(CountLines(body) == 1);
  CHECK(log::records_truncated() >= 1);
  std::remove(path2.c_str());

  // Field overflow drops whole fields and flags the record instead of
  // emitting a half-written key/value token.
  log::Fields fields;
  for (std::uint32_t i = 0; i < 200; ++i) fields.U64("key", i);
  CHECK(fields.truncated());
  log::ResetStatsForTest();
  const std::string path3 = TempPath("ftrunc.log");
  saved = CaptureBegin(path3.c_str());
  log::EmitFields({__FILE__, static_cast<std::uint32_t>(__LINE__)},
                  log::Level::kInfo, "field overflow", fields);
  CaptureEnd(saved);
  body = Slurp(path3.c_str());
  CHECK(Contains(body, "truncated=1"));
  CHECK(log::records_truncated() == 1);
  std::remove(path3.c_str());
}

void TestFileSinkRotation() {
  char dir[] = "/tmp/thyme_log_rot_XXXXXX";
  CHECK(mkdtemp(dir) != nullptr);
  const std::string path = std::string(dir) + "/service.log";
  logging::LogBudget::ResetForTest();
  log::ResetStatsForTest();

  log::FileSinkConfig cfg;
  cfg.path = path.c_str();
  cfg.max_bytes = 512;
  cfg.keep = 2;
  CHECK(log::OpenFileSink(cfg));
  CHECK(log::FileSinkOpen());

  const std::string stderr_path = TempPath("sink_stderr.log");
  const int saved = CaptureBegin(stderr_path.c_str());
  log::Site site{__FILE__, static_cast<std::uint32_t>(__LINE__)};
  for (int i = 0; i < 60; ++i) {
    log::Emit(site, log::Level::kInfo,
              "rotation record i=%d payload=abcdefghijklmnopqrstuvwxyz", i);
  }
  CaptureEnd(saved);

  CHECK(log::records_written() == 60);
  CHECK(log::rotations() >= 2);
  CHECK(Slurp(stderr_path.c_str()).empty());  // sink replaces stderr
  struct stat st {};
  CHECK(stat(path.c_str(), &st) == 0);
  CHECK(stat((path + ".1").c_str(), &st) == 0);
  CHECK(stat((path + ".2").c_str(), &st) == 0);
  CHECK(stat((path + ".3").c_str(), &st) != 0);
  CHECK(st.st_size <= 512 + 256);  // cap + at most one record

  log::CloseFileSink();
  CHECK(!log::FileSinkOpen());
  logging::LogBudget::ResetForTest();
  std::remove(stderr_path.c_str());
  for (const char* suffix : {"", ".1", ".2"}) {
    std::remove((path + suffix).c_str());
  }
  rmdir(dir);
}

void TestConcurrentWrites() {
  const std::string path = TempPath("threads.log");
  const int saved = CaptureBegin(path.c_str());
  const std::size_t per_thread = 200;
  std::thread a([per_thread] {
    const log::Site site{__FILE__, static_cast<std::uint32_t>(__LINE__)};
    for (std::size_t i = 0; i < per_thread; ++i) {
      log::Emit(site, log::Level::kInfo, "thread=a i=%zu", i);
    }
  });
  std::thread b([per_thread] {
    const log::Site site{__FILE__, static_cast<std::uint32_t>(__LINE__)};
    for (std::size_t i = 0; i < per_thread; ++i) {
      log::Emit(site, log::Level::kInfo, "thread=b i=%zu", i);
    }
  });
  a.join();
  b.join();
  CaptureEnd(saved);
  const std::string body = Slurp(path.c_str());
  CHECK(CountLines(body) == static_cast<int>(per_thread * 2));
  // Every line is complete: it starts with ts= and ends with "\n".
  std::size_t pos = 0;
  int lines = 0;
  while (pos < body.size()) {
    const std::size_t nl = body.find('\n', pos);
    CHECK(nl != std::string::npos);
    CHECK(body.compare(pos, 3, "ts=") == 0);
    ++lines;
    pos = nl + 1;
  }
  CHECK(lines == static_cast<int>(per_thread * 2));
  std::remove(path.c_str());
}

void TestZeroAllocEmit() {
  char dir[] = "/tmp/thyme_log_alloc_XXXXXX";
  CHECK(mkdtemp(dir) != nullptr);
  const std::string path = std::string(dir) + "/alloc.log";
  logging::LogBudget::ResetForTest();
  log::ResetStatsForTest();
  log::FileSinkConfig cfg;
  cfg.path = path.c_str();
  cfg.max_bytes = 64 * 1024 * 1024;
  cfg.keep = 1;
  CHECK(log::OpenFileSink(cfg));
  log::SetLevel(log::Level::kInfo);

  const log::Site site{__FILE__, static_cast<std::uint32_t>(__LINE__)};
  // Warm any one-time libc/mutex state outside the measured window.
  CHECK(log::Emit(site, log::Level::kInfo, "warmup trigger_id=%llu",
                  static_cast<unsigned long long>(0)));

  g_allocs.store(0, std::memory_order_relaxed);
  g_armed.store(true, std::memory_order_release);
  for (std::uint32_t i = 0; i < 200; ++i) {
    log::Fields fields;
    fields.U64("trigger_id", i).Str("state", "armed").Bool("ok", true);
    CHECK(log::EmitFields(site, log::Level::kWarn, "zero alloc", fields));
  }
  g_armed.store(false, std::memory_order_release);
  const std::uint64_t allocs = g_allocs.load(std::memory_order_relaxed);
  CHECK(allocs == 0);

  log::CloseFileSink();
  logging::LogBudget::ResetForTest();
  std::remove(path.c_str());
  rmdir(dir);
}

}  // namespace

int main() {
  TestLevelFiltering();
  TestFormatAndFields();
  TestEscaping();
  TestRateLimiting();
  TestBoundedMessage();
  TestFileSinkRotation();
  TestConcurrentWrites();
  TestZeroAllocEmit();
  log::SetLevel(log::Level::kInfo);
  std::fflush(nullptr);
  return 0;
}
