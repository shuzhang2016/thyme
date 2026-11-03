// test_triggered_recorder.cpp — TriggeredRecorder engine (P2): a forked
// producer process publishes through tap-wired primitives (ShmSpmcRing +
// LatestSlot -> RecordMirrorRingPage, plus a PinnedSlotStore); the host
// drives TriggeredRecorder through trigger -> pre/post harvest -> settle ->
// sealed package, and the package is read back and verified.
//
// Scenarios:
//   A: full flow (trigger meta incl. pre/post roll, window completeness,
//      out-of-window exclusion, monotonic timestamps, frame crc, wal seal);
//   B: window saturation (trigger with far more window than history);
//   C: post-window overrun -> hole (GAP) accounting is exact;
//   D: producer restart with new session -> engine re-attaches, next capture
//      contains only new-session data;
//   D2: admitted same-hash geometry drift -> all per-struct cursors resize,
//       continuous and triggered collection cover the attached geometry;
//   E: pending_bytes quota -> oldest-descriptor eviction, exact accounting.
//   F: preempt under idle gate + rate limit -> no busy-wait, sealed truncated.
//   G: REC-20 fixed descriptor ring -> oldest-first eviction at the
//      descriptor-count bound, FIFO order, zero enqueue/dequeue allocation.
#include "thyme/check.h"
#include "thyme/contracts/frame_header.h"
#include "thyme/record/record_mirror_ring_client.h"
#include "thyme/record/thyme_bin.h"
#include "thyme/record/triggered_recorder.h"
#include "thyme/runtime/pinned_slot_store.h"
#include "thyme/runtime/latest_slot.h"
#include "thyme/runtime/record_mirror_ring.h"
#include "thyme/runtime/shm_region.h"
#include "thyme/runtime/shm_spmc_ring.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <csignal>
#include <dirent.h>
#include <malloc.h>
#include <string>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

using namespace thyme;

namespace {

// glibc 2.33 introduced mallinfo2(); older toolchains still provide mallinfo().
std::size_t HeapInUse() {
#if defined(__GLIBC__) && \
    (__GLIBC__ > 2 || (__GLIBC__ == 2 && __GLIBC_MINOR__ >= 33))
  return mallinfo2().uordblks;
#else
  return static_cast<std::size_t>(mallinfo().uordblks);
#endif
}

struct Pod16 {
  uint64_t a;
  uint32_t b;
  uint32_t c;
};
struct Pod32 {
  uint64_t a;
  uint64_t b;
  uint8_t fill[16];
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

void WriteAll(int fd, const char* s) {
  CHECK(write(fd, s, 1) == 1);
}

uint32_t FrameLen(uint64_t f) {
  return 512 + static_cast<uint32_t>(f % 8) * 256;
}

void FillFrame(uint8_t* p, uint32_t len, uint64_t f) {
  for (uint32_t j = 0; j < len; ++j) {
    p[j] = static_cast<uint8_t>(f * 13 + j * 7);
  }
}

void RmTree(const std::string& path) {
  DIR* d = opendir(path.c_str());
  if (d == nullptr) return;
  while (dirent* e = readdir(d)) {
    if (e->d_name[0] == '.') continue;
    const std::string sub = path + "/" + e->d_name;
    if (e->d_type == DT_DIR) {
      RmTree(sub);
    } else {
      std::remove(sub.c_str());
    }
  }
  closedir(d);
  rmdir(path.c_str());
}

std::vector<ThymeBinDataEntry> ReadAll(const std::string& path) {
  ThymeBinReader rd(path.c_str());
  CHECK(rd.ok());
  std::vector<ThymeBinDataEntry> out;
  ThymeBinDataEntry e;
  while (rd.ReadNext(e) == thyme::ThymeBinReader::ReadResult::kOk) out.push_back(e);
  return out;
}

ThymeStructDesc MakeDesc(uint16_t sid, uint32_t payload, const char* name) {
  ThymeStructDesc d{};
  d.struct_id = sid;
  d.contract_version = 1;
  d.payload_size = payload;
  std::snprintf(d.name, sizeof(d.name), "%s", name);
  return d;
}

void MakeRingSpecs(ThymeStructDesc* descs, RecordMirrorRingSpec* specs,
                   uint32_t depth) {
  descs[0] = MakeDesc(0, sizeof(Pod16), "Pod16");
  descs[1] = MakeDesc(1, sizeof(Pod32), "Pod32");
  specs[0] = {sizeof(Pod16), depth};
  specs[1] = {sizeof(Pod32), depth};
}

// Full producer: mirror page (sid0 via SPMC tap, sid1 via LatestSlot tap) +
// PinnedSlotStore; `count` paced iterations, then exits leaving shm behind.
void ProducerFull(const char* page_name, const char* ring_name,
                  const char* chan_name, uint32_t session, uint64_t tag_base,
                  uint32_t count, uint64_t pace_ns, bool with_frames,
                  int ready_fd) {
  ThymeStructDesc descs[2];
  RecordMirrorRingSpec specs[2];
  MakeRingSpecs(descs, specs, 256);
  record_mirror::RingClient mirror;
  // The harness intentionally replaces a known-dead producer while an old
  // reader mapping is still retained for session-change observation.
  ShmRegion::UnlinkName(page_name);
  CHECK(mirror.Create(page_name, descs, specs, 2, session));
  ShmSpmcRing<Pod16, 16> ring;
  CHECK(ring.Create(ring_name));
  ring.SetRecordTap(mirror.Page(), 0);
  LatestSlot<Pod32> latest;
  latest.SetRecordTap(mirror.Page(), 1);
  PinnedSlotWriter frames;
  if (with_frames) {
    CHECK(frames.Create(chan_name, 4096, 128, 0));  // max FrameLen = 2304
  }
  WriteAll(ready_fd, "R");
  for (uint64_t i = 0; i < count; ++i) {
    Pod16 p{tag_base + i, static_cast<uint32_t>((tag_base + i) * 2),
            static_cast<uint32_t>((tag_base + i) * 3)};
    ring.Push(p);
    Pod32 q{};
    q.a = tag_base + i;
    q.b = tag_base + i;
    latest.Write(q);
    if (with_frames) {
      const uint32_t len = FrameLen(i);
      PinnedSlotWriter::BeginView bv = frames.Begin(len);
      CHECK(bv.payload != nullptr);
      FillFrame(bv.payload, len, i);
      bv.header->flags.store(i % 16 == 0 ? 1u : 0u, std::memory_order_relaxed);
      frames.Commit();
    }
    SleepNs(pace_ns);
  }
  if (with_frames) frames.Close();
  ring.Close();
  mirror.Shutdown(false);
  _exit(0);
}

// Drive the engine until it returns to Armed (one capture complete).
void RunCapture(triggered::TriggeredRecorder& rec) {
  const uint64_t deadline = NowNs() + 15000000000ULL;
  while (rec.phase() != triggered::Phase::kArmed) {
    rec.Tick();
    CHECK(NowNs() < deadline);
    SleepNs(50000);
  }
}

void TickUntil(triggered::TriggeredRecorder& rec, bool (*pred)(void*),
               void* arg, uint64_t budget_ns) {
  const uint64_t deadline = NowNs() + budget_ns;
  while (!pred(arg)) {
    rec.Tick();
    CHECK(NowNs() < deadline);
    SleepNs(100000);
  }
}

struct HeadGe {
  record_mirror::RingClient* client;
  uint16_t sid;
  uint64_t target;
};
bool HeadGePred(void* p) {
  auto* a = static_cast<HeadGe*>(p);
  return a->client->Page()->RingHead(a->sid) >= a->target;
}

// ---------------------------------------------------------------- scenario A

void RunFullFlow(const char* page_name, const char* ring_name,
                 const char* chan_name, const char* out_dir) {
  ThymeStructDesc descs[2];
  RecordMirrorRingSpec specs[2];
  MakeRingSpecs(descs, specs, 256);
  const uint64_t hash = RecordMirrorRingPage::HashConfig(descs, specs, 2);

  int ready_pipe[2];
  CHECK(pipe(ready_pipe) == 0);
  const pid_t pid = fork();
  CHECK(pid >= 0);
  if (pid == 0) {
    close(ready_pipe[0]);
    ProducerFull(page_name, ring_name, chan_name, 42, 0, 70, 1000000, true,
                 ready_pipe[1]);
  }
  close(ready_pipe[1]);
  char b = 0;
  CHECK(read(ready_pipe[0], &b, 1) == 1 && b == 'R');
  close(ready_pipe[0]);

  triggered::Config cfg;
  cfg.sources = {{triggered::SourceConfig::Kind::kMirrorRing, page_name, ""},
                 {triggered::SourceConfig::Kind::kPinnedSlot, chan_name,
                  "h264_front"}};
  cfg.out_dir = out_dir;
  cfg.reattach_scan_ns = 1000000;
  // Cover scheduling stalls between the tap's MonoNs stamp and the ring-head
  // store: samples stamped just before the post deadline may land up to a
  // thread-preemption later; 50 ms grace makes the post sweep deterministic.
  cfg.post_grace_ns = 50000000ULL;
  triggered::TriggeredRecorder rec;
  CHECK(rec.Init(cfg));

  // Observer client for trigger timing only.
  record_mirror::RingClient obs;
  CHECK(obs.AttachReadOnly(page_name, 2000000000ULL, hash));
  HeadGe hge{&obs, 0, 25};
  TickUntil(rec, HeadGePred, &hge, 10000000000ULL);

  constexpr uint64_t kBefore = 30000000ULL;
  constexpr uint64_t kAfter = 15000000ULL;
  CHECK(rec.Trigger(1, kBefore, kAfter));  // t0 = now
  CHECK(rec.phase() == triggered::Phase::kPost);
  RunCapture(rec);
  CHECK(rec.stats().captures_done == 1);

  int st = 0;
  CHECK(waitpid(pid, &st, 0) == pid);
  CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0);

  // ---- verify the package ----
  const std::string dir = rec.last_package_dir();
  CHECK(triggered::TriggeredRecorder::PackageComplete(dir));

  const std::vector<ThymeBinDataEntry> meta = ReadAll(dir + "/meta.tbin");
  CHECK(meta.size() == 4);  // trigger + window + PayProf + stats
  CHECK(meta[0].type == ThymeBinEntryType::kTrigger);
  CHECK(meta[0].record_sequence == 1);  // trigger_id
  CHECK(meta[0].pre_roll_ns == kBefore);   // exposed by the API-gap fix
  CHECK(meta[0].post_roll_ns == kAfter);
  const uint64_t t0 = meta[0].mono_ns;
  CHECK(meta[1].struct_id == triggered::kMetaStructId);
  triggered::TriggerMetaRecord tm{};
  std::memcpy(&tm, meta[1].payload.data(), sizeof(tm));
  CHECK(tm.t0 == t0 && tm.before_ns == kBefore && tm.after_ns == kAfter);
  CHECK(tm.config_hash == hash);
  CHECK(meta[2].struct_id == triggered::kPayloadProfileStructId);
  CHECK(meta[3].struct_id == triggered::kMetaStructId);
  triggered::CaptureStatsRecord csr{};
  std::memcpy(&csr, meta[3].payload.data(), sizeof(csr));
  CHECK(csr.holes_written == 0);
  CHECK(csr.pending_evicted == 0);
  CHECK(csr.descriptors == csr.samples_written + csr.frames_written);

  // small.tbin: window membership, per-struct monotonic order, content.
  uint64_t n0 = 0, n1 = 0;
  uint64_t last0 = 0, last1 = 0;
  for (const auto& e : ReadAll(dir + "/small.tbin")) {
    CHECK(e.type == ThymeBinEntryType::kData);
    CHECK(e.mono_ns >= t0 - kBefore && e.mono_ns <= t0 + kAfter);
    if (e.struct_id == 0) {
      Pod16 p{};
      std::memcpy(&p, e.payload.data(), sizeof(p));
      CHECK(p.a * 2 == p.b && p.a * 3 == p.c);
      CHECK(n0 == 0 || e.mono_ns > last0);
      last0 = e.mono_ns;
      ++n0;
    } else {
      CHECK(e.struct_id == 1);
      Pod32 q{};
      std::memcpy(&q, e.payload.data(), sizeof(q));
      CHECK(q.a == q.b);
      CHECK(n1 == 0 || e.mono_ns > last1);
      last1 = e.mono_ns;
      ++n1;
    }
  }

  CHECK(n0 + n1 == csr.samples_written);

  // Independent count from the (stopped) producer's page. The engine
  // harvested while the producer was live, so a sample being written during
  // a harvest sweep may be skipped after the retry budget (torn): package
  // count <= independent count, and every miss is accounted by the capture's
  // tear counter (holes are separately zero — nothing is evicted here).
  {
    RecordMirrorRingSampleDesc buf[256];
    uint64_t torn = 0;
    const uint32_t indep0 =
        obs.Page()->HarvestWindow(0, t0 - kBefore, t0 + kAfter, buf, 256,
                                  &torn);
    const uint32_t indep1 =
        obs.Page()->HarvestWindow(1, t0 - kBefore, t0 + kAfter, buf, 256,
                                  &torn);
    CHECK(torn == 0);  // writer stopped: the independent read is tear-free
    CHECK(n0 <= indep0 && n1 <= indep1);
    CHECK(indep0 - n0 + (indep1 - n1) <= csr.torn_reads);
  }
  CHECK(n0 >= 15 && n1 >= 15);  // ~45 each expected; scheduling margin

  // frames: index entries + raw segments; crc-equivalent pattern check.
  const std::vector<ThymeBinDataEntry> idx =
      ReadAll(dir + "/frames/h264_front.index.tbin");
  CHECK(idx.size() == csr.frames_written);
  CHECK(idx.size() >= 20);
  FILE* raw = std::fopen((dir + "/frames/h264_front.raw").c_str(), "rb");
  CHECK(raw != nullptr);
  // REC-26/27: the .raw file starts with a 128 B channel header; index
  // offsets are absolute and include it.
  uint64_t expect_off = 128;
  uint64_t last_mono = 0;
  for (std::size_t i = 0; i < idx.size(); ++i) {
    const ThymeBinDataEntry& e = idx[i];
    CHECK(e.type == ThymeBinEntryType::kData);
    CHECK(e.struct_id == triggered::kFrameIndexStructId);
    CHECK(e.mono_ns >= t0 - kBefore && e.mono_ns <= t0 + kAfter);
    CHECK(i == 0 || e.mono_ns > last_mono);
    last_mono = e.mono_ns;
    triggered::FrameIndexRecord ir{};
    std::memcpy(&ir, e.payload.data(), sizeof(ir));
    const uint64_t f = e.source_sequence;
    CHECK(ir.offset == expect_off);
    CHECK(ir.len == FrameLen(f));
    CHECK(ir.flags == (f % 16 == 0 ? 1u : 0u));
    std::vector<uint8_t> seg(ir.len);
    CHECK(fseeko(raw, static_cast<off_t>(ir.offset), SEEK_SET) == 0);
    CHECK(std::fread(seg.data(), 1, ir.len, raw) == ir.len);
    for (uint32_t j = 0; j < ir.len; ++j) {
      CHECK(seg[j] == static_cast<uint8_t>(f * 13 + j * 7));
    }
    CHECK(Crc32(seg.data(), ir.len) != 0);
    expect_off += ir.len;
  }
  CHECK(std::fclose(raw) == 0);

  // Independent frame count: drain the channel and filter by the window.
  {
    PinnedSlotReader r;
    CHECK(r.Open(chan_name, 1000000000ULL));
    CHECK(r.head() == 70);
    uint64_t in_window = 0;
    uint64_t outside = 0;
    PinnedSlotReader::View v;
    while (r.TryRead(v)) {
      CHECK(v.crc_ok);
      if (v.mono_ns >= t0 - kBefore && v.mono_ns <= t0 + kAfter) {
        ++in_window;
      } else {
        ++outside;
      }
    }
    CHECK(in_window == idx.size());  // package contains exactly the window
    CHECK(outside > 0);              // ...so exclusion was meaningful
    r.Close();
  }
  CHECK(rec.stats().holes_written == 0);
  rec.Shutdown();
}

// ---------------------------------------------------------------- scenario B

void RunSaturated(const char* page_name, const char* ring_name,
                  const char* out_dir) {
  int ready_pipe[2];
  CHECK(pipe(ready_pipe) == 0);
  const pid_t pid = fork();
  CHECK(pid >= 0);
  if (pid == 0) {
    close(ready_pipe[0]);
    ProducerFull(page_name, ring_name, "/unused", 7, 0, 5, 1000000, false,
                 ready_pipe[1]);
  }
  close(ready_pipe[1]);
  char b = 0;
  CHECK(read(ready_pipe[0], &b, 1) == 1 && b == 'R');
  close(ready_pipe[0]);
  int st = 0;
  CHECK(waitpid(pid, &st, 0) == pid);
  CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0);

  triggered::Config cfg;
  cfg.sources = {{triggered::SourceConfig::Kind::kMirrorRing, page_name, ""}};
  cfg.out_dir = out_dir;
  triggered::TriggeredRecorder rec;
  CHECK(rec.Init(cfg));
  // Window (55 ms) far exceeds the recorded history (5 ms): saturate.
  CHECK(rec.Trigger(2, 50000000ULL, 5000000ULL));
  RunCapture(rec);
  CHECK(triggered::TriggeredRecorder::PackageComplete(
      rec.last_package_dir()));
  const std::vector<ThymeBinDataEntry> small = ReadAll(
      rec.last_package_dir() + "/small.tbin");
  CHECK(small.size() == 10);  // exactly 5+5 real samples, no phantoms
  uint64_t tags = 0;
  for (const auto& e : small) {
    if (e.struct_id == 0) {
      Pod16 p{};
      std::memcpy(&p, e.payload.data(), sizeof(p));
      CHECK(p.a < 5);
      ++tags;
    }
  }
  CHECK(tags == 5);
  rec.Shutdown();
}

// ---------------------------------------------------------------- scenario C

void RunPostOverrun(const char* page_name, const char* ring_name,
                    const char* out_dir) {
  int ready_pipe[2], go_pipe[2];
  CHECK(pipe(ready_pipe) == 0 && pipe(go_pipe) == 0);
  const pid_t pid = fork();
  CHECK(pid >= 0);
  if (pid == 0) {
    close(ready_pipe[0]);
    close(go_pipe[1]);
    ThymeStructDesc d = MakeDesc(0, sizeof(Pod16), "Pod16");
    RecordMirrorRingSpec spec{sizeof(Pod16), 4};  // shallow ring
    record_mirror::RingClient mirror;
    CHECK(mirror.Create(page_name, &d, &spec, 1, 9));
    ShmSpmcRing<Pod16, 16> ring;
    CHECK(ring.Create(ring_name));
    ring.SetRecordTap(mirror.Page(), 0);
    WriteAll(ready_pipe[1], "R");
    char g = 0;
    CHECK(read(go_pipe[0], &g, 1) == 1 && g == 'G');
    for (uint64_t i = 0; i < 100; ++i) {  // paced: engine queues these
      Pod16 p{i, static_cast<uint32_t>(i * 2), static_cast<uint32_t>(i * 3)};
      ring.Push(p);
      SleepNs(200000);
    }
    for (uint64_t i = 100; i < 300; ++i) {  // blast: overwrites the queue
      Pod16 p{i, static_cast<uint32_t>(i * 2), static_cast<uint32_t>(i * 3)};
      ring.Push(p);
    }
    ring.Close();
    mirror.Shutdown(false);
    _exit(0);
  }
  close(ready_pipe[1]);
  close(go_pipe[0]);
  char b = 0;
  CHECK(read(ready_pipe[0], &b, 1) == 1 && b == 'R');

  triggered::Config cfg;
  cfg.sources = {{triggered::SourceConfig::Kind::kMirrorRing, page_name, ""}};
  cfg.out_dir = out_dir;
  triggered::TriggeredRecorder rec;
  CHECK(rec.Init(cfg));

  WriteAll(go_pipe[1], "G");
  close(go_pipe[1]);
  // post window covers the paced phase (20 ms) plus blast (~0 ms): 60 ms.
  CHECK(rec.Trigger(3, 0, 60000000ULL));
  RunCapture(rec);
  CHECK(triggered::TriggeredRecorder::PackageComplete(rec.last_package_dir()));

  int st = 0;
  CHECK(waitpid(pid, &st, 0) == pid);
  CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0);

  // Exact accounting: every queued descriptor became a sample or a GAP.
  const std::vector<ThymeBinDataEntry> meta = ReadAll(
      rec.last_package_dir() + "/meta.tbin");
  triggered::CaptureStatsRecord csr{};
  bool got_csr = false;
  for (const auto& e : meta) {
    if (e.struct_id == triggered::kMetaStructId &&
        e.source_sequence == 1) {
      std::memcpy(&csr, e.payload.data(), sizeof(csr));
      got_csr = true;
    }
  }
  CHECK(got_csr);
  CHECK(csr.descriptors >= 50);  // most of the paced phase was queued
  CHECK(csr.samples_written <= 4);  // depth-4 ring: at most 4 survive
  CHECK(csr.holes_written >= 40);
  CHECK(csr.holes_written + csr.samples_written == csr.descriptors);
  CHECK(rec.stats().overrun_before_queue >= 100);  // blast-era peek failures

  uint64_t gaps = 0, datas = 0;
  uint64_t last_mono = 0;
  for (const auto& e : ReadAll(rec.last_package_dir() + "/small.tbin")) {
    CHECK(e.mono_ns > last_mono || (gaps + datas) == 0);
    last_mono = e.mono_ns;
    if (e.type == ThymeBinEntryType::kGap) {
      CHECK(e.reserved == triggered::kGapReasonOverrun);
      ++gaps;
    } else {
      ++datas;
    }
  }
  CHECK(gaps == csr.holes_written && datas == csr.samples_written);
  rec.Shutdown();
}

// ---------------------------------------------------------------- scenario D

void RunSessionReattach(const char* page_name, const char* ring_name,
                        const char* out_dir) {
  int ready_pipe[2];
  CHECK(pipe(ready_pipe) == 0);
  pid_t pid = fork();
  CHECK(pid >= 0);
  if (pid == 0) {
    close(ready_pipe[0]);
    ProducerFull(page_name, ring_name, "/unused", 11, 0x1100, 20, 500000,
                 false, ready_pipe[1]);
  }
  close(ready_pipe[1]);
  char b = 0;
  CHECK(read(ready_pipe[0], &b, 1) == 1 && b == 'R');
  close(ready_pipe[0]);
  int st = 0;
  CHECK(waitpid(pid, &st, 0) == pid);
  CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0);

  triggered::Config cfg;
  cfg.sources = {{triggered::SourceConfig::Kind::kMirrorRing, page_name, ""}};
  cfg.out_dir = out_dir;
  cfg.reattach_scan_ns = 1000000;  // 1 ms scan for the test
  triggered::TriggeredRecorder rec;
  CHECK(rec.Init(cfg));
  CHECK(rec.Trigger(4, 100000000ULL, 0));
  RunCapture(rec);
  CHECK(triggered::TriggeredRecorder::PackageComplete(rec.last_package_dir()));
  for (const auto& e : ReadAll(rec.last_package_dir() + "/small.tbin")) {
    CHECK(e.type == ThymeBinEntryType::kData);
    if (e.struct_id != 0) continue;
    Pod16 p{};
    std::memcpy(&p, e.payload.data(), sizeof(p));
    CHECK(p.a >= 0x1100 && p.a < 0x1100 + 20);
  }

  // Producer restarts: same shm names, NEW session, new data tags. The SPMC
  // ring's Create is O_EXCL, so the stale ring object is dropped here (the
  // mirror page is unlinked+recreated inside RingClient::Create, which is
  // exactly the identity change the engine must detect).
  ShmRegion::UnlinkName(ring_name);
  CHECK(pipe(ready_pipe) == 0);
  pid = fork();
  CHECK(pid >= 0);
  if (pid == 0) {
    close(ready_pipe[0]);
    ProducerFull(page_name, ring_name, "/unused", 22, 0x2200, 15, 500000,
                 false, ready_pipe[1]);
  }
  close(ready_pipe[1]);
  CHECK(read(ready_pipe[0], &b, 1) == 1 && b == 'R');
  close(ready_pipe[0]);
  CHECK(waitpid(pid, &st, 0) == pid);
  CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0);

  // Engine must detect the shm replacement and re-attach.
  struct ReattachPred {
    static bool Fired(void* p) {
      return static_cast<triggered::TriggeredRecorder*>(p)
                 ->stats()
                 .session_reattaches >= 1;
    }
  };
  TickUntil(rec, ReattachPred::Fired, &rec, 5000000000ULL);
  CHECK(rec.stats().source_errors == 0);

  CHECK(rec.Trigger(5, 100000000ULL, 0));
  RunCapture(rec);
  CHECK(triggered::TriggeredRecorder::PackageComplete(rec.last_package_dir()));
  uint64_t n = 0;
  for (const auto& e : ReadAll(rec.last_package_dir() + "/small.tbin")) {
    CHECK(e.type == ThymeBinEntryType::kData);
    if (e.struct_id != 0) continue;
    Pod16 p{};
    std::memcpy(&p, e.payload.data(), sizeof(p));
    CHECK(p.a >= 0x2200 && p.a < 0x2200 + 15);  // new session only
    ++n;
  }
  CHECK(n == 15);
  CHECK(rec.stats().captures_done == 2);
  rec.Shutdown();
}

// ------------------------------------------------------------- scenario D2

void RunContinuousReattachGeometryDefense(const char* page_name,
                                          const char* out_dir) {
  ThymeStructDesc descs[2];
  RecordMirrorRingSpec specs[2];
  MakeRingSpecs(descs, specs, 32);
  record_mirror::RingClient mirror;
  CHECK(mirror.Create(page_name, descs, specs, 1, 31));
  const uint64_t pinned_hash = mirror.Page()->config_hash();

  triggered::Config cfg;
  cfg.sources = {{triggered::SourceConfig::Kind::kMirrorRing, page_name, ""}};
  cfg.out_dir = out_dir;
  cfg.reattach_scan_ns = 1000000;
  triggered::TriggeredRecorder rec;
  CHECK(rec.Init(cfg));
  triggered::ContinuousConfig ccfg;
  ccfg.slice_bytes = 1000000;
  CHECK(rec.ArmContinuous(ccfg));

  mirror.Shutdown(true);
  CHECK(mirror.Create(page_name, descs, specs, 2, 32));
  // Simulate inconsistent metadata (or a hash collision): the normal pin
  // admits the page although its struct geometry grew.
  mirror.Page()->header.config_hash.store(pinned_hash,
                                          std::memory_order_relaxed);
  struct ReattachPred {
    static bool Fired(void* p) {
      return static_cast<triggered::TriggeredRecorder*>(p)
                 ->stats()
                 .session_reattaches >= 1;
    }
  };
  TickUntil(rec, ReattachPred::Fired, &rec, 5000000000ULL);
  CHECK(rec.stats().source_errors == 0);

  const uint64_t mono = NowNs();
  const Pod16 p{0x3300, 0x6600, 0x9900};
  Pod32 q{};
  q.a = 0x4400;
  q.b = 0x4400;
  CHECK(mirror.Page()->TryWriteRing(0, &p, sizeof(p), mono + 1, 1));
  CHECK(mirror.Page()->TryWriteRing(1, &q, sizeof(q), mono + 2, 2));
  rec.Tick();
  rec.DisarmContinuous();
  CHECK(rec.continuous_stats().samples_written == 2);
  CHECK(rec.continuous_stats().holes_written == 0);
  const std::vector<ThymeBinDataEntry> continuous =
      ReadAll(rec.current_slice_dir() + "/small.tbin");
  CHECK(continuous.size() == 2);
  CHECK(continuous[0].struct_id == 0);
  CHECK(continuous[1].struct_id == 1);

  // The triggered cursor has the same per-struct shape and must cover both.
  CHECK(rec.Trigger(23, 1000000000ULL, 0));
  RunCapture(rec);
  const std::vector<ThymeBinDataEntry> triggered =
      ReadAll(rec.last_package_dir() + "/small.tbin");
  CHECK(triggered.size() == 2);
  CHECK(triggered[0].struct_id == 0);
  CHECK(triggered[1].struct_id == 1);
  rec.Shutdown();
  mirror.Shutdown(true);
}

// ---------------------------------------------------------------- scenario E

void RunQuotaEviction(const char* page_name, const char* ring_name,
                      const char* out_dir) {
  int ready_pipe[2];
  CHECK(pipe(ready_pipe) == 0);
  const pid_t pid = fork();
  CHECK(pid >= 0);
  if (pid == 0) {
    close(ready_pipe[0]);
    ProducerFull(page_name, ring_name, "/unused", 8, 0, 60, 100000, false,
                 ready_pipe[1]);
  }
  close(ready_pipe[1]);
  char b = 0;
  CHECK(read(ready_pipe[0], &b, 1) == 1 && b == 'R');
  close(ready_pipe[0]);
  int st = 0;
  CHECK(waitpid(pid, &st, 0) == pid);
  CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0);

  triggered::Config cfg;
  cfg.sources = {{triggered::SourceConfig::Kind::kMirrorRing, page_name, ""}};
  cfg.out_dir = out_dir;
  cfg.pending_bytes_quota = 200;  // 12 Pod16 descriptors; 60+60 are queued
  triggered::TriggeredRecorder rec;
  CHECK(rec.Init(cfg));
  CHECK(rec.Trigger(6, 1000000000ULL, 0));  // 1 s window covers all 60 pushes
  RunCapture(rec);
  CHECK(triggered::TriggeredRecorder::PackageComplete(rec.last_package_dir()));
  CHECK(rec.stats().pending_evicted > 0);

  const std::vector<ThymeBinDataEntry> meta = ReadAll(
      rec.last_package_dir() + "/meta.tbin");
  triggered::CaptureStatsRecord csr{};
  for (const auto& e : meta) {
    if (e.struct_id == triggered::kMetaStructId && e.source_sequence == 1) {
      std::memcpy(&csr, e.payload.data(), sizeof(csr));
    }
  }
  CHECK(csr.pending_evicted == rec.stats().pending_evicted);
  // Exact accounting: queued = written + evicted (producer stopped: no holes).
  CHECK(csr.holes_written == 0);
  CHECK(csr.descriptors == csr.samples_written + csr.pending_evicted);
  CHECK(csr.descriptors == 120);  // 60 Pod16 + 60 Pod32... quota trims by size
  uint64_t n0 = 0, n1 = 0;
  for (const auto& e : ReadAll(rec.last_package_dir() + "/small.tbin")) {
    if (e.struct_id == 0) ++n0;
    if (e.struct_id == 1) ++n1;
  }
  CHECK(n0 + n1 == csr.samples_written);
  rec.Shutdown();
}

// ---------------------------------------------------------------- scenario F

// Preempt under defer_settle + io_rate_limit: the drain must not busy-spin on
// an empty token bucket — Preempt lifts the rate limit and finishes within a
// wall-clock bound, with the package sealed complete and marked truncated.
void RunPreemptNoBusyWait(const char* page_name, const char* out_dir) {
  ThymeStructDesc desc = MakeDesc(0, 256, "Big256");
  RecordMirrorRingSpec spec{256, 4096};
  record_mirror::RingClient mirror;
  CHECK(mirror.Create(page_name, &desc, &spec, 1, 1));
  std::vector<uint8_t> buf(256, 0x5A);
  for (uint64_t i = 0; i < 1000; ++i) {
    CHECK(mirror.Page()->TryWriteRing(0, buf.data(), 256, NowNs(),
                                      static_cast<uint32_t>(i)));
  }

  triggered::Config ecfg;
  ecfg.sources = {{triggered::SourceConfig::Kind::kMirrorRing, page_name, ""}};
  ecfg.out_dir = out_dir;
  ecfg.io_rate_limit_bps = 1000;          // 1 KB/s: unlimited wait = minutes
  ecfg.io_burst_bytes = 4096;
  triggered::TriggeredRecorder rec;
  CHECK(rec.Init(ecfg));
  CHECK(rec.Trigger(9, 60000000000ULL, 0, 0, /*defer_settle=*/true));
  // Post window (0) elapses; settle stays parked behind the idle gate.
  {
    const uint64_t deadline = NowNs() + 10000000000ULL;
    while (rec.phase() != triggered::Phase::kSettle) {
      rec.Tick();
      CHECK(NowNs() < deadline);
      SleepNs(100000);
    }
  }
  const uint64_t t0 = NowNs();
  CHECK(rec.Preempt());  // ~256 KB queued; must not wait for the bucket
  const uint64_t elapsed = NowNs() - t0;
  CHECK(elapsed < 2000000000ULL);  // un-limited drain: well under 2 s
  CHECK(rec.phase() == triggered::Phase::kArmed);
  CHECK(triggered::TriggeredRecorder::PackageComplete(
      rec.last_package_dir()));
  triggered::CaptureStatsRecord csr{};
  for (const auto& e : ReadAll(rec.last_package_dir() + "/meta.tbin")) {
    if (e.struct_id == triggered::kMetaStructId && e.source_sequence == 1) {
      std::memcpy(&csr, e.payload.data(), sizeof(csr));
    }
  }
  CHECK((csr.flags & triggered::kCaptureFlagTruncated) != 0);
  CHECK(csr.samples_written == 1000);
  CHECK(csr.holes_written == 0);
  mirror.Shutdown(true);
  rec.Shutdown();
}

// ---------------------------------------------------------------- scenario G

// REC-20: the descriptor queues are fixed-capacity rings sized once in Init().
// Capacity comes from pending_bytes_quota (one descriptor per KiB, clamped to
// [256, 65536]), so a 256 KiB quota yields a 256-descriptor ring. Harvesting
// 1000 retained samples must keep the NEWEST 256 in FIFO order, evict the
// oldest 744 through pending_evicted, and enqueue/dequeue without heap growth.
void RunRingCapacity(const char* page_name, const char* out_dir) {
  CHECK(triggered::TriggeredRecorder::DescriptorRingCapacity(256 * 1024) == 256);
  CHECK(triggered::TriggeredRecorder::DescriptorRingCapacity(0) == 256);
  CHECK(triggered::TriggeredRecorder::DescriptorRingCapacity(
            64ull * 1024 * 1024) == 65536);

  ThymeStructDesc desc = MakeDesc(0, 256, "Big256");
  RecordMirrorRingSpec spec{256, 4096};
  record_mirror::RingClient mirror;
  CHECK(mirror.Create(page_name, &desc, &spec, 1, 1));
  std::vector<uint8_t> buf(256, 0);
  for (uint64_t i = 0; i < 1000; ++i) {
    std::memcpy(buf.data(), &i, sizeof(i));  // payload carries its index
    CHECK(mirror.Page()->TryWriteRing(0, buf.data(), 256, NowNs() + i,
                                      static_cast<uint32_t>(i)));
  }

  triggered::Config cfg;
  cfg.sources = {{triggered::SourceConfig::Kind::kMirrorRing, page_name, ""}};
  cfg.out_dir = out_dir;
  // 256 KiB of payload fits (1000 * 256 B); the ring bound binds first.
  cfg.pending_bytes_quota = 256 * 1024;
  triggered::TriggeredRecorder rec;
  CHECK(rec.Init(cfg));
  CHECK(rec.Trigger(21, 60000000000ULL, 0));
  RunCapture(rec);
  CHECK(triggered::TriggeredRecorder::PackageComplete(rec.last_package_dir()));
  CHECK(rec.stats().pending_evicted == 744);

  const std::string dir = rec.last_package_dir();
  triggered::CaptureStatsRecord csr{};
  for (const auto& e : ReadAll(dir + "/meta.tbin")) {
    if (e.struct_id == triggered::kMetaStructId && e.source_sequence == 1) {
      std::memcpy(&csr, e.payload.data(), sizeof(csr));
    }
  }
  CHECK(csr.descriptors == 1000);
  CHECK(csr.pending_evicted == 744);
  CHECK(csr.samples_written == 256);
  CHECK(csr.holes_written == 0);
  // Newest-256 FIFO order: the drained package is exactly indices 744..999.
  uint64_t expect = 744;
  for (const auto& e : ReadAll(dir + "/small.tbin")) {
    CHECK(e.struct_id == 0);
    uint64_t idx = 0;
    CHECK(e.payload.size() >= sizeof(idx));
    std::memcpy(&idx, e.payload.data(), sizeof(idx));
    CHECK(idx == expect);
    ++expect;
  }
  CHECK(expect == 1000);

  // Enqueue/dequeue are index updates on the preallocated ring: 10k pushes at
  // the 256-entry bound (9744 oldest-first evictions) plus a full drain must
  // not move the heap watermark.
  const std::size_t before = HeapInUse();
  for (int i = 0; i < 10000; ++i) {
    rec.PushContinuous(0, false, 0, static_cast<uint64_t>(i), 0, 0, 0);
  }
  CHECK(rec.continuous_stats().queue_evicted == 9744);
  CHECK(rec.pending_bytes() == 0);  // zero-length descriptors
  const uint64_t freed = rec.EvictContinuous(~0ull);
  const std::size_t after = HeapInUse();
  CHECK(freed == 0);
  CHECK(rec.continuous_stats().queue_evicted == 10000);
  CHECK(after == before);
  mirror.Shutdown(true);
  rec.Shutdown();
}

}  // namespace

namespace {
void UnlinkShmPair(const char* n) {
  ShmRegion::UnlinkName(n);
  const std::string lease = std::string(n) + ".lease";
  ShmRegion::UnlinkName(lease.c_str());
}
}  // namespace

int main() {
  alarm(120);
  const int me = static_cast<int>(getpid());
  char page_name[64], ring_name[64], chan_name[64], out_dir[96];

  std::snprintf(page_name, sizeof(page_name), "/c9trec_a_page_%d", me);
  std::snprintf(ring_name, sizeof(ring_name), "/c9trec_a_ring_%d", me);
  std::snprintf(chan_name, sizeof(chan_name), "/c9trec_a_h264_%d", me);
  std::snprintf(out_dir, sizeof(out_dir), "/tmp/c9trec_a_%d", me);
  ShmRegion::UnlinkName(page_name);
  ShmRegion::UnlinkName(ring_name);
  ShmRegion::UnlinkName(chan_name);
  RmTree(out_dir);
  RunFullFlow(page_name, ring_name, chan_name, out_dir);
  RmTree(out_dir);
  UnlinkShmPair(chan_name);
  UnlinkShmPair(ring_name);
  UnlinkShmPair(page_name);

  std::snprintf(page_name, sizeof(page_name), "/c9trec_b_page_%d", me);
  std::snprintf(ring_name, sizeof(ring_name), "/c9trec_b_ring_%d", me);
  std::snprintf(out_dir, sizeof(out_dir), "/tmp/c9trec_b_%d", me);
  ShmRegion::UnlinkName(page_name);
  ShmRegion::UnlinkName(ring_name);
  RmTree(out_dir);
  RunSaturated(page_name, ring_name, out_dir);
  RmTree(out_dir);
  UnlinkShmPair(ring_name);
  UnlinkShmPair(page_name);

  std::snprintf(page_name, sizeof(page_name), "/c9trec_c_page_%d", me);
  std::snprintf(ring_name, sizeof(ring_name), "/c9trec_c_ring_%d", me);
  std::snprintf(out_dir, sizeof(out_dir), "/tmp/c9trec_c_%d", me);
  ShmRegion::UnlinkName(page_name);
  ShmRegion::UnlinkName(ring_name);
  RmTree(out_dir);
  RunPostOverrun(page_name, ring_name, out_dir);
  RmTree(out_dir);
  UnlinkShmPair(ring_name);
  UnlinkShmPair(page_name);

  std::snprintf(page_name, sizeof(page_name), "/c9trec_d_page_%d", me);
  std::snprintf(ring_name, sizeof(ring_name), "/c9trec_d_ring_%d", me);
  std::snprintf(out_dir, sizeof(out_dir), "/tmp/c9trec_d_%d", me);
  ShmRegion::UnlinkName(page_name);
  ShmRegion::UnlinkName(ring_name);
  RmTree(out_dir);
  RunSessionReattach(page_name, ring_name, out_dir);
  RmTree(out_dir);
  UnlinkShmPair(ring_name);
  UnlinkShmPair(page_name);

  std::snprintf(page_name, sizeof(page_name), "/c9trec_d2_page_%d", me);
  std::snprintf(out_dir, sizeof(out_dir), "/tmp/c9trec_d2_%d", me);
  ShmRegion::UnlinkName(page_name);
  RmTree(out_dir);
  RunContinuousReattachGeometryDefense(page_name, out_dir);
  RmTree(out_dir);
  UnlinkShmPair(page_name);

  std::snprintf(page_name, sizeof(page_name), "/c9trec_e_page_%d", me);
  std::snprintf(ring_name, sizeof(ring_name), "/c9trec_e_ring_%d", me);
  std::snprintf(out_dir, sizeof(out_dir), "/tmp/c9trec_e_%d", me);
  ShmRegion::UnlinkName(page_name);
  ShmRegion::UnlinkName(ring_name);
  RmTree(out_dir);
  RunQuotaEviction(page_name, ring_name, out_dir);
  RmTree(out_dir);
  UnlinkShmPair(ring_name);
  UnlinkShmPair(page_name);

  std::snprintf(page_name, sizeof(page_name), "/c9trec_f_page_%d", me);
  std::snprintf(out_dir, sizeof(out_dir), "/tmp/c9trec_f_%d", me);
  ShmRegion::UnlinkName(page_name);
  RmTree(out_dir);
  RunPreemptNoBusyWait(page_name, out_dir);
  RmTree(out_dir);
  UnlinkShmPair(page_name);

  std::snprintf(page_name, sizeof(page_name), "/c9trec_g_page_%d", me);
  std::snprintf(out_dir, sizeof(out_dir), "/tmp/c9trec_g_%d", me);
  ShmRegion::UnlinkName(page_name);
  RmTree(out_dir);
  RunRingCapacity(page_name, out_dir);
  RmTree(out_dir);
  UnlinkShmPair(page_name);
  return 0;
}
