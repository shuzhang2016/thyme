// test_pinned_slot_channel.cpp — payload-agnostic large-frame channel (design
// doc 4.3): three parameterizations (cloud-type MB slots, image-type fixed
// frames, h264-type 64 KB x many slots), variable data_len, footer/crc
// validation, tear detection, oversize Begin, blocking WaitView, and a
// cross-process (fork) reader.
#include "thyme/check.h"
#include "thyme/contracts/frame_header.h"
#include "thyme/runtime/pinned_slot_store.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <pthread.h>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

using namespace thyme;

namespace {

uint64_t NowNs() {
  struct timespec ts {};
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<uint64_t>(ts.tv_sec) * 1000000000ULL +
         static_cast<uint64_t>(ts.tv_nsec);
}

void FillPattern(uint8_t* p, uint32_t n, uint8_t seed) {
  for (uint32_t i = 0; i < n; ++i) {
    p[i] = static_cast<uint8_t>(seed + i * 7u);
  }
}

// Writes `count` frames with variable lengths (len_i = base + i*step, clamped
// to slot_bytes) through writer; returns total frames committed.
uint32_t WriteFrames(PinnedSlotWriter& w, uint32_t count, uint32_t base_len,
                     uint32_t step, uint32_t slot_bytes) {
  for (uint32_t i = 0; i < count; ++i) {
    uint32_t len = base_len + i * step;
    if (len > slot_bytes) len = slot_bytes;
    PinnedSlotWriter::BeginView v = w.Begin(len);
    CHECK(v.payload != nullptr);
    FillPattern(v.payload, len, static_cast<uint8_t>(i));
    v.header->flags.store(i, std::memory_order_relaxed);  // per-frame flag
    w.Commit(1000 + i);
  }
  return count;
}

bool ViewMatches(const PinnedSlotReader::View& v, uint64_t frame_index,
                 uint32_t base_len, uint32_t step, uint32_t slot_bytes) {
  uint32_t len = base_len + static_cast<uint32_t>(frame_index) * step;
  if (len > slot_bytes) len = slot_bytes;
  if (v.frame_index != frame_index || v.data_len != len ||
      v.mono_ns != 1000 + frame_index || v.flags != frame_index) {
    return false;
  }
  std::vector<uint8_t> expect(len);
  FillPattern(expect.data(), len, static_cast<uint8_t>(frame_index));
  return std::memcmp(v.payload, expect.data(), len) == 0;
}

// Generic exercise for one parameterization: write/read `frames` frames with
// variable lengths and verify payload bytes, metadata, and crc.
void RunParam(const char* tag, uint32_t slot_bytes, uint32_t slot_count,
              uint32_t meta_bytes, uint32_t frames) {
  char name[96];
  std::snprintf(name, sizeof(name), "/c9bfc_%s_%d", tag,
                static_cast<int>(getpid()));
  PinnedSlotWriter w;
  CHECK(w.Create(name, slot_bytes, slot_count, meta_bytes));
  // OPS-02: a second same-version writer must not unlink the live channel.
  PinnedSlotWriter duplicate;
  CHECK(!duplicate.Create(name, slot_bytes, slot_count, meta_bytes));
  CHECK(duplicate.create_status() == PinnedSlotWriter::CreateStatus::kCreateFailed);
  CHECK(w.head() == 0);
  w.SetCrcEnabled(true);  // REC-21: transport CRC defaults off; opt back in

  // Channel-level meta blob survives the trip to the reader.
  if (meta_bytes >= 8) {
    std::memset(w.MetaBlob(), 0, meta_bytes);
    std::memcpy(w.MetaBlob(), "GEOMETRY", 8);
  }

  WriteFrames(w, frames, 64, 32, slot_bytes);
  CHECK(w.head() == frames);

  PinnedSlotReader r;
  CHECK(r.Open(name, 1000000000ULL));
  CHECK(r.slot_bytes() == slot_bytes);
  CHECK(r.slot_count() == slot_count);
  if (meta_bytes >= 8) {
    CHECK(std::memcmp(r.MetaBlob(), "GEOMETRY", 8) == 0);
  }
  uint64_t expected_drops =
      frames > slot_count ? frames - slot_count : 0;
  uint64_t first = frames > slot_count ? frames - slot_count : 0;
  for (uint64_t i = first; i < frames; ++i) {
    PinnedSlotReader::View v;
    CHECK(r.TryRead(v));
    CHECK(ViewMatches(v, i, 64, 32, slot_bytes));
    CHECK(v.crc_ok);
  }
  CHECK(r.drop_count() == expected_drops);
  PinnedSlotReader::View none;
  CHECK(!r.TryRead(none));
  r.Close();
  w.Close();
  ShmRegion::UnlinkName(name);
}

struct DelayedCommitArg {
  PinnedSlotWriter* w;
  uint32_t len;
  uint64_t delay_ns;
};

void* DelayedCommit(void* p) {
  auto* a = static_cast<DelayedCommitArg*>(p);
  struct timespec ts {};
  ts.tv_sec = static_cast<time_t>(a->delay_ns / 1000000000ULL);
  ts.tv_nsec = static_cast<long>(a->delay_ns % 1000000000ULL);
  nanosleep(&ts, nullptr);
  PinnedSlotWriter::BeginView v = a->w->Begin(a->len);
  if (v.payload != nullptr) {
    FillPattern(v.payload, a->len, 0x5A);
    a->w->Commit();
  }
  return nullptr;
}

void SleepNs(uint64_t ns) {
  struct timespec ts {};
  ts.tv_sec = static_cast<time_t>(ns / 1000000000ULL);
  ts.tv_nsec = static_cast<long>(ns % 1000000000ULL);
  nanosleep(&ts, nullptr);
}

// Stats the lagging reader child ships back over the pipe.
struct LagStats {
  uint64_t frames;
  uint64_t drops;
  uint64_t torn;
};

// Cross-process consumption under a deliberately lagging reader: the writer
// bursts kLagFrames frames into a 4-slot channel while the reader child
// dawdles, so gap-skips must trigger; every delivered frame stays intact
// (content, crc, strictly increasing frame_index), the meta blob crosses the
// process boundary, and once the writer goes idle WaitView times out.
void RunCrossProcessLag() {
  constexpr uint64_t kLagFrames = 1000;
  constexpr uint32_t kSlotBytes = 4096;
  char name[96];
  std::snprintf(name, sizeof(name), "/c9bfc_lag_%d",
                static_cast<int>(getpid()));
  PinnedSlotWriter w;
  CHECK(w.Create(name, kSlotBytes, 4, 64));
  w.SetCrcEnabled(true);
  std::memcpy(w.MetaBlob(), "LAGMETA", 7);

  int sync_pipe[2];
  CHECK(pipe(sync_pipe) == 0);
  const pid_t pid = fork();
  CHECK(pid >= 0);
  if (pid == 0) {
    close(sync_pipe[0]);
    PinnedSlotReader r;
    CHECK(r.Open(name, 2000000000ULL));
    CHECK(std::memcmp(r.MetaBlob(), "LAGMETA", 7) == 0);  // meta crosses procs
    CHECK(write(sync_pipe[1], "R", 1) == 1);  // ready: writer may burst now
    SleepNs(20000000);  // 20 ms: guarantee the reader starts behind
    uint64_t last = ~0ULL;
    uint64_t count = 0;
    const uint64_t deadline = NowNs() + 30000000000ULL;
    for (;;) {
      PinnedSlotReader::View v;
      if (r.TryRead(v)) {
        CHECK(ViewMatches(v, v.frame_index, 64, 8, kSlotBytes));
        CHECK(v.crc_ok);
        if (last != ~0ULL) CHECK(v.frame_index > last);
        last = v.frame_index;
        ++count;
        if (last == kLagFrames - 1) break;
        SleepNs(500000);  // stay slow: keep gap-skip pressure on
      } else {
        CHECK(NowNs() < deadline);
        r.WaitUntil(NowNs() + 5000000ULL);
      }
    }
    // Drain: nothing may remain after the last frame.
    PinnedSlotReader::View v;
    CHECK(!r.TryRead(v));
    // Writer idle (not exited): WaitView must time out, not block forever.
    CHECK(!r.WaitView(v, NowNs() + 200000000ULL));
    LagStats stats{count, r.drop_count(), r.torn_read_count()};
    CHECK(write(sync_pipe[1], &stats, sizeof(stats)) ==
          static_cast<ssize_t>(sizeof(stats)));
    r.Close();
    _exit(0);
  }

  close(sync_pipe[1]);
  char b = 0;
  CHECK(read(sync_pipe[0], &b, 1) == 1 && b == 'R');
  WriteFrames(w, static_cast<uint32_t>(kLagFrames), 64, 8, kSlotBytes);
  LagStats stats{};
  CHECK(read(sync_pipe[0], &stats, sizeof(stats)) ==
        static_cast<ssize_t>(sizeof(stats)));
  int st = 0;
  CHECK(waitpid(pid, &st, 0) == pid);
  CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0);
  CHECK(stats.frames >= 1);
  CHECK(stats.frames < kLagFrames);  // lagging reader skipped a gap...
  CHECK(stats.drops > 0);            // ...and counted it
  CHECK(stats.torn == 0);            // slot-sized writes: no torn completions
  std::fprintf(stderr, "lag: frames=%llu drops=%llu\n",
               static_cast<unsigned long long>(stats.frames),
               static_cast<unsigned long long>(stats.drops));
  w.Close();
  ShmRegion::UnlinkName(name);
}

// Writer process exits WITHOUT closing/unlinking: the reader still drains
// every committed frame, then observes clean timeout semantics (no hang, no
// spurious frames).
void RunWriterExit() {
  constexpr uint64_t kFrames = 50;
  char name[96];
  std::snprintf(name, sizeof(name), "/c9bfc_wexit_%d",
                static_cast<int>(getpid()));
  ShmRegion::UnlinkName(name);
  const pid_t pid = fork();
  CHECK(pid >= 0);
  if (pid == 0) {
    PinnedSlotWriter w;
    CHECK(w.Create(name, 2048, 64, 0));  // 64 slots >= kFrames: all retained
    w.SetCrcEnabled(true);
    WriteFrames(w, static_cast<uint32_t>(kFrames), 128, 4, 2048);
    _exit(0);  // no Close(), no unlink: shm object outlives the producer
  }
  int st = 0;
  CHECK(waitpid(pid, &st, 0) == pid);
  CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0);

  PinnedSlotReader r;
  CHECK(r.Open(name, 2000000000ULL));
  for (uint64_t i = 0; i < kFrames; ++i) {
    PinnedSlotReader::View v;
    CHECK(r.TryRead(v));
    CHECK(ViewMatches(v, i, 128, 4, 2048));
    CHECK(v.crc_ok);
  }
  PinnedSlotReader::View none;
  CHECK(!r.TryRead(none));
  CHECK(!r.WaitView(none, NowNs() + 200000000ULL));  // times out, no frame
  CHECK(!r.WaitView(none, NowNs() + 200000000ULL));  // repeatable
  CHECK(r.drop_count() == 0);
  r.Close();
  ShmRegion::UnlinkName(name);
  // The writer (and therefore its lease companion) lived in the forked child,
  // which _exit()ed without running atexit cleanup.
  {
    const std::string lease = std::string(name) + ".lease";
    ShmRegion::UnlinkName(lease.c_str());
  }
}

// Random-access API (head/ReadFrame/slot_seq): the triggered-acquisition
// pre-window lookback and drain-time re-validation surface.
void RunRandomAccess() {
  char name[96];
  std::snprintf(name, sizeof(name), "/c9bfc_ra_%d", static_cast<int>(getpid()));
  PinnedSlotWriter w;
  CHECK(w.Create(name, 512, 8, 0));
  w.SetCrcEnabled(true);
  WriteFrames(w, 10, 100, 10, 512);  // frames 0..9; slots retain 2..9

  PinnedSlotReader r;
  CHECK(r.Open(name, 1000000000ULL));
  CHECK(r.head() == 10);

  using RR = PinnedSlotReader::ReadResult;
  PinnedSlotReader::View v;
  CHECK(r.ReadFrame(9, v) == RR::kOk);
  CHECK(ViewMatches(v, 9, 100, 10, 512));
  CHECK(v.slot_seq == 10);  // frame_index + 1
  CHECK(v.crc_ok);
  CHECK(r.ReadFrame(2, v) == RR::kOk);
  CHECK(v.slot_seq == 3);
  // Evicted: fast path (beyond retained window) and slot-replaced path.
  CHECK(r.ReadFrame(0, v) == RR::kOverrun);  // head - 0 = 10 > 8 slots
  CHECK(r.ReadFrame(1, v) == RR::kOverrun);  // slot 1 now holds frame 9
  // Not yet published.
  CHECK(r.ReadFrame(10, v) == RR::kNotPublished);
  CHECK(r.ReadFrame(999, v) == RR::kNotPublished);
  // Header-only harvest mode: no crc verification, slot_seq still filled.
  CHECK(r.ReadFrame(9, v, /*verify_crc=*/false) == RR::kOk);
  CHECK(!v.crc_ok);
  CHECK(v.slot_seq == 10);
  // ReadFrame does not move the follow cursor.
  CHECK(r.TryRead(v));
  CHECK(v.frame_index == 2);  // gap-skip 0..1, first retained
  CHECK(v.slot_seq == 3);
  CHECK(r.drop_count() == 2);

  // Killed-mid-write slot (seq == 0): ReadFrame reports kTorn, counted.
  ShmRegion rw;
  CHECK(rw.OpenRetry(name, PinnedSlotWriter::RegionBytes(512, 8, 0),
                     1000000000ULL, false));
  auto* h = static_cast<PinnedSlotPageHeader*>(rw.data());
  uint8_t* slot9 =
      reinterpret_cast<uint8_t*>(h) + PinnedSlotWriter::SlotsOffset(0) +
      (9 % 8) * PinnedSlotWriter::SlotStride(512);
  auto* hdr = reinterpret_cast<PinnedSlotHeader*>(slot9);
  const uint64_t saved = hdr->seq.load(std::memory_order_acquire);
  hdr->seq.store(0, std::memory_order_release);
  CHECK(r.ReadFrame(9, v) == RR::kTorn);
  CHECK(r.torn_read_count() == 1);
  hdr->seq.store(saved, std::memory_order_release);
  CHECK(r.ReadFrame(9, v) == RR::kOk);  // restored

  // The view borrows the slot and arms a debug watch; drop it before the
  // channel mapping goes away.
  v = PinnedSlotReader::View{};
  r.Close();
  rw.Unmap();
  w.Close();
  ShmRegion::UnlinkName(name);
}

}  // namespace

int main() {
  // Three parameterizations (sizes scaled down from production; layout and
  // semantics are size-independent):
  //   cloud-type: MB-class variable-length frames, few slots
  //   img-type:   fixed-size frames (writer sends full slot every time)
  //   h264-type:  64 KB slots x many, small variable AUs
  RunParam("cloud", 1u * 1024u * 1024u, 2, 64, 5);
  RunParam("img", 4096, 4, 128, 6);
  RunParam("h264", 64u * 1024u, 8, 64, 20);

  // --- Oversize Begin: nullptr + dropped_oversize, channel keeps working. ---
  char name[96];
  std::snprintf(name, sizeof(name), "/c9bfc_over_%d",
                static_cast<int>(getpid()));
  PinnedSlotWriter w;
  CHECK(w.Create(name, 1024, 2, 0));
  w.SetCrcEnabled(true);
  CHECK(w.Begin(1025).payload == nullptr);
  CHECK(w.dropped_oversize() == 1);
  CHECK(w.Begin(1024).payload != nullptr);  // exact fit works
  w.Commit(1);
  CHECK(w.dropped_oversize() == 1);

  // --- WaitView: timeout with no frames, then wake on Commit. ---
  {
    PinnedSlotReader r;
    CHECK(r.Open(name, 1000000000ULL));
    PinnedSlotReader::View v;
    CHECK(r.TryRead(v));  // the one committed frame
    CHECK(v.data_len == 1024);
    const uint64_t t0 = NowNs();
    CHECK(!r.WaitView(v, t0 + 50000000ULL));  // 50 ms timeout, no frame
    CHECK(NowNs() >= t0 + 40000000ULL);

    DelayedCommitArg arg{&w, 512, 30000000ULL};
    pthread_t th{};
    CHECK(pthread_create(&th, nullptr, DelayedCommit, &arg) == 0);
    CHECK(r.WaitView(v, NowNs() + 2000000000ULL));  // wakes on Commit
    CHECK(v.data_len == 512);
    CHECK(v.crc_ok);
    CHECK(pthread_join(th, nullptr) == 0);
    r.Close();
  }

  // --- Footer validation: stomp a committed frame's footer magic through a
  // second RW mapping; the reader must reject the frame as torn and skip it.
  {
    char n2[96];
    std::snprintf(n2, sizeof(n2), "/c9bfc_foot_%d",
                  static_cast<int>(getpid()));
    PinnedSlotWriter w2;
    CHECK(w2.Create(n2, 512, 4, 0));
    w2.SetCrcEnabled(true);
    WriteFrames(w2, 3, 100, 10, 512);

    ShmRegion rw;
    CHECK(rw.OpenRetry(n2, PinnedSlotWriter::RegionBytes(512, 4, 0),
                       1000000000ULL, false));
    auto* h = static_cast<PinnedSlotPageHeader*>(rw.data());
    uint8_t* slot1 =
        reinterpret_cast<uint8_t*>(h) + PinnedSlotWriter::SlotsOffset(0) +
        1 * PinnedSlotWriter::SlotStride(512);
    auto* footer = reinterpret_cast<PinnedSlotFooter*>(
        slot1 + sizeof(PinnedSlotHeader) + PinnedSlotWriter::RoundUp64(512));
    footer->magic.store(0, std::memory_order_release);  // corrupt completion

    PinnedSlotReader r2;
    CHECK(r2.Open(n2, 1000000000ULL));
    PinnedSlotReader::View v;
    CHECK(r2.TryRead(v));  // frame 0 intact
    CHECK(ViewMatches(v, 0, 100, 10, 512));
    CHECK(r2.TryRead(v));  // frame 1 torn -> skipped, frame 2 delivered
    CHECK(ViewMatches(v, 2, 100, 10, 512));
    CHECK(r2.torn_read_count() == 1);
    CHECK(r2.drop_count() == 1);
    CHECK(!r2.TryRead(v));
    r2.Close();
    rw.Unmap();
    w2.Close();
    ShmRegion::UnlinkName(n2);
  }

  // --- CRC mismatch is reported on the view (crc_ok=false) without dropping
  // the frame — matches the image channel's consumer-side semantics. ---
  {
    char n3[96];
    std::snprintf(n3, sizeof(n3), "/c9bfc_crc_%d",
                  static_cast<int>(getpid()));
    PinnedSlotWriter w3;
    CHECK(w3.Create(n3, 256, 2, 0));
    w3.SetCrcEnabled(true);
    PinnedSlotWriter::BeginView bv = w3.Begin(256);
    CHECK(bv.payload != nullptr);
    FillPattern(bv.payload, 256, 0x11);
    w3.Commit(42);

    ShmRegion rw;
    CHECK(rw.OpenRetry(n3, PinnedSlotWriter::RegionBytes(256, 2, 0),
                       1000000000ULL, false));
    auto* h = static_cast<PinnedSlotPageHeader*>(rw.data());
    uint8_t* payload =
        reinterpret_cast<uint8_t*>(h) + PinnedSlotWriter::SlotsOffset(0) +
        sizeof(PinnedSlotHeader);
    payload[0] ^= 0xFF;  // flip one payload byte

    PinnedSlotReader r3;
    CHECK(r3.Open(n3, 1000000000ULL));
    PinnedSlotReader::View v;
    CHECK(r3.TryRead(v));
    CHECK(v.data_len == 256);
    CHECK(!v.crc_ok);
    CHECK(r3.torn_read_count() == 0);
    r3.Close();
    rw.Unmap();
    w3.Close();
    ShmRegion::UnlinkName(n3);
  }

  // --- Cross-process reader (fork + read-only attach). ---
  {
    char n4[96];
    std::snprintf(n4, sizeof(n4), "/c9bfc_xp_%d", static_cast<int>(getpid()));
    PinnedSlotWriter w4;
    CHECK(w4.Create(n4, 2048, 4, 64));
    w4.SetCrcEnabled(true);
    std::memcpy(w4.MetaBlob(), "XPROC", 5);
    WriteFrames(w4, 3, 200, 100, 2048);

    pid_t pid = fork();
    CHECK(pid >= 0);
    if (pid == 0) {
      PinnedSlotReader r4;
      CHECK(r4.Open(n4, 1000000000ULL));
      CHECK(std::memcmp(r4.MetaBlob(), "XPROC", 5) == 0);
      for (uint64_t i = 0; i < 3; ++i) {
        PinnedSlotReader::View v;
        CHECK(r4.TryRead(v));
        CHECK(ViewMatches(v, i, 200, 100, 2048));
        CHECK(v.crc_ok);
      }
      PinnedSlotReader::View none;
      CHECK(!r4.TryRead(none));
      r4.Close();
      _exit(0);
    }
    int st = 0;
    CHECK(waitpid(pid, &st, 0) == pid);
    CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0);
    w4.Close();
    ShmRegion::UnlinkName(n4);
  }

  // --- Lagging reader gap-skip: more frames than slots drop the oldest. ---
  {
    char n5[96];
    std::snprintf(n5, sizeof(n5), "/c9bfc_gap_%d", static_cast<int>(getpid()));
    PinnedSlotWriter w5;
    CHECK(w5.Create(n5, 128, 2, 0));
    w5.SetCrcEnabled(true);
    WriteFrames(w5, 5, 32, 8, 128);
    PinnedSlotReader r5;
    CHECK(r5.Open(n5, 1000000000ULL));
    PinnedSlotReader::View v;
    CHECK(r5.TryRead(v));
    CHECK(v.frame_index == 3);  // frames 0..2 evicted
    CHECK(r5.drop_count() == 3);
    CHECK(r5.TryRead(v));
    CHECK(v.frame_index == 4);
    CHECK(!r5.TryRead(v));
    r5.Close();
    w5.Close();
    ShmRegion::UnlinkName(n5);
  }

  w.Close();
  ShmRegion::UnlinkName(name);

  // Multi-process scenarios.
  RunCrossProcessLag();
  RunWriterExit();
  RunRandomAccess();
  return 0;
}
