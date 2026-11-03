// test_crash_injection.cpp — fault-injection tests for the "killed process
// leaves no dirty/unusable shm" semantics of the triggered-acquisition
// components:
//
//   1. PinnedSlotStore: producer SIGKILLed between Begin() and Commit() —
//      the slot's seqlock stays 0 (write in progress) and the footer is never
//      written; committed frames remain fully readable, the killed slot is
//      invisible (hole), and readers never block on it.
//   2. RecordMirrorRingPage: a slot jammed to an odd seq (killed mid-
//      TryWriteRing) is skipped by ReadRingSample/HarvestWindow while the
//      surrounding samples and the window harvest stay intact.
//   3. Full SIGKILL/restart cycle: kill -9 mid-stream -> readers see exactly
//      the committed prefix (no half-frame dirt) -> new producer Creates the
//      same names with a new session -> re-attached readers see fresh data
//      while stale mappings still show the old session.
//   4. Recorder (reader process) SIGKILLed: the producer keeps
//      Push/Commit-ing without backpressure; a fresh reader re-attaches and
//      harvests the retained window.
//
// A global alarm() watchdog bounds every wait; ctest sees a hang as failure.
#include "thyme/check.h"
#include "thyme/contracts/frame_header.h"
#include "thyme/record/record_mirror_ring_client.h"
#include "thyme/runtime/pinned_slot_store.h"
#include "thyme/runtime/record_mirror_ring.h"
#include "thyme/runtime/shm_region.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <csignal>
#include <dirent.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

using namespace thyme;

namespace {
void UnlinkShmPair(const char* n) {
  ShmRegion::UnlinkName(n);
  const std::string lease = std::string(n) + ".lease";
  ShmRegion::UnlinkName(lease.c_str());
}


constexpr uint32_t kSlotBytes = 2048;

struct CrashPod {
  uint64_t index;
  uint64_t tag;
};

struct FrameRec {
  const uint8_t* payload;
  uint64_t frame_index;
  uint64_t mono_ns;
  uint32_t data_len;
  uint32_t flags;
  uint32_t crc32;
};

uint64_t NowNs() {
  struct timespec ts {};
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<uint64_t>(ts.tv_sec) * 1000000000ULL +
         static_cast<uint64_t>(ts.tv_nsec);
}

void SleepNs(uint64_t ns) {
  struct timespec ts {};
  ts.tv_sec = static_cast<time_t>(ns / 1000000000ULL);
  ts.tv_nsec = static_cast<long>(ns % 1000000000ULL);
  nanosleep(&ts, nullptr);
}

void FillFrame(uint8_t* p, uint32_t len, uint64_t f) {
  for (uint32_t j = 0; j < len; ++j) {
    p[j] = static_cast<uint8_t>(f * 31 + j * 3);
  }
}

void WriteFrame(PinnedSlotWriter& w, uint64_t f) {
  const uint32_t len = 256 + static_cast<uint32_t>(f % 4) * 128;
  PinnedSlotWriter::BeginView bv = w.Begin(len);
  CHECK(bv.payload != nullptr);
  FillFrame(bv.payload, len, f);
  w.Commit(1000 + f);
}

bool FrameRecValid(const FrameRec& r) {
  const uint32_t expect_len =
      256 + static_cast<uint32_t>(r.frame_index % 4) * 128;
  if (r.data_len != expect_len || r.mono_ns != 1000 + r.frame_index) {
    return false;
  }
  for (uint32_t j = 0; j < r.data_len; ++j) {
    if (r.payload[j] != static_cast<uint8_t>(r.frame_index * 31 + j * 3)) {
      return false;
    }
  }
  return Crc32(r.payload, r.data_len) == r.crc32;
}

void DrainFrames(PinnedSlotReader& r, std::vector<FrameRec>& out) {
  PinnedSlotReader::View v;
  while (r.TryRead(v)) {
    FrameRec rec{v.payload,  v.frame_index, v.mono_ns,
                 v.data_len, v.flags,       v.crc32};
    out.push_back(rec);
  }
}

void ExpectKilled(int st) {
  CHECK(WIFSIGNALED(st) && WTERMSIG(st) == SIGKILL);
}

// Soft /dev/shm leak check (informational, not asserted): our test must leave
// no c9crash_* objects behind.
int CountCrashShm() {
  DIR* d = opendir("/dev/shm");
  if (d == nullptr) return -1;
  int n = 0;
  while (dirent* e = readdir(d)) {
    if (std::strncmp(e->d_name, "c9crash", 7) == 0) ++n;
  }
  closedir(d);
  return n;
}

// ---------------------------------------------------------------- scenario 1

// Producer SIGKILLed after Begin() but before Commit(): the slot keeps
// seq == 0 and no footer. Readers must observe the committed prefix, never
// see the partial frame, and never block on the dead slot.
void RunKilledMidBegin() {
  char name[64];
  std::snprintf(name, sizeof(name), "/c9crash_begin_%d",
                static_cast<int>(getpid()));
  ShmRegion::UnlinkName(name);

  const pid_t pid = fork();
  CHECK(pid >= 0);
  if (pid == 0) {
    PinnedSlotWriter w;
    CHECK(w.Create(name, kSlotBytes, 8, 0));
    w.SetCrcEnabled(true);
    for (uint64_t f = 0; f < 3; ++f) WriteFrame(w, f);  // 3 committed frames
    PinnedSlotWriter::BeginView bv = w.Begin(kSlotBytes);  // 4th: started...
    CHECK(bv.payload != nullptr);
    FillFrame(bv.payload, kSlotBytes / 2, 3);  // ...partially packed...
    raise(SIGKILL);  // ...then killed: no footer, no Commit
    _exit(1);
  }
  int st = 0;
  CHECK(waitpid(pid, &st, 0) == pid);
  ExpectKilled(st);

  PinnedSlotReader r;
  CHECK(r.Open(name, 1000000000ULL));
  for (uint64_t f = 0; f < 3; ++f) {
    PinnedSlotReader::View v;
    CHECK(r.TryRead(v));
    CHECK(v.frame_index == f);
    FrameRec rec{v.payload,  v.frame_index, v.mono_ns,
                 v.data_len, v.flags,       v.crc32};
    CHECK(FrameRecValid(rec));  // committed frames fully intact, crc passes
  }
  // The killed slot: not visible, not torn, and no read blocks on it.
  PinnedSlotReader::View v;
  CHECK(!r.TryRead(v));
  const uint64_t t0 = NowNs();
  CHECK(!r.WaitView(v, t0 + 100000000ULL));  // bounded: 100 ms deadline
  CHECK(NowNs() < t0 + 1000000000ULL);       // returned near the deadline
  CHECK(r.drop_count() == 0);
  CHECK(r.torn_read_count() == 0);
  r.Close();

  // Probe the physical slot: seqlock left at 0 (write in progress), footer
  // magic never written — "no footer = frame invisible" held above.
  ShmRegion rw;
  CHECK(rw.OpenRetry(name, PinnedSlotWriter::RegionBytes(kSlotBytes, 8, 0),
                     1000000000ULL, false));
  auto* h = static_cast<PinnedSlotPageHeader*>(rw.data());
  CHECK(h->head.load(std::memory_order_acquire) == 3);  // never advanced
  const uint8_t* slot3 = reinterpret_cast<uint8_t*>(h) +
                         PinnedSlotWriter::SlotsOffset(0) +
                         3 * PinnedSlotWriter::SlotStride(kSlotBytes);
  const auto* slot_hdr = reinterpret_cast<const PinnedSlotHeader*>(slot3);
  CHECK(slot_hdr->seq.load(std::memory_order_acquire) == 0);
  const auto* footer = reinterpret_cast<const PinnedSlotFooter*>(
      slot3 + sizeof(PinnedSlotHeader) + PinnedSlotWriter::RoundUp64(kSlotBytes));
  CHECK(footer->magic.load(std::memory_order_acquire) != kPinnedSlotFooterMagic);
  rw.Unmap();
  UnlinkShmPair(name);
}

// ---------------------------------------------------------------- scenario 2

// A mirror-ring slot jammed to an odd seq (the state a SIGKILL mid-
// TryWriteRing leaves behind) is skipped by readers; surrounding samples and
// the window harvest stay intact.
void RunMirrorKilledMidWrite() {
  char name[64];
  std::snprintf(name, sizeof(name), "/c9crash_mirror_%d",
                static_cast<int>(getpid()));
  ShmRegion::UnlinkName(name);

  ThymeStructDesc desc{};
  desc.struct_id = 0;
  desc.contract_version = 1;
  desc.payload_size = sizeof(CrashPod);
  std::snprintf(desc.name, sizeof(desc.name), "CrashPod");
  RecordMirrorRingSpec spec{sizeof(CrashPod), 16};
  const uint64_t hash = RecordMirrorRingPage::HashConfig(&desc, &spec, 1);

  record_mirror::RingClient owner;
  CHECK(owner.Create(name, &desc, &spec, 1, 5));
  for (uint64_t i = 0; i < 5; ++i) {
    CrashPod p{i, 0xC0FFEE00 + i};
    CHECK(owner.Page()->TryWriteRing(0, &p, sizeof(p), i * 10,
                                     static_cast<uint32_t>(i)));
  }
  CHECK(owner.Page()->RingHead(0) == 5);

  // Simulate the kill site: slot of publish_index 2 stuck mid-write.
  {
    RecordMirrorRingDesc& d = owner.Page()->ring_descs[0];
    auto* slot = reinterpret_cast<RecordMirrorRingSlotHeader*>(
        owner.Page()->ArenaBase() + d.slots_offset +
        (2 % d.ring_depth) * d.slot_stride);
    const uint32_t s = slot->seq.load(std::memory_order_acquire);
    CHECK((s & 1u) == 0);
    slot->seq.store(s | 1u, std::memory_order_release);
  }

  const pid_t pid = fork();
  CHECK(pid >= 0);
  if (pid == 0) {
    // Read-only attached reader: the killed slot is a hole, not a blocker.
    record_mirror::RingClient ro;
    CHECK(ro.AttachReadOnly(name, 1000000000ULL, hash));
    const RecordMirrorRingPage* page = ro.Page();
    uint64_t torn = 0;
    RecordMirrorRingSampleDesc buf[16];
    const uint32_t n = page->HarvestWindow(0, 0, ~0ULL, buf, 16, &torn);
    CHECK(n == 4);       // 5 samples minus the killed slot
    CHECK(torn == 1);    // ...counted once during harvest
    const uint64_t expect[4] = {0, 1, 3, 4};
    for (uint32_t i = 0; i < 4; ++i) {
      CHECK(buf[i].publish_index == expect[i]);
      CrashPod p{};
      uint32_t sz = 0;
      uint64_t mono = 0;
      uint32_t sseq = 0;
      CHECK(page->ReadRingSample(0, buf[i].publish_index,
                                 reinterpret_cast<uint8_t*>(&p), sizeof(p), sz,
                                 mono, sseq, nullptr, &torn));
      CHECK(p.index == expect[i]);
      CHECK(p.tag == 0xC0FFEE00 + expect[i]);
      CHECK(mono == expect[i] * 10);
    }
    // Reading the killed slot itself: bounded retries, false, torn-counted.
    CrashPod p{};
    uint32_t sz = 0;
    uint64_t mono = 0;
    uint32_t sseq = 0;
    CHECK(!page->ReadRingSample(0, 2, reinterpret_cast<uint8_t*>(&p),
                                sizeof(p), sz, mono, sseq, nullptr, &torn));
    CHECK(torn == 2);
    _exit(0);
  }
  int st = 0;
  CHECK(waitpid(pid, &st, 0) == pid);
  CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0);
  owner.Shutdown(true);
}

// ---------------------------------------------------------------- scenario 3

// kill -9 mid-stream -> readers see exactly the committed prefix -> a new
// producer re-Creates the same names with a new session -> re-attached
// readers see fresh data; the stale mapping still shows the old session.
void RunSigkillRestart() {
  char page_name[64], chan_name[64];
  std::snprintf(page_name, sizeof(page_name), "/c9crash_rs_page_%d",
                static_cast<int>(getpid()));
  std::snprintf(chan_name, sizeof(chan_name), "/c9crash_rs_chan_%d",
                static_cast<int>(getpid()));
  ShmRegion::UnlinkName(page_name);
  UnlinkShmPair(chan_name);

  ThymeStructDesc desc{};
  desc.struct_id = 0;
  desc.contract_version = 1;
  desc.payload_size = sizeof(CrashPod);
  std::snprintf(desc.name, sizeof(desc.name), "CrashPod");
  RecordMirrorRingSpec spec{sizeof(CrashPod), 64};
  const uint64_t hash = RecordMirrorRingPage::HashConfig(&desc, &spec, 1);

  int ready_pipe[2];
  CHECK(pipe(ready_pipe) == 0);
  pid_t pid = fork();
  CHECK(pid >= 0);
  if (pid == 0) {
    close(ready_pipe[0]);
    record_mirror::RingClient mirror;
    CHECK(mirror.Create(page_name, &desc, &spec, 1, 11));
    PinnedSlotWriter w;
    CHECK(w.Create(chan_name, kSlotBytes, 32, 0));
    w.SetCrcEnabled(true);  // REC-21: transport CRC defaults off; opt back in
    CHECK(write(ready_pipe[1], "R", 1) == 1);
    for (uint64_t i = 0; i < 20; ++i) {
      CrashPod p{i, 0x1100 + i};
      CHECK(mirror.Page()->TryWriteRing(0, &p, sizeof(p), i * 10,
                                        static_cast<uint32_t>(i)));
      WriteFrame(w, i);
      SleepNs(2000000);  // 2 ms: parent kills mid-stream
    }
    _exit(0);
  }
  close(ready_pipe[1]);
  char b = 0;
  CHECK(read(ready_pipe[0], &b, 1) == 1 && b == 'R');
  close(ready_pipe[0]);

  record_mirror::RingClient ro1;
  CHECK(ro1.AttachReadOnly(page_name, 1000000000ULL, hash));
  const RecordMirrorRingPage* page1 = ro1.Page();
  PinnedSlotReader fr1;
  CHECK(fr1.Open(chan_name, 1000000000ULL));

  // Wait until at least 5 frames are committed, then SIGKILL the producer.
  std::vector<FrameRec> frecs;
  const uint64_t deadline = NowNs() + 5000000000ULL;
  while (frecs.size() < 5) {
    DrainFrames(fr1, frecs);
    CHECK(NowNs() < deadline);
    SleepNs(500000);
  }
  CHECK(kill(pid, SIGKILL) == 0);
  int st = 0;
  CHECK(waitpid(pid, &st, 0) == pid);
  ExpectKilled(st);
  DrainFrames(fr1, frecs);

  // Exactly the committed prefix: contiguous 0..m-1, every frame intact, no
  // torn frame ever surfaced.
  CHECK(frecs.size() >= 5 && frecs.size() <= 20);
  for (std::size_t i = 0; i < frecs.size(); ++i) {
    CHECK(frecs[i].frame_index == i);
    CHECK(FrameRecValid(frecs[i]));
  }
  CHECK(fr1.torn_read_count() == 0);
  CHECK(fr1.drop_count() == 0);
  PinnedSlotReader::View v;
  CHECK(!fr1.WaitView(v, NowNs() + 100000000ULL));  // dead producer: bounded

  // Mirror side: the committed prefix 0..h-1 reads back byte-perfect; the
  // possibly-killed-mid-write slot at index h is simply not visible.
  const uint64_t h = page1->RingHead(0);
  CHECK(h >= 5 && h <= 20);
  uint64_t torn = 0;
  for (uint64_t i = 0; i < h; ++i) {
    CrashPod p{};
    uint32_t sz = 0;
    uint64_t mono = 0;
    uint32_t sseq = 0;
    CHECK(page1->ReadRingSample(0, i, reinterpret_cast<uint8_t*>(&p),
                                sizeof(p), sz, mono, sseq, nullptr, &torn));
    CHECK(p.index == i && p.tag == 0x1100 + i);
    CHECK(mono == i * 10);
  }
  CrashPod beyond{};
  uint32_t sz = 0;
  uint64_t mono = 0;
  uint32_t sseq = 0;
  CHECK(!page1->ReadRingSample(0, h, reinterpret_cast<uint8_t*>(&beyond),
                               sizeof(beyond), sz, mono, sseq));

  // ---- restart: new producer, same names, new session ----
  pid = fork();
  CHECK(pid >= 0);
  if (pid == 0) {
    record_mirror::RingClient mirror;
    // The parent keeps the old mapping only to observe the stale session;
    // replace the object explicitly as a known-dead-session restart.
    ShmRegion::UnlinkName(page_name);
    CHECK(mirror.Create(page_name, &desc, &spec, 1, 22));  // session 22
    PinnedSlotWriter w;
    ShmRegion::UnlinkName(chan_name);
    ShmRegion::UnlinkName(PinnedSlotWriter::LeaseShmName(chan_name).c_str());
    CHECK(w.Create(chan_name, kSlotBytes, 32, 0));
    w.SetCrcEnabled(true);
    for (uint64_t i = 0; i < 7; ++i) {
      CrashPod p{i, 0x2200 + i};
      CHECK(mirror.Page()->TryWriteRing(0, &p, sizeof(p), i * 10,
                                        static_cast<uint32_t>(i)));
      WriteFrame(w, i);
    }
    mirror.Shutdown(false);
    _exit(0);
  }
  CHECK(waitpid(pid, &st, 0) == pid);
  CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0);

  // Stale mapping still shows session 11 (re-attach is mandatory).
  CHECK(page1->header.session_id.load(std::memory_order_acquire) == 11);

  record_mirror::RingClient ro2;
  CHECK(ro2.AttachReadOnly(page_name, 1000000000ULL, hash));
  const RecordMirrorRingPage* page2 = ro2.Page();
  CHECK(page2->header.session_id.load(std::memory_order_acquire) == 22);
  CHECK(page2->RingHead(0) == 7);
  for (uint64_t i = 0; i < 7; ++i) {
    CrashPod p{};
    uint32_t z = 0;
    uint64_t m = 0;
    uint32_t q = 0;
    CHECK(page2->ReadRingSample(0, i, reinterpret_cast<uint8_t*>(&p),
                                sizeof(p), z, m, q));
    CHECK(p.index == i && p.tag == 0x2200 + i);  // never stale session data
  }
  PinnedSlotReader fr2;
  CHECK(fr2.Open(chan_name, 1000000000ULL));
  std::vector<FrameRec> fresh;
  DrainFrames(fr2, fresh);
  CHECK(fresh.size() == 7);
  for (std::size_t i = 0; i < fresh.size(); ++i) {
    CHECK(fresh[i].frame_index == i);
    CHECK(FrameRecValid(fresh[i]));
  }

  fr1.Close();
  fr2.Close();
  ro1.Shutdown(false);
  ro2.Shutdown(true);
  UnlinkShmPair(chan_name);
}

// ---------------------------------------------------------------- scenario 4

// Recorder (reader process) SIGKILLed mid-consumption: the producer never
// blocks (no backpressure), and a fresh reader re-attaches to harvest the
// retained window.
void RunKilledReader() {
  char page_name[64], chan_name[64];
  std::snprintf(page_name, sizeof(page_name), "/c9crash_kr_page_%d",
                static_cast<int>(getpid()));
  std::snprintf(chan_name, sizeof(chan_name), "/c9crash_kr_chan_%d",
                static_cast<int>(getpid()));
  ShmRegion::UnlinkName(page_name);
  UnlinkShmPair(chan_name);

  ThymeStructDesc desc{};
  desc.struct_id = 0;
  desc.contract_version = 1;
  desc.payload_size = sizeof(CrashPod);
  std::snprintf(desc.name, sizeof(desc.name), "CrashPod");
  RecordMirrorRingSpec spec{sizeof(CrashPod), 64};
  const uint64_t hash = RecordMirrorRingPage::HashConfig(&desc, &spec, 1);

  record_mirror::RingClient owner;
  CHECK(owner.Create(page_name, &desc, &spec, 1, 33));

  int ready_pipe[2];
  CHECK(pipe(ready_pipe) == 0);
  const pid_t writer = fork();
  CHECK(writer >= 0);
  if (writer == 0) {
    close(ready_pipe[0]);
    record_mirror::RingClient w;
    CHECK(w.AttachReadWrite(page_name, 1000000000ULL, hash));
    PinnedSlotWriter frames;
    CHECK(frames.Create(chan_name, kSlotBytes, 8, 0));
    frames.SetCrcEnabled(true);
    CHECK(write(ready_pipe[1], "R", 1) == 1);
    for (uint64_t i = 0; i < 200; ++i) {
      CrashPod p{i, 0x4400 + i};
      // Every write must succeed: no reader backpressure exists by design.
      CHECK(w.Page()->TryWriteRing(0, &p, sizeof(p), i * 10,
                                   static_cast<uint32_t>(i)));
      WriteFrame(frames, i);
      SleepNs(500000);
    }
    frames.Close();
    w.Shutdown(false);
    _exit(0);
  }
  close(ready_pipe[1]);
  char b = 0;
  CHECK(read(ready_pipe[0], &b, 1) == 1 && b == 'R');
  close(ready_pipe[0]);

  // Reader process: consumes, then gets SIGKILLed by the parent mid-stream.
  const pid_t reader = fork();
  CHECK(reader >= 0);
  if (reader == 0) {
    record_mirror::RingClient ro;
    CHECK(ro.AttachReadOnly(page_name, 1000000000ULL, hash));
    PinnedSlotReader fr;
    CHECK(fr.Open(chan_name, 1000000000ULL));
    std::vector<FrameRec> sink;
    RecordMirrorRingSampleDesc buf[64];
    uint64_t torn = 0;
    for (;;) {
      DrainFrames(fr, sink);
      ro.Page()->HarvestWindow(0, 0, ~0ULL, buf, 64, &torn);
      SleepNs(1000000);
    }
  }
  SleepNs(50000000);  // let the reader actually consume for ~50 ms
  CHECK(kill(reader, SIGKILL) == 0);
  int st = 0;
  CHECK(waitpid(reader, &st, 0) == reader);
  ExpectKilled(st);

  // The writer finishes the full run unaffected (bounded by the watchdog).
  CHECK(waitpid(writer, &st, 0) == writer);
  CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0);
  CHECK(owner.Page()->RingHead(0) == 200);

  // A fresh reader harvests the retained window: last 64 samples intact.
  record_mirror::RingClient ro2;
  CHECK(ro2.AttachReadOnly(page_name, 1000000000ULL, hash));
  std::vector<RecordMirrorRingSampleDesc> win(64);
  uint64_t torn = 0;
  const uint32_t n = ro2.Page()->HarvestWindow(0, 0, ~0ULL, win.data(), 64,
                                               &torn);
  CHECK(n == 64);
  CHECK(torn == 0);  // writer finished: tear-free
  for (uint32_t i = 0; i < n; ++i) {
    const uint64_t expect = 136 + i;
    CHECK(win[i].publish_index == expect);
    CrashPod p{};
    uint32_t z = 0;
    uint64_t m = 0;
    uint32_t q = 0;
    CHECK(ro2.Page()->ReadRingSample(0, expect, reinterpret_cast<uint8_t*>(&p),
                                     sizeof(p), z, m, q));
    CHECK(p.index == expect && p.tag == 0x4400 + expect);
  }

  // Channel side: fresh reader gap-skips to the last 8 slots, all intact.
  PinnedSlotReader fr2;
  CHECK(fr2.Open(chan_name, 1000000000ULL));
  std::vector<FrameRec> tail;
  DrainFrames(fr2, tail);
  CHECK(fr2.drop_count() == 192);
  CHECK(tail.size() == 8);
  for (std::size_t i = 0; i < tail.size(); ++i) {
    CHECK(tail[i].frame_index == 192 + i);
    CHECK(FrameRecValid(tail[i]));
  }

  fr2.Close();
  ro2.Shutdown(false);
  owner.Shutdown(true);
  UnlinkShmPair(chan_name);
}

}  // namespace

int main() {
  alarm(120);  // global watchdog: any hang fails the test
  const int shm_before = CountCrashShm();
  RunKilledMidBegin();
  RunMirrorKilledMidWrite();
  RunSigkillRestart();
  RunKilledReader();
  const int shm_after = CountCrashShm();
  std::fprintf(stderr, "crash-injection: /dev/shm c9crash_* before=%d after=%d\n",
               shm_before, shm_after);
  return 0;
}
