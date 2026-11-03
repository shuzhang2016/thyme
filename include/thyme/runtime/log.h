#pragma once

// log.h — OBS-08: structured, leveled, rate-limited service log for the
// thyme core libraries.
//
// Design constraints (docs/reference/coding-rules.md CR-01, TST-13):
//   * No exceptions, no RTTI, no dynamic allocation on the emit path. A
//     record is formatted into fixed stack buffers (message 384 B, fields
//     256 B, full line 832 B) with snprintf/vsnprintf; nothing allocates.
//   * One write() per record: a configured file sink is a RotatingFile
//     (OPS-05: O_APPEND, size cap, rename rotation, LogBudget) and is
//     serialized by one process-wide mutex. With no file sink the record
//     goes to stderr with a single write(2). A configured sink that cannot
//     write counts a drop and does not tee to stderr (which may be the same
//     journal, and double records would corrupt the rate accounting).
//   * Levels: error < warn < info < debug. Runtime filter via SetLevel()
//     (default info); compile-time filter via THYME_LOG_MAX_LEVEL (default
//     debug) so a release build can strip debug call sites entirely.
//   * Rate limiting per call site: the *_RL macros keep one static RateSite
//     per expansion and emit the first `burst` records, then every `every_nth`
//     record, carrying the number of suppressed records in the record. This is
//     the only logging allowed on a per-message / per-callback path — a hot
//     path must never log unconditionally (docs/reference/coding-rules.md CR-03).
//   * Structured output: one line of `key=value` pairs, matching the SEC-02
//     audit writer's style so `grep`/`thyme_log_merge` work uniformly:
//
//       ts=2026-10-24T09:12:33.123Z level=warn pid=4242 tid=4243
//       site=record_mirror.h:274 msg="oversized payload dropped"
//       struct_id=0 size=2048 capacity=512 suppressed=0
//
//     Values inside msg/Str fields are quoted with \\ \" \n \r \t escapes;
//     other control bytes become \xHH and UTF-8 high bytes pass through, so
//     a record is always exactly one line. A record that cannot fit its
//     bounded buffers is emitted with `truncated=1` rather than silently cut
//     mid-token.
//
// NOT async-signal-safe: Emit takes a mutex, calls vsnprintf/clock_gettime
// and, for a file sink, may rename() generations. Never call it from a signal
// handler. Crash-window logging lives in PerfLogDumper, which appends a
// periodic dump from a normal thread instead (docs/guides/runbook.md).
//
// NOT safe in a forked child of a multithreaded process: the sink mutex may
// be held by a thread that no longer exists. The supervisor's post-fork
// exec-failure path therefore keeps raw write()/fprintf (see
// src/app/mp_supervisor.cpp).
//
// Sink configuration is startup-only: call OpenFileSink() before Start().
// Emit itself never allocates, including the first record.

#include <atomic>
#include <cstdarg>
#include <cstddef>
#include <cstdint>

namespace thyme {
namespace log {

enum class Level : uint8_t {
  kError = 0,
  kWarn = 1,
  kInfo = 2,
  kDebug = 3,
};

// Compile-time floor: call sites above this value compile to nothing. Keep
// the numeric order in sync with Level. Override per target with
// -DTHYME_LOG_MAX_LEVEL=2 (info) etc.
#ifndef THYME_LOG_MAX_LEVEL
#define THYME_LOG_MAX_LEVEL 3
#endif

inline constexpr std::size_t kMessageMax = 384;  // formatted message text
inline constexpr std::size_t kFieldsMax = 256;   // structured field suffix
inline constexpr std::size_t kRecordMax = 832;   // one complete line

struct Site {
  const char* file;
  std::uint32_t line;
};

const char* LevelName(Level level);

constexpr bool CompileEnabled(Level level) {
  return static_cast<std::uint8_t>(level) <=
         static_cast<std::uint8_t>(THYME_LOG_MAX_LEVEL);
}

// Runtime filter (default kInfo). SetLevel(kDebug) enables debug records.
bool Enabled(Level level);
void SetLevel(Level level);
Level GetLevel();

// first `burst` records per site are always emitted, then every `every_nth`.
// every_nth == 0 means "suppress everything after the burst".
struct RatePolicy {
  std::uint32_t burst;
  std::uint32_t every_nth;
};

// The hot-path default: 3, then every 1000th (max ~1000x reduction).
inline constexpr RatePolicy kDefaultRate{3, 1000};
// For a site that must stay one-shot (its former static-atomic behaviour).
inline constexpr RatePolicy kOnceThenEvery1000{1, 1000};

// Per call-site rate state. A function-local static of this type is
// constant-initialized, so the first call does not allocate or take a guard
// lock beyond the compiler's static-init check.
struct RateSite {
  std::atomic<std::uint32_t> seen{0};
  std::atomic<std::uint32_t> last_emitted{0};
  std::atomic<std::uint64_t> suppressed{0};
};

// Returns true when this call should be emitted. Single-threaded callers see
// exact counts; under concurrent calls the decision is atomic but a racing
// pair may both fall on the same nth slot (the log is advisory, not an audit
// trail). `suppressed_out` receives the number of records suppressed since the
// previous emitted record (0 on the first burst).
bool ShouldEmit(RateSite* site, const RatePolicy& policy,
                std::uint64_t* suppressed_out);

void ResetRateSiteForTest(RateSite* site);

// Fixed-capacity key=value builder. Methods never allocate; once the buffer
// is full the field is dropped as a whole and truncated() becomes true.
class Fields {
 public:
  Fields() = default;

  Fields& Str(const char* key, const char* value);
  Fields& U64(const char* key, std::uint64_t value);
  Fields& I64(const char* key, std::int64_t value);
  Fields& Bool(const char* key, bool value);
  Fields& Hex(const char* key, std::uint64_t value);

  const char* data() const { return buf_; }
  std::size_t size() const { return len_; }
  bool truncated() const { return truncated_; }

 private:
  void Begin(const char* key);
  void End(std::size_t key_start);
  void AppendRaw(const char* text);
  void AppendQuoted(const char* value);

  char buf_[kFieldsMax] = {};
  std::size_t len_ = 0;
  bool truncated_ = false;
};

// Emit a formatted record. Returns false when the level is filtered, the rate
// limiter suppresses this call, the sink is unavailable, or the write failed;
// check the counters below for diagnostics. `fmt` uses printf syntax; values
// are quoted into msg="...".
bool Emit(const Site& site, Level level, const char* fmt, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 3, 4)))
#endif
    ;

bool EmitRated(const Site& site, RateSite* rate, Level level,
               const RatePolicy& policy, const char* fmt, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 5, 6)))
#endif
    ;

// Structured variants: `msg` is literal text (no printf formatting); `fields`
// is appended as separate key=value tokens outside the msg quotes.
bool EmitFields(const Site& site, Level level, const char* msg,
                const Fields& fields);
bool EmitFieldsRated(const Site& site, RateSite* rate, Level level,
                     const RatePolicy& policy, const char* msg,
                     const Fields& fields);

// File sink (startup only). `path` must outlive the call; the implementation
// copies it into the RotatingFile. Returns false when the file cannot be
// opened; the logger then keeps using stderr. A sink configured with
// max_bytes == 0 or keep == 0 disables rotation (unbounded, not recommended).
struct FileSinkConfig {
  const char* path = nullptr;
  std::uint64_t max_bytes = 8ull * 1024 * 1024;
  std::uint32_t keep = 2;
  bool account = true;
};

bool OpenFileSink(const FileSinkConfig& cfg);
void CloseFileSink();
bool FileSinkOpen();

// Process-wide counters since start (atomic; safe from any thread).
std::uint64_t records_written();
std::uint64_t records_suppressed();
std::uint64_t records_dropped();   // sink unavailable / write error
std::uint64_t records_truncated(); // carried truncated=1
std::uint64_t rotations();         // file-sink rotations (OPS-05)
void ResetStatsForTest();

}  // namespace log
}  // namespace thyme

// ---------------------------------------------------------------------------
// Macros. Every expansion owns its own static RateSite for the *_RL forms.
// ---------------------------------------------------------------------------

#define THYME_LOG_SITE_() \
  ::thyme::log::Site { __FILE__, static_cast<std::uint32_t>(__LINE__) }

#define THYME_LOG_PLAIN_(level_, ...)                                      \
  do {                                                                     \
    if constexpr (::thyme::log::CompileEnabled(                            \
                      ::thyme::log::Level::level_)) {                      \
      if (::thyme::log::Enabled(::thyme::log::Level::level_)) {            \
        (void)::thyme::log::Emit(THYME_LOG_SITE_(),                        \
                                 ::thyme::log::Level::level_, __VA_ARGS__);\
      }                                                                    \
    }                                                                      \
  } while (0)

#define THYME_LOG_RATED_(level_, policy_, ...)                             \
  do {                                                                     \
    if constexpr (::thyme::log::CompileEnabled(                            \
                      ::thyme::log::Level::level_)) {                      \
      if (::thyme::log::Enabled(::thyme::log::Level::level_)) {            \
        static ::thyme::log::RateSite _thyme_log_rate_site;                \
        (void)::thyme::log::EmitRated(THYME_LOG_SITE_(),                   \
                                      &_thyme_log_rate_site,               \
                                      ::thyme::log::Level::level_, policy_,\
                                      __VA_ARGS__);                        \
      }                                                                    \
    }                                                                      \
  } while (0)

#define THYME_LOG_FIELDS_(level_, msg_, fields_)                           \
  do {                                                                     \
    if constexpr (::thyme::log::CompileEnabled(                            \
                      ::thyme::log::Level::level_)) {                      \
      if (::thyme::log::Enabled(::thyme::log::Level::level_)) {            \
        (void)::thyme::log::EmitFields(THYME_LOG_SITE_(),                  \
                                       ::thyme::log::Level::level_, msg_,  \
                                       fields_);                           \
      }                                                                    \
    }                                                                      \
  } while (0)

#define THYME_LOG_RATED_FIELDS_(level_, policy_, msg_, fields_)            \
  do {                                                                     \
    if constexpr (::thyme::log::CompileEnabled(                            \
                      ::thyme::log::Level::level_)) {                      \
      if (::thyme::log::Enabled(::thyme::log::Level::level_)) {            \
        static ::thyme::log::RateSite _thyme_log_rate_site;                \
        (void)::thyme::log::EmitFieldsRated(                               \
            THYME_LOG_SITE_(), &_thyme_log_rate_site,                      \
            ::thyme::log::Level::level_, policy_, msg_, fields_);          \
      }                                                                    \
    }                                                                      \
  } while (0)

#define THYME_LOG_ERROR(...) THYME_LOG_PLAIN_(kError, __VA_ARGS__)
#define THYME_LOG_WARN(...) THYME_LOG_PLAIN_(kWarn, __VA_ARGS__)
#define THYME_LOG_INFO(...) THYME_LOG_PLAIN_(kInfo, __VA_ARGS__)
#define THYME_LOG_DEBUG(...) THYME_LOG_PLAIN_(kDebug, __VA_ARGS__)

// Explicit policy (e.g. kOnceThenEvery1000 for a formerly one-shot site).
#define THYME_LOG_WARN_RATED(policy_, ...) \
  THYME_LOG_RATED_(kWarn, policy_, __VA_ARGS__)
#define THYME_LOG_INFO_RATED(policy_, ...) \
  THYME_LOG_RATED_(kInfo, policy_, __VA_ARGS__)
#define THYME_LOG_DEBUG_RATED(policy_, ...) \
  THYME_LOG_RATED_(kDebug, policy_, __VA_ARGS__)

// Rate-limited (hot path / per message). Default policy: first 3, then every
// 1000th, with suppressed=<n> on the emitted record.
#define THYME_LOG_ERROR_RL(...) \
  THYME_LOG_RATED_(kError, ::thyme::log::kDefaultRate, __VA_ARGS__)
#define THYME_LOG_WARN_RL(...) \
  THYME_LOG_RATED_(kWarn, ::thyme::log::kDefaultRate, __VA_ARGS__)
#define THYME_LOG_INFO_RL(...) \
  THYME_LOG_RATED_(kInfo, ::thyme::log::kDefaultRate, __VA_ARGS__)
#define THYME_LOG_DEBUG_RL(...) \
  THYME_LOG_RATED_(kDebug, ::thyme::log::kDefaultRate, __VA_ARGS__)

#define THYME_LOG_WARN_FIELDS(msg_, fields_) \
  THYME_LOG_FIELDS_(kWarn, msg_, fields_)
#define THYME_LOG_INFO_FIELDS(msg_, fields_) \
  THYME_LOG_FIELDS_(kInfo, msg_, fields_)
#define THYME_LOG_WARN_RL_FIELDS(msg_, fields_) \
  THYME_LOG_RATED_FIELDS_(kWarn, ::thyme::log::kDefaultRate, msg_, fields_)
#define THYME_LOG_INFO_RL_FIELDS(msg_, fields_) \
  THYME_LOG_RATED_FIELDS_(kInfo, ::thyme::log::kDefaultRate, msg_, fields_)
