#pragma once
// perf_dump.h — crash-safe periodic perf dump (design review, option 1): new
// samples/records are appended to an O_APPEND file every interval_ns, so a
// SIGKILL loses at most one interval of perf data. Deliberately NO signal
// handler flush: nothing here is async-signal-safe, and the periodic append
// already bounds the loss window.
//
// The dumper is driven by the host loop (cyclic thread / host tick), keeping
// the library thread-free.
//
// OBS-03 dump-then-reset: after a dump is fully written, the source rings
// (PerfSampler / PerfWorkLog / ProcSampler) are Reset — each dump block then
// covers exactly one non-overlapping window (the FlushNew watermark already
// guarantees no duplicates; the reset additionally bounds the retained
// window, which is what makes rate×interval ring sizing sufficient). If the
// write FAILS the rings are kept (their content stays reachable for a final
// Flush), though the FlushNew watermark has already advanced. The writer is
// allocated once in Open (not per dump), so DumpIfDue stays allocation-free.

#include "thyme/runtime/perf_log.h"
#include "thyme/runtime/proc_sampler.h"
#include "thyme/runtime/rotating_file.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>

namespace thyme {

// OPS-05: perf dumps rotate like every other append-only log: <path> is
// renamed to <path>.1 (then ...N) when it reaches max_bytes, so a long-running
// host cannot fill the disk with perf data. Each generation is a complete
// T9P1 file (it carries the magic), so `thyme_log_merge` can consume the live
// path and its `.1..N` siblings as independent inputs.
struct PerfDumpRotation {
  uint64_t max_bytes = 64ull * 1024 * 1024;
  uint32_t rotate_keep = 2;
};

class PerfLogDumper {
 public:
  PerfLogDumper() = default;
  ~PerfLogDumper() { Close(); }
  PerfLogDumper(const PerfLogDumper&) = delete;
  PerfLogDumper& operator=(const PerfLogDumper&) = delete;

  // O_APPEND: survives a crash mid-write with at most a partial tail record;
  // the parser stops at the first short block. Default rotation: 64 MiB per
  // file, 2 generations (<= 192 MiB + one dump per file).
  //
  // sample_cap/work_cap size the reusable write buffer; they must be >= the
  // capacity() of every sampler/work log ever passed to DumpIfDue. A mismatch
  // is a wiring bug: DumpIfDue aborts before the interval check or any append.
  // The defaults cover default-constructed (kCap) rings.
  bool Open(const char* path, uint64_t interval_ns,
            std::size_t sample_cap = PerfSampler::kCap,
            std::size_t work_cap = PerfWorkLog::kCap) {
    return Open(path, interval_ns, PerfDumpRotation{}, sample_cap, work_cap);
  }

  bool Open(const char* path, uint64_t interval_ns,
            const PerfDumpRotation& rotation,
            std::size_t sample_cap = PerfSampler::kCap,
            std::size_t work_cap = PerfWorkLog::kCap) {
    Close();
    logging::RotateConfig rcfg;
    rcfg.path = path;
    rcfg.max_bytes = rotation.max_bytes;
    rcfg.keep = rotation.rotate_keep;
    rcfg.mandatory = false;
    rcfg.account = true;
    if (!file_.Open(rcfg)) return false;
    writer_ = PerfLogWriter(sample_cap, work_cap);
    if (!writer_.ok()) {
      Close();
      return false;
    }
    sample_cap_ = sample_cap;
    work_cap_ = work_cap;
    interval_ns_ = interval_ns;
    wrote_magic_ = false;
    series_metadata_written_ = false;
    node_budget_written_ = false;
    last_ns_ = 0;
    return true;
  }

  void Close() { file_.Close(); }

  bool ok() const { return file_.enabled(); }

  const logging::RotatingFile& file() const { return file_; }

  // ABI-10: pin the GEN-12 record manifest (hash + version) in every file
  // generation. Call after Open; (0, 0) disables the block, keeping the log
  // byte-identical to the pre-ABI-10 format (legacy readers see no block).
  void SetRecordManifest(uint64_t hash, uint16_t version) {
    manifest_hash_ = hash;
    manifest_version_ = version;
    manifest_written_ = false;
  }

  void SetNodeBudget(uint64_t period_ns, uint64_t deadline_ns) {
    node_period_ns_ = period_ns;
    node_deadline_ns_ = deadline_ns;
    node_budget_set_ = true;
    node_budget_written_ = false;
  }

  // Append this node's not-yet-dumped samples when the interval has elapsed.
  // Each dump is preceded by an epoch block (P2): the u32 sample timestamps
  // wrap every ~19.5 h; the epoch record carries the current wrap base so
  // analysis can expand them (and bump on a low-word regression). ARCH-15 adds
  // a file-wide full-width mono/realtime anchor to every non-empty dump.
  // `sys` (OBS-04, optional) appends the process counter / queue-depth
  // series as one type-5 block on the same watermark contract.
  // On a successful write the rings are Reset (dump-then-reset, OBS-03).
  // Returns true if anything was written.
  bool DumpIfDue(uint64_t now_ns, PerfSampler& sampler, PerfWorkLog* work,
                 uint8_t node_id, ProcSampler* sys = nullptr) {
    if (!file_.enabled() || !writer_.ok()) return false;
    if (sampler.capacity() > sample_cap_ ||
        (work != nullptr && work->capacity() > work_cap_)) {
      std::fprintf(stderr,
                   "PerfLogDumper: ring capacity exceeds the writer budget "
                   "passed to Open (wiring bug)\n");
      std::abort();
    }
    if (last_ns_ != 0 && now_ns - last_ns_ < interval_ns_) return false;
    struct timespec realtime {};
    if (clock_gettime(CLOCK_REALTIME, &realtime) != 0 || realtime.tv_sec < 0) {
      return false;  // no anchor: keep the rings and retry on a later dump
    }
    const uint64_t realtime_ns =
        static_cast<uint64_t>(realtime.tv_sec) * 1000000000ull +
        static_cast<uint64_t>(realtime.tv_nsec);
    last_ns_ = now_ns;
    // OPS-05: rotate BEFORE choosing the skip prefix. A rotated file is empty
    // and must receive the T9P1 magic again, otherwise the generation would
    // not be independently parseable. The ABI-10 manifest block is emitted
    // once per generation for the same reason.
    if (file_.RotateIfNeeded()) {
      wrote_magic_ = false;
      manifest_written_ = false;
      series_metadata_written_ = false;
      node_budget_written_ = false;
    }
    writer_.Rewind();
    PerfLogWriter& bw = writer_;
    // ABI-10: manifest block first (it describes every block that follows).
    // It is rebuilt into the reusable buffer until a dump actually lands, so
    // a `return false` below never marks it written.
    if (!manifest_written_ && manifest_version_ != 0) {
      bw.U8(kPerfManifestBlockType);
      bw.U8(0);  // build-wide: not attributed to a node
      bw.U32(1);
      bw.U16(manifest_version_);
      bw.U16(0);  // reserved, must be 0
      bw.U64(manifest_hash_);
    }
    if (sys != nullptr && !series_metadata_written_) {
      sys->FlushMetadata(bw, node_id);
    }
    if (node_budget_set_ && !node_budget_written_) {
      WritePerfNodeBudget(bw, node_id, node_period_ns_, node_deadline_ns_);
    }
    // Epoch block: type 4, one 8-byte record per node block.
    bw.U8(PerfSampler::kEpochBlockType);
    bw.U8(node_id);
    bw.U32(1);
    bw.U64((now_ns >> PerfSampler::kTsShift) & ~0xFFFFFFFFull);
    WritePerfClockAnchor(bw, now_ns, realtime_ns);
    const std::size_t before_content = bw.size();
    sampler.FlushNew(bw, node_id);
    if (work != nullptr) work->FlushNew(bw, node_id);
    if (sys != nullptr) sys->FlushNew(bw, node_id);
    if (bw.size() == before_content) return false;  // nothing new: skip epoch + anchor
    const std::size_t skip = wrote_magic_ ? 4 : 0;
    if (bw.size() <= skip) return false;
    // One unbuffered write() (O_APPEND): no user-space buffer means a crash
    // loses at most the tail of this dump, and the disk's latency never sits
    // in the host loop beyond that single write.
    const std::ptrdiff_t wrote =
        file_.Append(bw.data() + skip, bw.size() - skip);
    const bool ok = wrote == static_cast<std::ptrdiff_t>(bw.size() - skip);
    if (ok) {
      // OBS-03 dump-then-reset: the dumped window is on disk; the rings now
      // hold only samples recorded after this dump.
      sampler.Reset();
      if (work != nullptr) work->Reset();
      if (sys != nullptr) sys->Reset();
    }
    // A budget-dropped dump retries the magic and all per-generation metadata.
    if (wrote > 0) wrote_magic_ = true;
    if (ok && manifest_version_ != 0) manifest_written_ = true;
    if (ok && sys != nullptr) series_metadata_written_ = true;
    if (ok && node_budget_set_) node_budget_written_ = true;
    return ok;
  }

 private:
  logging::RotatingFile file_;
  PerfLogWriter writer_{};
  std::size_t sample_cap_ = PerfSampler::kCap;
  std::size_t work_cap_ = PerfWorkLog::kCap;
  uint64_t interval_ns_ = 0;
  uint64_t last_ns_ = 0;
  bool wrote_magic_ = false;
  // ABI-10 manifest block state (see SetRecordManifest).
  uint64_t manifest_hash_ = 0;
  uint16_t manifest_version_ = 0;
  bool manifest_written_ = false;
  bool series_metadata_written_ = false;
  uint64_t node_period_ns_ = 0;
  uint64_t node_deadline_ns_ = 0;
  bool node_budget_set_ = false;
  bool node_budget_written_ = false;
};

}  // namespace thyme
