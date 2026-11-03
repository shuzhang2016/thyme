// test_trigger_capture_pipeline.cpp — pipeline-level integration test for
// triggered acquisition (design doc 4.4/6.1), simulating the P2 recorder flow
// with existing components only (no trigger_center / recorder process):
//
//   producer child process: ShmSpmcRing + LatestSlot tapped into a
//     RecordMirrorRingPage (SetRecordTap wiring, publishes ONLY via
//     Push/Write), plus a PinnedSlotStore as the /h264_*-style large-frame
//     channel;
//   parent process as recorder: continuous sampling -> simulated trigger at
//     t0 -> pre-window HarvestWindow + big-frame head lookback -> post-window
//     continued harvest on both sides -> drain to a real .thyme.bin + raw
//     segment file with seqlock re-validation and hole (gap) markers ->
//     read-back verification.
//
// Scenarios:
//   A: end-to-end happy path (pre/post windows, exclusion of out-of-window
//      samples, trigger metadata, frame crc);
//   B: trigger right after producer start — window saturates the ring's
//      available history without wrapping or phantom samples;
//   C: producer overruns the shallow post-window ring during drain — evicted
//      descriptors become gap markers with exact accounting.
#include "thyme/check.h"
#include "thyme/contracts/frame_header.h"
#include "thyme/record/record_mirror_ring_client.h"
#include "thyme/record/thyme_bin.h"
#include "thyme/runtime/pinned_slot_store.h"
#include "thyme/runtime/latest_slot.h"
#include "thyme/runtime/record_mirror_ring.h"
#include "thyme/runtime/shm_region.h"
#include "thyme/runtime/shm_spmc_ring.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

using namespace thyme;

namespace {

constexpr uint16_t kSid0 = 0;        // ShmSpmcRing-tapped small struct
constexpr uint16_t kSid1 = 1;        // LatestSlot-tapped small struct
constexpr uint16_t kSidFrameIdx = 60;  // big-frame index records
constexpr uint16_t kSidFrameGap = 61;  // big-frame hole markers
constexpr uint16_t kSidMeta = 63;    // trigger window metadata record
constexpr uint16_t kGapOverrun = 1;  // hole reason: slot overwritten

constexpr uint32_t kFrameSlotBytes = 4096;
// Deep enough that scenario A's 70 frames are never overwritten before drain
// (the happy path asserts zero holes).
constexpr uint32_t kFrameSlots = 128;

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

// thyme_bin index record for one raw frame segment (design doc 6.1).
struct FrameIdxRec {
  uint64_t offset;
  uint32_t len;
  uint32_t flags;
};
// Window parameters the reader API cannot recover from a kTrigger entry.
struct TriggerMeta {
  uint64_t t0;
  uint64_t before_ns;
  uint64_t after_ns;
  uint64_t config_hash;
};

// One drained big-frame view, retained for drain-time validation.
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

ThymeStructDesc MakeDesc(uint16_t sid, uint32_t payload, const char* name) {
  ThymeStructDesc d{};
  d.struct_id = sid;
  d.contract_version = 1;
  d.payload_size = payload;
  std::snprintf(d.name, sizeof(d.name), "%s", name);
  return d;
}

uint32_t FrameLen(uint64_t f) {
  return 512 + static_cast<uint32_t>(f % 8) * 256;
}

void FillFrame(uint8_t* p, uint32_t len, uint64_t f) {
  for (uint32_t j = 0; j < len; ++j) {
    p[j] = static_cast<uint8_t>(f * 13 + j * 7);
  }
}

// Drain-time validation of a retained frame view: length, payload pattern,
// and crc must all agree with the descriptor harvested earlier (the
// zero-copy-swap seqlock re-check of design doc 4.4 step 3).
bool FrameRecValid(const FrameRec& r) {
  if (r.data_len != FrameLen(r.frame_index)) return false;
  for (uint32_t j = 0; j < r.data_len; ++j) {
    if (r.payload[j] != static_cast<uint8_t>(r.frame_index * 13 + j * 7)) {
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

void WriteAll(int fd, const void* data, std::size_t n) {
  const uint8_t* p = static_cast<const uint8_t*>(data);
  std::size_t left = n;
  while (left > 0) {
    const ssize_t w = write(fd, p, left);
    CHECK(w > 0);
    p += w;
    left -= static_cast<std::size_t>(w);
  }
}

// ---------------------------------------------------------------- scenario A

void ProducerChildA(const char* page_name, const char* ring_name,
                    const char* frame_name, int ready_fd) {
  ThymeStructDesc descs[2] = {MakeDesc(kSid0, sizeof(Pod16), "Pod16"),
                              MakeDesc(kSid1, sizeof(Pod32), "Pod32")};
  RecordMirrorRingSpec specs[2] = {{sizeof(Pod16), 256}, {sizeof(Pod32), 256}};
  record_mirror::RingClient mirror;
  CHECK(mirror.Create(page_name, descs, specs, 2, 42));
  ShmSpmcRing<Pod16, 16> ring;
  CHECK(ring.Create(ring_name));
  ring.SetRecordTap(mirror.Page(), kSid0);
  LatestSlot<Pod32> latest;
  latest.SetRecordTap(mirror.Page(), kSid1);
  PinnedSlotWriter frames;
  CHECK(frames.Create(frame_name, kFrameSlotBytes, kFrameSlots, 64));
  frames.SetCrcEnabled(true);  // REC-21: transport CRC defaults off
  std::memcpy(frames.MetaBlob(), "H264SIM", 7);

  WriteAll(ready_fd, "R", 1);
  for (uint64_t i = 0; i < 70; ++i) {
    Pod16 p{i, static_cast<uint32_t>(i * 2), static_cast<uint32_t>(i * 3)};
    ring.Push(p);  // node-oblivious: the tap fires inside Push
    Pod32 q{};
    q.a = i;
    q.b = i;
    for (uint32_t j = 0; j < sizeof(q.fill); ++j) {
      q.fill[j] = static_cast<uint8_t>(i * 5 + j);
    }
    latest.Write(q);
    PinnedSlotWriter::BeginView bv = frames.Begin(FrameLen(i));
    CHECK(bv.payload != nullptr);
    FillFrame(bv.payload, FrameLen(i), i);
    bv.header->flags.store(i % 16 == 0 ? 1u : 0u, std::memory_order_relaxed);
    frames.Commit();
    SleepNs(1000000);  // 1 ms cadence
  }
  frames.Close();
  ring.Close();
  mirror.Shutdown(false);  // leave shm for the recorder
  _exit(0);
}

void RunEndToEnd() {
  constexpr uint64_t kBeforeNs = 30000000ULL;  // 30 ms
  constexpr uint64_t kAfterNs = 15000000ULL;   // 15 ms
  const int pid_int = static_cast<int>(getpid());
  char page_name[64], ring_name[64], frame_name[64], tbin[64], rawf[64];
  std::snprintf(page_name, sizeof(page_name), "/c9pipe_a_page_%d", pid_int);
  std::snprintf(ring_name, sizeof(ring_name), "/c9pipe_a_ring_%d", pid_int);
  std::snprintf(frame_name, sizeof(frame_name), "/c9pipe_a_h264_%d", pid_int);
  std::snprintf(tbin, sizeof(tbin), "/tmp/c9pipe_a_%d.tbin", pid_int);
  std::snprintf(rawf, sizeof(rawf), "/tmp/c9pipe_a_%d.raw", pid_int);
  ShmRegion::UnlinkName(page_name);
  ShmRegion::UnlinkName(ring_name);
  ShmRegion::UnlinkName(frame_name);
  { const std::string l = std::string(frame_name) + ".lease"; ShmRegion::UnlinkName(l.c_str()); }
  std::remove(tbin);
  std::remove(rawf);

  int ready_pipe[2];
  CHECK(pipe(ready_pipe) == 0);
  const pid_t pid = fork();
  CHECK(pid >= 0);
  if (pid == 0) {
    close(ready_pipe[0]);
    ProducerChildA(page_name, ring_name, frame_name, ready_pipe[1]);
  }
  close(ready_pipe[1]);
  char b = 0;
  CHECK(read(ready_pipe[0], &b, 1) == 1 && b == 'R');
  close(ready_pipe[0]);

  // ---- recorder attaches to the producer's channels ----
  ThymeStructDesc page_descs[2] = {MakeDesc(kSid0, sizeof(Pod16), "Pod16"),
                                   MakeDesc(kSid1, sizeof(Pod32), "Pod32")};
  RecordMirrorRingSpec page_specs[2] = {{sizeof(Pod16), 256},
                                        {sizeof(Pod32), 256}};
  const uint64_t hash =
      RecordMirrorRingPage::HashConfig(page_descs, page_specs, 2);
  record_mirror::RingClient ro;
  CHECK(ro.AttachReadOnly(page_name, 2000000000ULL, hash));
  const RecordMirrorRingPage* page = ro.Page();
  CHECK(page->header.session_id.load(std::memory_order_acquire) == 42);
  PinnedSlotReader frames;
  CHECK(frames.Open(frame_name, 2000000000ULL));
  CHECK(std::memcmp(frames.MetaBlob(), "H264SIM", 7) == 0);

  // ---- continuous sampling, simulated trigger at t0, post-window wait ----
  std::vector<FrameRec> frecs;
  uint64_t t0 = 0;
  uint64_t h0 = 0;
  const uint64_t deadline = NowNs() + 10000000000ULL;
  for (;;) {
    DrainFrames(frames, frecs);
    const uint64_t head0 = page->RingHead(kSid0);
    if (t0 == 0 && head0 >= 25) {
      t0 = NowNs();  // simulated trigger
      h0 = head0;
    }
    if (t0 != 0 && head0 >= h0 + 20) break;  // post window covered
    CHECK(NowNs() < deadline);
    SleepNs(200000);
  }
  CHECK(t0 != 0);

  int st = 0;
  CHECK(waitpid(pid, &st, 0) == pid);
  CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0);
  DrainFrames(frames, frecs);  // writer stopped: remaining views stay valid
  CHECK(frecs.size() == 70);

  // ---- pre/post window harvest (small structs) ----
  std::vector<RecordMirrorRingSampleDesc> pre0(256), post0(256), pre1(256),
      post1(256);
  uint64_t torn = 0;
  const uint32_t n_pre0 = page->HarvestWindow(kSid0, t0 - kBeforeNs, t0,
                                              pre0.data(), 256, &torn);
  const uint32_t n_post0 = page->HarvestWindow(kSid0, t0 + 1, t0 + kAfterNs,
                                               post0.data(), 256, &torn);
  const uint32_t n_pre1 = page->HarvestWindow(kSid1, t0 - kBeforeNs, t0,
                                              pre1.data(), 256, &torn);
  const uint32_t n_post1 = page->HarvestWindow(kSid1, t0 + 1, t0 + kAfterNs,
                                               post1.data(), 256, &torn);
  CHECK(torn == 0);  // writer stopped: harvest must be tear-free
  CHECK(n_pre0 >= 15);   // ~25 expected; generous scheduling margin
  CHECK(n_post0 >= 8);   // ~15 expected
  CHECK(n_pre1 >= 15);
  CHECK(n_post1 >= 8);
  for (uint32_t i = 0; i < n_pre0; ++i) {
    CHECK(pre0[i].mono_ns >= t0 - kBeforeNs && pre0[i].mono_ns <= t0);
    if (i > 0) CHECK(pre0[i].mono_ns > pre0[i - 1].mono_ns);
  }
  for (uint32_t i = 0; i < n_post0; ++i) {
    CHECK(post0[i].mono_ns > t0 && post0[i].mono_ns <= t0 + kAfterNs);
  }

  // Big-frame head lookback: window frames by mono_ns filter.
  std::vector<const FrameRec*> window_frames;
  uint64_t outside_frames = 0;
  for (const FrameRec& r : frecs) {
    if (r.mono_ns >= t0 - kBeforeNs && r.mono_ns <= t0 + kAfterNs) {
      window_frames.push_back(&r);
    } else {
      ++outside_frames;
    }
  }
  CHECK(window_frames.size() >= 20);
  CHECK(outside_frames > 0);  // exclusion must actually exclude something

  // ---- drain to a real capture package (.tbin + raw segment) ----
  ThymeBinFileSink sink(tbin);
  CHECK(sink.ok());
  ThymeBinEncoder enc(&sink);
  FILE* raw = std::fopen(rawf, "wb");
  CHECK(raw != nullptr);
  ThymeStructDesc tbin_descs[4] = {MakeDesc(kSid0, sizeof(Pod16), "Pod16"),
                                   MakeDesc(kSid1, sizeof(Pod32), "Pod32"),
                                   MakeDesc(kSidFrameIdx, sizeof(FrameIdxRec),
                                            "FrameIdx"),
                                   MakeDesc(kSidMeta, sizeof(TriggerMeta),
                                            "TrigMeta")};
  CHECK(enc.WriteHeader(42, t0 - kBeforeNs, tbin_descs, 4));
  CHECK(enc.WriteTrigger(1, t0, kBeforeNs, kAfterNs));
  const TriggerMeta meta{t0, kBeforeNs, kAfterNs, hash};
  uint64_t rec_seq = 0;
  CHECK(enc.WriteData(rec_seq++, kSidMeta, t0, 0, &meta, sizeof(meta)));

  uint64_t gaps = 0;
  uint64_t datas = 0;
  uint8_t sample_buf[sizeof(Pod32)];
  auto DrainDescs = [&](uint16_t sid,
                        const std::vector<RecordMirrorRingSampleDesc>& pre,
                        uint32_t n_pre,
                        const std::vector<RecordMirrorRingSampleDesc>& post,
                        uint32_t n_post) {
    for (uint32_t half = 0; half < 2; ++half) {
      const RecordMirrorRingSampleDesc* ds = half == 0 ? pre.data() : post.data();
      const uint32_t n = half == 0 ? n_pre : n_post;
      for (uint32_t i = 0; i < n; ++i) {
        uint32_t sz = 0;
        uint64_t mono = 0;
        uint32_t sseq = 0;
        uint32_t stable = 0;
        // Seqlock re-validation before serialize: seq must still match the
        // harvested descriptor; otherwise the slot was overwritten -> hole.
        if (!page->ReadRingSample(sid, ds[i].publish_index, sample_buf,
                                  sizeof(sample_buf), sz, mono, sseq,
                                  &stable, &torn) ||
            stable != ds[i].slot_seq || mono != ds[i].mono_ns) {
          CHECK(enc.WriteGap(sid, ds[i].mono_ns, kGapOverrun, 1));
          ++gaps;
          continue;
        }
        CHECK(enc.WriteData(rec_seq++, sid, mono, sseq, sample_buf, sz));
        ++datas;
      }
    }
  };
  DrainDescs(kSid0, pre0, n_pre0, post0, n_post0);
  DrainDescs(kSid1, pre1, n_pre1, post1, n_post1);

  uint64_t raw_off = 0;
  for (const FrameRec* r : window_frames) {
    if (!FrameRecValid(*r)) {
      CHECK(enc.WriteGap(kSidFrameGap, r->mono_ns, kGapOverrun, 1));
      ++gaps;
      continue;
    }
    CHECK(std::fwrite(r->payload, 1, r->data_len, raw) == r->data_len);
    const FrameIdxRec idx{raw_off, r->data_len, r->flags};
    CHECK(enc.WriteData(rec_seq++, kSidFrameIdx, r->mono_ns,
                        static_cast<uint32_t>(r->frame_index), &idx,
                        sizeof(idx)));
    raw_off += r->data_len;
    ++datas;
  }
  CHECK(sink.Sync());
  CHECK(std::fclose(raw) == 0);
  CHECK(gaps == 0);  // happy path: nothing was overwritten before drain

  // ---- read back and verify the package ----
  ThymeBinReader rd(tbin);
  CHECK(rd.ok());
  CHECK(rd.header().session_id == 42);
  CHECK(rd.header().start_mono_ns == t0 - kBeforeNs);
  CHECK(rd.header().struct_count == 4);
  CHECK(rd.registry()[2].struct_id == kSidFrameIdx);

  FILE* raw_in = std::fopen(rawf, "rb");
  CHECK(raw_in != nullptr);

  ThymeBinDataEntry e;
  // Entry 1: trigger metadata (id + t0; window params are crc-verified but
  // NOT exposed by ThymeBinReader — covered via the TrigMeta record below).
  CHECK(rd.ReadNext(e) == thyme::ThymeBinReader::ReadResult::kOk);
  CHECK(e.type == ThymeBinEntryType::kTrigger);
  CHECK(e.record_sequence == 1);  // trigger_id
  CHECK(e.mono_ns == t0);
  // Entry 2: window metadata record.
  CHECK(rd.ReadNext(e) == thyme::ThymeBinReader::ReadResult::kOk);
  CHECK(e.type == ThymeBinEntryType::kData);
  CHECK(e.struct_id == kSidMeta);
  CHECK(e.payload_size == sizeof(TriggerMeta));
  TriggerMeta got_meta{};
  std::memcpy(&got_meta, e.payload.data(), sizeof(got_meta));
  CHECK(got_meta.t0 == t0);
  CHECK(got_meta.before_ns == kBeforeNs);
  CHECK(got_meta.after_ns == kAfterNs);
  CHECK(got_meta.config_hash == hash);

  uint64_t last_mono[64] = {};
  uint64_t count_sid0 = 0, count_sid1 = 0, count_idx = 0, count_gap = 0;
  while (rd.ReadNext(e) == thyme::ThymeBinReader::ReadResult::kOk) {
    if (e.type == ThymeBinEntryType::kGap) {
      ++count_gap;
      continue;
    }
    CHECK(e.type == ThymeBinEntryType::kData);
    // No out-of-window sample may appear in the package.
    CHECK(e.mono_ns >= t0 - kBeforeNs && e.mono_ns <= t0 + kAfterNs);
    // Per-struct timestamps strictly increase.
    CHECK(e.mono_ns > last_mono[e.struct_id] || last_mono[e.struct_id] == 0);
    last_mono[e.struct_id] = e.mono_ns;
    if (e.struct_id == kSid0) {
      CHECK(e.payload_size == sizeof(Pod16));
      Pod16 p{};
      std::memcpy(&p, e.payload.data(), sizeof(p));
      CHECK(p.a * 2 == p.b && p.a * 3 == p.c);
      CHECK(p.a == e.source_sequence);  // SPMC publish index preserved
      ++count_sid0;
    } else if (e.struct_id == kSid1) {
      CHECK(e.payload_size == sizeof(Pod32));
      Pod32 q{};
      std::memcpy(&q, e.payload.data(), sizeof(q));
      CHECK(q.a == q.b);
      ++count_sid1;
    } else if (e.struct_id == kSidFrameIdx) {
      CHECK(e.payload_size == sizeof(FrameIdxRec));
      FrameIdxRec idx{};
      std::memcpy(&idx, e.payload.data(), sizeof(idx));
      const uint64_t f = e.source_sequence;
      CHECK(idx.len == FrameLen(f));
      CHECK(idx.flags == (f % 16 == 0 ? 1u : 0u));
      // Raw segment integrity: pattern + crc of the bytes on disk.
      std::vector<uint8_t> seg(idx.len);
      CHECK(fseeko(raw_in, static_cast<off_t>(idx.offset), SEEK_SET) == 0);
      CHECK(std::fread(seg.data(), 1, idx.len, raw_in) == idx.len);
      for (uint32_t j = 0; j < idx.len; ++j) {
        CHECK(seg[j] == static_cast<uint8_t>(f * 13 + j * 7));
      }
      ++count_idx;
    } else {
      CHECK(false);  // unexpected struct in package
    }
  }
  CHECK(count_sid0 == n_pre0 + n_post0);
  CHECK(count_sid1 == n_pre1 + n_post1);
  CHECK(count_idx == window_frames.size());
  CHECK(count_gap == 0);
  CHECK(std::fclose(raw_in) == 0);

  ro.Shutdown(true);
  frames.Close();
  ShmRegion::UnlinkName(ring_name);
  ShmRegion::UnlinkName(frame_name);
  { const std::string l = std::string(frame_name) + ".lease"; ShmRegion::UnlinkName(l.c_str()); }
  std::remove(tbin);
  std::remove(rawf);
}

// ---------------------------------------------------------------- scenario B

// Trigger when the ring holds far fewer samples than the window would cover:
// the harvest must return exactly what exists — no wrap-around phantoms.
void RunSaturatedWindow() {
  const int pid_int = static_cast<int>(getpid());
  char page_name[64], ring_name[64], tbin[64];
  std::snprintf(page_name, sizeof(page_name), "/c9pipe_b_page_%d", pid_int);
  std::snprintf(ring_name, sizeof(ring_name), "/c9pipe_b_ring_%d", pid_int);
  std::snprintf(tbin, sizeof(tbin), "/tmp/c9pipe_b_%d.tbin", pid_int);
  ShmRegion::UnlinkName(page_name);
  ShmRegion::UnlinkName(ring_name);
  std::remove(tbin);

  const pid_t pid = fork();
  CHECK(pid >= 0);
  if (pid == 0) {
    ThymeStructDesc d = MakeDesc(kSid0, sizeof(Pod16), "Pod16");
    RecordMirrorRingSpec spec{sizeof(Pod16), 256};
    record_mirror::RingClient mirror;
    CHECK(mirror.Create(page_name, &d, &spec, 1, 7));
    ShmSpmcRing<Pod16, 16> ring;
    CHECK(ring.Create(ring_name));
    ring.SetRecordTap(mirror.Page(), kSid0);
    for (uint64_t i = 0; i < 5; ++i) {  // only 5 samples, then exit
      Pod16 p{i, static_cast<uint32_t>(i * 2), static_cast<uint32_t>(i * 3)};
      ring.Push(p);
      SleepNs(1000000);
    }
    ring.Close();
    mirror.Shutdown(false);
    _exit(0);
  }
  int st = 0;
  CHECK(waitpid(pid, &st, 0) == pid);
  CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0);

  record_mirror::RingClient ro;
  CHECK(ro.AttachReadOnly(page_name, 2000000000ULL));
  const RecordMirrorRingPage* page = ro.Page();
  const uint64_t t0 = NowNs();  // trigger after the producer already exited
  constexpr uint64_t kBeforeNs = 50000000ULL;  // 50 ms >> 5 ms of history
  constexpr uint64_t kAfterNs = 5000000ULL;

  RecordMirrorRingSampleDesc pre[256], post[256];
  const uint32_t n_pre = page->HarvestWindow(kSid0, t0 - kBeforeNs, t0,
                                             pre, 256);
  const uint32_t n_post = page->HarvestWindow(kSid0, t0 + 1, t0 + kAfterNs,
                                              post, 256);
  CHECK(n_pre == 5);   // saturated window: exactly the 5 real samples
  CHECK(n_post == 0);  // nothing after t0
  for (uint32_t i = 0; i < n_pre; ++i) {
    CHECK(pre[i].publish_index == i);  // no phantom wrap-around entries
    CHECK(pre[i].mono_ns <= t0);
  }

  ThymeBinFileSink sink(tbin);
  CHECK(sink.ok());
  ThymeBinEncoder enc(&sink);
  ThymeStructDesc d = MakeDesc(kSid0, sizeof(Pod16), "Pod16");
  CHECK(enc.WriteHeader(7, t0 - kBeforeNs, &d, 1));
  CHECK(enc.WriteTrigger(2, t0, kBeforeNs, kAfterNs));
  uint8_t buf[sizeof(Pod16)];
  for (uint32_t i = 0; i < n_pre; ++i) {
    uint32_t sz = 0;
    uint64_t mono = 0;
    uint32_t sseq = 0;
    CHECK(page->ReadRingSample(kSid0, pre[i].publish_index, buf, sizeof(buf),
                               sz, mono, sseq));
    CHECK(enc.WriteData(i, kSid0, mono, sseq, buf, sz));
  }
  CHECK(sink.Sync());

  ThymeBinReader rd(tbin);
  CHECK(rd.ok());
  CHECK(rd.header().session_id == 7);
  ThymeBinDataEntry e;
  CHECK(rd.ReadNext(e) == thyme::ThymeBinReader::ReadResult::kOk &&
      e.type == ThymeBinEntryType::kTrigger);
  CHECK(e.record_sequence == 2 && e.mono_ns == t0);
  uint64_t n = 0;
  uint64_t last_mono = 0;
  while (rd.ReadNext(e) == thyme::ThymeBinReader::ReadResult::kOk) {
    CHECK(e.type == ThymeBinEntryType::kData);
    CHECK(e.struct_id == kSid0);
    CHECK(e.mono_ns <= t0);  // nothing from outside the window
    CHECK(e.mono_ns > last_mono || n == 0);
    last_mono = e.mono_ns;
    Pod16 p{};
    std::memcpy(&p, e.payload.data(), sizeof(p));
    CHECK(p.a == n);
    ++n;
  }
  CHECK(n == 5);
  ro.Shutdown(true);
  ShmRegion::UnlinkName(ring_name);
  std::remove(tbin);
}

// ---------------------------------------------------------------- scenario C

// Post-window overrun: the producer keeps writing through the post window on
// a depth-4 ring, so most descriptors harvested during the window are already
// evicted at drain time and must become hole markers with exact accounting.
void RunPostOverrunHoles() {
  const int pid_int = static_cast<int>(getpid());
  char page_name[64], ring_name[64], tbin[64];
  std::snprintf(page_name, sizeof(page_name), "/c9pipe_c_page_%d", pid_int);
  std::snprintf(ring_name, sizeof(ring_name), "/c9pipe_c_ring_%d", pid_int);
  std::snprintf(tbin, sizeof(tbin), "/tmp/c9pipe_c_%d.tbin", pid_int);
  ShmRegion::UnlinkName(page_name);
  ShmRegion::UnlinkName(ring_name);
  std::remove(tbin);

  int ready_pipe[2], go_pipe[2], done_pipe[2];
  CHECK(pipe(ready_pipe) == 0 && pipe(go_pipe) == 0 && pipe(done_pipe) == 0);
  const pid_t pid = fork();
  CHECK(pid >= 0);
  if (pid == 0) {
    close(ready_pipe[0]);
    close(go_pipe[1]);
    close(done_pipe[0]);
    ThymeStructDesc d = MakeDesc(kSid0, sizeof(Pod16), "Pod16");
    RecordMirrorRingSpec spec{sizeof(Pod16), 4};  // shallow ring
    record_mirror::RingClient mirror;
    CHECK(mirror.Create(page_name, &d, &spec, 1, 9));
    ShmSpmcRing<Pod16, 16> ring;
    CHECK(ring.Create(ring_name));
    ring.SetRecordTap(mirror.Page(), kSid0);
    WriteAll(ready_pipe[1], "R", 1);
    char g = 0;
    CHECK(read(go_pipe[0], &g, 1) == 1 && g == 'G');
    for (uint64_t i = 0; i < 100; ++i) {  // paced phase: harvestable
      Pod16 p{i, static_cast<uint32_t>(i * 2), static_cast<uint32_t>(i * 3)};
      ring.Push(p);
      SleepNs(200000);
    }
    for (uint64_t i = 100; i < 300; ++i) {  // blast phase: overwrites slots
      Pod16 p{i, static_cast<uint32_t>(i * 2), static_cast<uint32_t>(i * 3)};
      ring.Push(p);
    }
    WriteAll(done_pipe[1], "D", 1);
    ring.Close();
    mirror.Shutdown(false);
    _exit(0);
  }
  close(ready_pipe[1]);
  close(go_pipe[0]);
  close(done_pipe[1]);
  char b = 0;
  CHECK(read(ready_pipe[0], &b, 1) == 1 && b == 'R');

  record_mirror::RingClient ro;
  CHECK(ro.AttachReadOnly(page_name, 2000000000ULL));
  const RecordMirrorRingPage* page = ro.Page();

  const uint64_t t0 = NowNs();  // trigger; post window starts now
  WriteAll(go_pipe[1], "G", 1);

  // Post-window continued harvest (design doc 4.4 step 2): poll and append
  // new descriptors until the producer signals completion.
  CHECK(fcntl(done_pipe[0], F_SETFL, O_NONBLOCK) == 0);
  std::vector<RecordMirrorRingSampleDesc> queue;
  uint64_t next_index = 0;
  uint64_t torn = 0;
  RecordMirrorRingSampleDesc buf[64];
  const uint64_t deadline = NowNs() + 10000000000ULL;
  for (;;) {
    const uint32_t n = page->HarvestWindow(kSid0, t0 + 1, ~0ULL, buf, 64,
                                           &torn);
    for (uint32_t i = 0; i < n; ++i) {
      if (buf[i].publish_index >= next_index) {
        queue.push_back(buf[i]);
        next_index = buf[i].publish_index + 1;
      }
    }
    char d = 0;
    const ssize_t r = read(done_pipe[0], &d, 1);
    if (r == 1 && d == 'D') break;
    CHECK(NowNs() < deadline);
    SleepNs(50000);
  }
  const uint64_t t1 = NowNs();
  CHECK(queue.size() >= 20);  // paced phase alone should queue ~100

  int st = 0;
  CHECK(waitpid(pid, &st, 0) == pid);
  CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0);
  // The producer stopped at head 300 on a depth-4 ring: at most the last 4
  // queued descriptors can still be read; the rest are overrun holes.
  CHECK(page->RingHead(kSid0) == 300);

  ThymeBinFileSink sink(tbin);
  CHECK(sink.ok());
  ThymeBinEncoder enc(&sink);
  ThymeStructDesc d = MakeDesc(kSid0, sizeof(Pod16), "Pod16");
  CHECK(enc.WriteHeader(9, t0, &d, 1));
  CHECK(enc.WriteTrigger(3, t0, 0, t1 - t0));
  uint8_t sbuf[sizeof(Pod16)];
  uint64_t gaps = 0, datas = 0, rec_seq = 0;
  for (const RecordMirrorRingSampleDesc& desc : queue) {
    uint32_t sz = 0;
    uint64_t mono = 0;
    uint32_t sseq = 0;
    uint32_t stable = 0;
    if (!page->ReadRingSample(kSid0, desc.publish_index, sbuf, sizeof(sbuf),
                              sz, mono, sseq, &stable, &torn) ||
        stable != desc.slot_seq) {
      CHECK(enc.WriteGap(kSid0, desc.mono_ns, kGapOverrun, 1));
      ++gaps;
      continue;
    }
    CHECK(enc.WriteData(rec_seq++, kSid0, mono, sseq, sbuf, sz));
    ++datas;
  }
  CHECK(sink.Sync());
  CHECK(gaps + datas == queue.size());  // every descriptor accounted for
  CHECK(datas <= 4);                    // depth-4 ring: at most 4 survive
  CHECK(gaps >= 10);

  ThymeBinReader rd(tbin);
  CHECK(rd.ok());
  ThymeBinDataEntry e;
  CHECK(rd.ReadNext(e) == thyme::ThymeBinReader::ReadResult::kOk &&
      e.type == ThymeBinEntryType::kTrigger);
  CHECK(e.record_sequence == 3 && e.mono_ns == t0);
  uint64_t n_gap = 0, n_data = 0;
  uint64_t last_mono = 0;
  while (rd.ReadNext(e) == thyme::ThymeBinReader::ReadResult::kOk) {
    CHECK(e.mono_ns > t0 && e.mono_ns <= t1);  // holes stay in-window
    CHECK(e.mono_ns > last_mono || (n_gap + n_data) == 0);
    last_mono = e.mono_ns;
    if (e.type == ThymeBinEntryType::kGap) {
      CHECK(e.struct_id == kSid0);
      CHECK(e.reserved == kGapOverrun);      // hole reason
      CHECK(e.source_sequence == 1);         // missed_count
      ++n_gap;
    } else {
      CHECK(e.type == ThymeBinEntryType::kData);
      Pod16 p{};
      std::memcpy(&p, e.payload.data(), sizeof(p));
      CHECK(p.a * 2 == p.b && p.a * 3 == p.c);
      ++n_data;
    }
  }
  CHECK(n_gap == gaps);
  CHECK(n_data == datas);
  ro.Shutdown(true);
  ShmRegion::UnlinkName(ring_name);
  std::remove(tbin);
}

}  // namespace

int main() {
  RunEndToEnd();
  RunSaturatedWindow();
  RunPostOverrunHoles();
  return 0;
}
