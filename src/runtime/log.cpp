#include "thyme/runtime/log.h"

#include "thyme/runtime/rotating_file.h"

#include <cerrno>
#include <cstdio>
#include <cstring>

#if !defined(_WIN32)
#include <pthread.h>
#include <time.h>
#include <unistd.h>
#if defined(__linux__)
#include <sys/syscall.h>
#endif
#endif

namespace thyme {
namespace log {
namespace {

constexpr char kHexDigits[] = "0123456789abcdef";

// Reserve at the end of a record for ` suppressed=N truncated=1\n`.
constexpr std::size_t kTailReserve = 64;
// Extra headroom so a message escape is never cut between two bytes of one
// \xHH token (AppendEscaped only commits whole escapes).
constexpr std::size_t kEscapeReserve = 8;

std::atomic<std::uint8_t> g_level{static_cast<std::uint8_t>(Level::kInfo)};
std::atomic<std::uint64_t> g_written{0};
std::atomic<std::uint64_t> g_suppressed{0};
std::atomic<std::uint64_t> g_dropped{0};
std::atomic<std::uint64_t> g_truncated{0};
std::atomic<bool> g_sink_open{false};

#if !defined(_WIN32)
pthread_mutex_t g_sink_mutex = PTHREAD_MUTEX_INITIALIZER;
#endif

#if !defined(_WIN32)
// Constructed on the first OpenFileSink/CloseFileSink call; a logger that has
// no file sink never touches it, so pre-main logging cannot race static
// initialization.
struct SinkState {
  logging::RotatingFile file;
};

SinkState& Sink() {
  static SinkState sink;
  return sink;
}

void LockSink() { (void)pthread_mutex_lock(&g_sink_mutex); }
void UnlockSink() { (void)pthread_mutex_unlock(&g_sink_mutex); }
#else
void LockSink() {}
void UnlockSink() {}
#endif

const char* BaseName(const char* path) {
  if (path == nullptr) return "<unknown>";
  const char* slash = std::strrchr(path, '/');
#if defined(_WIN32)
  const char* backslash = std::strrchr(path, '\\');
  if (backslash != nullptr && (slash == nullptr || backslash > slash)) {
    slash = backslash;
  }
#endif
  return slash != nullptr ? slash + 1 : path;
}

void FormatTimestamp(char* out, std::size_t cap) {
#if defined(_WIN32)
  if (cap > 0) out[0] = '\0';
#else
  struct timespec ts {};
  (void)clock_gettime(CLOCK_REALTIME, &ts);
  struct tm tmv {};
  if (gmtime_r(&ts.tv_sec, &tmv) == nullptr) {
    std::snprintf(out, cap, "1970-01-01T00:00:00.000Z");
    return;
  }
  const unsigned millis =
      static_cast<unsigned>((ts.tv_nsec / 1000000L) % 1000L);
  std::snprintf(out, cap, "%04d-%02d-%02dT%02d:%02d:%02d.%03uZ",
                tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday, tmv.tm_hour,
                tmv.tm_min, tmv.tm_sec, millis);
#endif
}

unsigned CurrentPid() {
#if defined(_WIN32)
  return 0;
#else
  return static_cast<unsigned>(getpid());
#endif
}

unsigned CurrentTid() {
#if defined(__linux__)
  return static_cast<unsigned>(syscall(SYS_gettid));
#else
  return 0;
#endif
}

// Append `src` to dst[*out_len], escaping \, ", \n, \r, \t and non-printables.
// dst must keep room for a terminating NUL at dst[cap - 1]; returns false
// without writing a partial escape when the buffer is full.
bool AppendEscaped(char* dst, std::size_t cap, std::size_t* out_len,
                   const char* src) {
  std::size_t o = *out_len;
  for (const unsigned char* p =
           reinterpret_cast<const unsigned char*>(src != nullptr ? src : "");
       *p != 0; ++p) {
    const unsigned char c = *p;
    const char* esc = nullptr;
    std::size_t esc_len = 0;
    char hex[5];
    if (c == '"') {
      esc = "\\\"";
      esc_len = 2;
    } else if (c == '\\') {
      esc = "\\\\";
      esc_len = 2;
    } else if (c == '\n') {
      esc = "\\n";
      esc_len = 2;
    } else if (c == '\r') {
      esc = "\\r";
      esc_len = 2;
    } else if (c == '\t') {
      esc = "\\t";
      esc_len = 2;
    } else if (c >= 0x20 && c != 0x7f) {
      // Printable ASCII and UTF-8 high bytes pass through unchanged; only
      // control bytes (and the quote/backslash cases above) are escaped.
      if (o + 2 > cap) return false;
      dst[o++] = static_cast<char>(c);
      continue;
    } else {
      hex[0] = '\\';
      hex[1] = 'x';
      hex[2] = kHexDigits[(c >> 4) & 0x0f];
      hex[3] = kHexDigits[c & 0x0f];
      hex[4] = '\0';
      esc = hex;
      esc_len = 4;
    }
    if (o + esc_len + 1 > cap) return false;
    std::memcpy(dst + o, esc, esc_len);
    o += esc_len;
  }
  *out_len = o;
  return true;
}

bool WriteAll(int fd, const char* data, std::size_t len) {
#if defined(_WIN32)
  (void)fd;
  return std::fwrite(data, 1, len, stderr) == len;
#else
  ssize_t wrote = 0;
  do {
    wrote = ::write(fd, data, len);
  } while (wrote < 0 && errno == EINTR);
  return wrote == static_cast<ssize_t>(len);
#endif
}

bool WriteRecord(const char* line, std::size_t len) {
#if defined(_WIN32)
  return WriteAll(-1, line, len);
#else
  bool ok = false;
  if (g_sink_open.load(std::memory_order_acquire)) {
    LockSink();
    // Re-check under the lock: CloseFileSink flips the flag under the same
    // lock, so the RotatingFile cannot be torn down mid-Append.
    if (g_sink_open.load(std::memory_order_relaxed)) {
      const std::ptrdiff_t wrote = Sink().file.Append(line, len);
      ok = wrote == static_cast<std::ptrdiff_t>(len);
    } else {
      ok = WriteAll(STDERR_FILENO, line, len);
    }
    UnlockSink();
  } else {
    ok = WriteAll(STDERR_FILENO, line, len);
  }
  return ok;
#endif
}

bool EmitImpl(const Site& site, Level level, RateSite* rate,
              const RatePolicy* policy, const char* msg, bool msg_truncated,
              const Fields* fields) {
  if (!Enabled(level)) return false;

  std::uint64_t suppressed = 0;
  const bool rated = policy != nullptr && rate != nullptr;
  if (rated && !ShouldEmit(rate, *policy, &suppressed)) {
    g_suppressed.fetch_add(1, std::memory_order_relaxed);
    return false;
  }

  char ts[64];
  FormatTimestamp(ts, sizeof(ts));

  char line[kRecordMax];
  const int prefix = std::snprintf(
      line, sizeof(line), "ts=%s level=%s pid=%u tid=%u site=%s:%u msg=\"", ts,
      LevelName(level), CurrentPid(), CurrentTid(), BaseName(site.file),
      static_cast<unsigned>(site.line));
  if (prefix < 0 || static_cast<std::size_t>(prefix) >= sizeof(line)) {
    g_dropped.fetch_add(1, std::memory_order_relaxed);
    return false;
  }
  std::size_t len = static_cast<std::size_t>(prefix);
  const std::size_t msg_cap =
      sizeof(line) - kTailReserve - kEscapeReserve;
  bool truncated = msg_truncated;
  if (!AppendEscaped(line, msg_cap, &len, msg)) truncated = true;
  if (len + 1 < sizeof(line)) {
    line[len++] = '"';
  } else {
    truncated = true;
    len = sizeof(line) - 1;
  }
  line[len] = '\0';

  if (fields != nullptr) {
    const std::size_t n = fields->size();
    if (n != 0 && len + n + 1 < sizeof(line) - kTailReserve) {
      std::memcpy(line + len, fields->data(), n);
      len += n;
      line[len] = '\0';
    } else if (n != 0) {
      truncated = true;
    }
    if (fields->truncated()) truncated = true;
  }

  if (rated) {
    const int w = std::snprintf(line + len, sizeof(line) - len,
                                " suppressed=%llu",
                                static_cast<unsigned long long>(suppressed));
    if (w > 0 && static_cast<std::size_t>(w) < sizeof(line) - len) {
      len += static_cast<std::size_t>(w);
    } else {
      truncated = true;
    }
  }
  if (truncated) {
    const int w = std::snprintf(line + len, sizeof(line) - len, " truncated=1");
    if (w > 0 && static_cast<std::size_t>(w) < sizeof(line) - len) {
      len += static_cast<std::size_t>(w);
    }
    g_truncated.fetch_add(1, std::memory_order_relaxed);
  }
  if (len + 1 >= sizeof(line)) {
    g_dropped.fetch_add(1, std::memory_order_relaxed);
    return false;
  }
  line[len++] = '\n';

  const bool ok = WriteRecord(line, len);
  if (ok) {
    g_written.fetch_add(1, std::memory_order_relaxed);
  } else {
    g_dropped.fetch_add(1, std::memory_order_relaxed);
  }
  return ok;
}

}  // namespace

const char* LevelName(Level level) {
  switch (level) {
    case Level::kError: return "error";
    case Level::kWarn: return "warn";
    case Level::kInfo: return "info";
    case Level::kDebug: return "debug";
  }
  return "info";
}

bool Enabled(Level level) {
  return static_cast<std::uint8_t>(level) <=
         g_level.load(std::memory_order_relaxed);
}

void SetLevel(Level level) {
  std::uint8_t value = static_cast<std::uint8_t>(level);
  if (value > static_cast<std::uint8_t>(Level::kDebug)) value = 3;
  g_level.store(value, std::memory_order_relaxed);
}

Level GetLevel() { return static_cast<Level>(g_level.load(std::memory_order_relaxed)); }

bool ShouldEmit(RateSite* site, const RatePolicy& policy,
                std::uint64_t* suppressed_out) {
  if (suppressed_out != nullptr) *suppressed_out = 0;
  if (site == nullptr) return true;

  const std::uint32_t n =
      site->seen.fetch_add(1, std::memory_order_relaxed) + 1;
  if (n <= policy.burst) {
    site->last_emitted.store(n, std::memory_order_relaxed);
    return true;
  }
  if (policy.every_nth == 0 || (n - policy.burst) % policy.every_nth != 0) {
    site->suppressed.fetch_add(1, std::memory_order_relaxed);
    return false;
  }
  const std::uint32_t previous =
      site->last_emitted.exchange(n, std::memory_order_relaxed);
  if (suppressed_out != nullptr) {
    // Under concurrent calls a later nth slot may have been recorded first;
    // report "unknown/low" rather than underflowing the count.
    *suppressed_out =
        n > previous ? static_cast<std::uint64_t>(n - previous - 1) : 0;
  }
  return true;
}

void ResetRateSiteForTest(RateSite* site) {
  if (site == nullptr) return;
  site->seen.store(0, std::memory_order_relaxed);
  site->last_emitted.store(0, std::memory_order_relaxed);
  site->suppressed.store(0, std::memory_order_relaxed);
}

void Fields::Begin(const char* key) {
  if (truncated_) return;
  if (key == nullptr) key = "?";
  const std::size_t need = std::strlen(key) + 2;  // ' ' key '='
  if (len_ + need + 1 > sizeof(buf_)) {
    truncated_ = true;
    return;
  }
  buf_[len_++] = ' ';
  const std::size_t n = std::strlen(key);
  std::memcpy(buf_ + len_, key, n);
  len_ += n;
  buf_[len_++] = '=';
  buf_[len_] = '\0';
}

void Fields::AppendRaw(const char* text) {
  if (truncated_ || text == nullptr) return;
  const std::size_t n = std::strlen(text);
  if (len_ + n + 1 > sizeof(buf_)) {
    truncated_ = true;
    return;
  }
  std::memcpy(buf_ + len_, text, n);
  len_ += n;
  buf_[len_] = '\0';
}

void Fields::AppendQuoted(const char* value) {
  if (truncated_) return;
  if (len_ + 2 > sizeof(buf_)) {
    truncated_ = true;
    return;
  }
  buf_[len_++] = '"';
  const std::size_t saved = len_;
  if (!AppendEscaped(buf_, sizeof(buf_), &len_, value)) {
    len_ = saved;
    truncated_ = true;
    return;
  }
  if (len_ + 2 > sizeof(buf_)) {
    len_ = saved;
    truncated_ = true;
    return;
  }
  buf_[len_++] = '"';
  buf_[len_] = '\0';
}

Fields& Fields::Str(const char* key, const char* value) {
  if (truncated_) return *this;
  const std::size_t saved = len_;
  Begin(key);
  if (!truncated_) AppendQuoted(value != nullptr ? value : "");
  if (truncated_) {
    len_ = saved;
    buf_[len_] = '\0';
  }
  return *this;
}

Fields& Fields::U64(const char* key, std::uint64_t value) {
  if (truncated_) return *this;
  const std::size_t saved = len_;
  Begin(key);
  char num[24];
  std::snprintf(num, sizeof(num), "%llu",
                static_cast<unsigned long long>(value));
  if (!truncated_) AppendRaw(num);
  if (truncated_) {
    len_ = saved;
    buf_[len_] = '\0';
  }
  return *this;
}

Fields& Fields::I64(const char* key, std::int64_t value) {
  if (truncated_) return *this;
  const std::size_t saved = len_;
  Begin(key);
  char num[24];
  std::snprintf(num, sizeof(num), "%lld",
                static_cast<long long>(value));
  if (!truncated_) AppendRaw(num);
  if (truncated_) {
    len_ = saved;
    buf_[len_] = '\0';
  }
  return *this;
}

Fields& Fields::Bool(const char* key, bool value) {
  if (truncated_) return *this;
  const std::size_t saved = len_;
  Begin(key);
  if (!truncated_) AppendRaw(value ? "1" : "0");
  if (truncated_) {
    len_ = saved;
    buf_[len_] = '\0';
  }
  return *this;
}

Fields& Fields::Hex(const char* key, std::uint64_t value) {
  if (truncated_) return *this;
  const std::size_t saved = len_;
  Begin(key);
  char num[24];
  std::snprintf(num, sizeof(num), "0x%llx",
                static_cast<unsigned long long>(value));
  if (!truncated_) AppendRaw(num);
  if (truncated_) {
    len_ = saved;
    buf_[len_] = '\0';
  }
  return *this;
}

namespace {

bool FormatMessage(char* out, std::size_t cap, const char* fmt, va_list ap) {
  if (fmt == nullptr) {
    if (cap > 0) out[0] = '\0';
    return false;
  }
  const int n = std::vsnprintf(out, cap, fmt, ap);
  if (n < 0) {
    if (cap > 0) out[0] = '\0';
    return true;
  }
  if (static_cast<std::size_t>(n) >= cap) {
    // vsnprintf NUL-terminates the truncated text. Mark the cut explicitly;
    // the record also carries truncated=1.
    const std::size_t len = std::strlen(out);
    if (len >= 3) {
      out[len - 1] = '.';
      out[len - 2] = '.';
      out[len - 3] = '.';
    }
    return true;
  }
  return false;
}

}  // namespace

bool Emit(const Site& site, Level level, const char* fmt, ...) {
  if (!Enabled(level)) return false;
  char msg[kMessageMax];
  va_list ap;
  va_start(ap, fmt);
  const bool truncated = FormatMessage(msg, sizeof(msg), fmt, ap);
  va_end(ap);
  return EmitImpl(site, level, nullptr, nullptr, msg, truncated, nullptr);
}

bool EmitRated(const Site& site, RateSite* rate, Level level,
               const RatePolicy& policy, const char* fmt, ...) {
  if (!Enabled(level)) return false;
  char msg[kMessageMax];
  va_list ap;
  va_start(ap, fmt);
  const bool truncated = FormatMessage(msg, sizeof(msg), fmt, ap);
  va_end(ap);
  return EmitImpl(site, level, rate, &policy, msg, truncated, nullptr);
}

bool EmitFields(const Site& site, Level level, const char* msg,
                const Fields& fields) {
  return EmitImpl(site, level, nullptr, nullptr, msg != nullptr ? msg : "",
                  false, &fields);
}

bool EmitFieldsRated(const Site& site, RateSite* rate, Level level,
                     const RatePolicy& policy, const char* msg,
                     const Fields& fields) {
  return EmitImpl(site, level, rate, &policy, msg != nullptr ? msg : "", false,
                  &fields);
}

bool OpenFileSink(const FileSinkConfig& cfg) {
#if defined(_WIN32)
  (void)cfg;
  return false;
#else
  if (cfg.path == nullptr || cfg.path[0] == '\0') return false;
  LockSink();
  logging::RotateConfig rcfg;
  rcfg.path = cfg.path;
  rcfg.max_bytes = cfg.max_bytes;
  rcfg.keep = cfg.keep;
  rcfg.mandatory = false;
  rcfg.account = cfg.account;
  const bool ok = Sink().file.Open(rcfg);
  g_sink_open.store(ok, std::memory_order_release);
  UnlockSink();
  return ok;
#endif
}

void CloseFileSink() {
#if !defined(_WIN32)
  LockSink();
  g_sink_open.store(false, std::memory_order_release);
  Sink().file.Close();
  UnlockSink();
#endif
}

bool FileSinkOpen() { return g_sink_open.load(std::memory_order_acquire); }

std::uint64_t records_written() {
  return g_written.load(std::memory_order_relaxed);
}
std::uint64_t records_suppressed() {
  return g_suppressed.load(std::memory_order_relaxed);
}
std::uint64_t records_dropped() {
  return g_dropped.load(std::memory_order_relaxed);
}
std::uint64_t records_truncated() {
  return g_truncated.load(std::memory_order_relaxed);
}

std::uint64_t rotations() {
#if defined(_WIN32)
  return 0;
#else
  std::uint64_t n = 0;
  if (g_sink_open.load(std::memory_order_acquire)) {
    LockSink();
    if (g_sink_open.load(std::memory_order_relaxed)) {
      n = Sink().file.rotations();
    }
    UnlockSink();
  }
  return n;
#endif
}

void ResetStatsForTest() {
  g_written.store(0, std::memory_order_relaxed);
  g_suppressed.store(0, std::memory_order_relaxed);
  g_dropped.store(0, std::memory_order_relaxed);
  g_truncated.store(0, std::memory_order_relaxed);
}

}  // namespace log
}  // namespace thyme
