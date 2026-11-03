#pragma once

// rotating_file.h — OPS-05: uniform size-cap + generation-count rotation for
// the append-only observability files (trigger audit, <socket>.fsm JSONL and
// the T9P1 perf dumps).
//
// Contract (same discipline as the SEC-02 audit writer, which this class now
// backs):
//   * O_APPEND + exactly one write() per Append. There is no user-space
//     buffer and no retry loop: a slow/full disk stalls the caller for at most
//     one write and a partial tail is left for the parser to stop at.
//   * The size check is one integer comparison on the commit path. When the
//     live file has reached max_bytes it is rotated BEFORE the oversized
//     record is written, so a file overshoots the cap by at most one record
//     (or one perf dump block, which is the same contract).
//   * Rotation is rename-based: <path> -> <path>.1, <path>.1 -> <path>.2, ...,
//     <path>.<keep> is unlinked first, so generations cost at most
//     max_bytes * (keep + 1) plus one record.
//   * A crash (SIGKILL/power loss) can leave a partial last line/block; it can
//     never interleave two records or truncate an older one. Every rotated
//     generation is a complete, independently parseable file (perf generations
//     each carry the T9P1 magic they had when they were live).
//
// LogBudget is a process-wide upper bound shared by every RotatingFile whose
// RotateConfig::account is set. Non-mandatory writers drop (and count) records
// once the budget is exhausted; mandatory writers (the SEC-02 audit trail)
// never drop — a deployment cannot trade auditability for disk. The bound is
// per PROCESS: MP role processes each own their budget, so the fleet worst
// case is the sum over processes (documented in docs/guides/trigger-config.md).
//
// Thread-safety: one writer per RotatingFile instance. LogBudget is atomic and
// safe across threads (perf dumps from a host thread + the center tick thread
// share one process budget).

#include <cstddef>
#include <cstdint>
#include <string>

namespace thyme {
namespace logging {

// One writer's rotation geometry. max_bytes == 0 or keep == 0 disables
// rotation (unbounded growth — only for configs that explicitly opt out of a
// bound; the shipped defaults never are).
struct RotateConfig {
  std::string path;
  uint64_t max_bytes = 0;
  uint32_t keep = 0;  // generations kept as <path>.1 .. <path>.N
  // mandatory = budget never drops this writer (audit trail). It is still
  // counted, so the budget reflects the real disk footprint.
  bool mandatory = false;
  // account = participate in LogBudget (default). false keeps a writer out of
  // the process budget (used by tests and by writers that are externally
  // bounded).
  bool account = true;
};

// Process-wide log budget (bytes). Atomic; cheap on the append path (two
// relaxed loads for non-mandatory writers).
class LogBudget {
 public:
  // 0 = no aggregate budget (per-file caps only, the default).
  static void Configure(uint64_t limit_bytes);
  static uint64_t limit_bytes();
  static uint64_t used_bytes();
  static uint64_t drops();      // records dropped because the budget was full
  static uint64_t rotations();  // rotations across all accounted writers
  // SCH-06: rotations postponed because the caller was inside an RT
  // dispatch/Loop section. The record is still appended; rotation happens on
  // the next non-RT append (or an explicit RotateIfNeeded from a slow path).
  static uint64_t deferred_rotations();

  static bool WouldExceed(uint64_t bytes);
  static void Add(uint64_t bytes);
  static void Subtract(uint64_t bytes);
  static void NoteDrop();
  static void NoteRotation();
  static void NoteDeferredRotation();

  // Test helper: limit = 0, every counter = 0. Not for production paths.
  static void ResetForTest();
};

class RotatingFile {
 public:
  RotatingFile() = default;
  ~RotatingFile();
  RotatingFile(const RotatingFile&) = delete;
  RotatingFile& operator=(const RotatingFile&) = delete;

  // False when the file cannot be created. Open never truncates an existing
  // file; it adopts its size, rotates it if it is already at the cap, and
  // accounts the on-disk generations in LogBudget.
  bool Open(const RotateConfig& cfg);
  void Close();
  bool Sync();  // fdatasync; cheap no-op when closed

  // One unbuffered write().
  //   == len  full record committed
  //   >  0    short write (partial tail; caller may terminate the line)
  //   == 0    dropped by LogBudget (nothing written)
  //   <  0    fd not open / write error (nothing written)
  std::ptrdiff_t Append(const void* data, std::size_t len);

  // Rotate now when the live file reached max_bytes. Rotation is also done
  // inside Append; this entry point exists for writers that must re-emit a
  // file header (perf dumps) before the next append.
  bool RotateIfNeeded();

  bool enabled() const { return fd_ >= 0; }
  int fd() const { return fd_; }
  const std::string& path() const { return path_; }
  uint64_t size_bytes() const { return size_bytes_; }      // live file
  uint64_t bytes_written() const { return bytes_written_; }
  uint64_t records() const { return records_; }            // complete appends
  uint64_t drops() const { return drops_; }
  uint64_t rotations() const { return rotations_; }
  // ARCH-05: unlink/rename failures during rotation (other than the expected
  // ENOENT for a generation that never existed). The records are still
  // appended; only the on-disk generation layout is affected.
  uint64_t rotate_errors() const { return rotate_errors_; }

  // Live + retained generations (stat() each; call from slow paths only).
  uint64_t DiskBytes() const;

 private:
  bool Reopen();
  bool Rotate();
  uint64_t GenerationSize(uint32_t gen) const;
  void ForgetAccounting();

  int fd_ = -1;
  std::string path_;
  uint64_t max_bytes_ = 0;
  uint32_t keep_ = 0;
  bool mandatory_ = false;
  bool account_ = true;
  uint64_t size_bytes_ = 0;   // live file size
  uint64_t bytes_written_ = 0;
  uint64_t records_ = 0;
  uint64_t drops_ = 0;
  uint64_t rotations_ = 0;
  uint64_t rotate_errors_ = 0;
  uint64_t accounted_ = 0;  // bytes this instance registered in LogBudget
};

}  // namespace logging
}  // namespace thyme
