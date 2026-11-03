// test_pinned_slot_drain_tear.cpp — R1: a frame whose slot is overwritten while
// the recorder drains it must never be written to .raw as valid. The engine
// drain is the lease path (reader pin -> writer skip; acquire failure
// -> GAP); the ReadFrameStable copy primitive it replaced stays as the
// caller-owned-scratch API (bench_core, cross-lease retention), exercised
// directly here.
//
//   1. deterministic unit level: stable read of a stable frame; stomped
//      payload (crc mismatch) -> kTorn; advanced slot -> kOverrun;
//   2. engine level: descriptors queued, then the producer laps the channel
//      before settle -> every frame drains as GAP, zero raw bytes, wal all
//      hole rows;
//   3. race level: producer thread hammers MB-class frames through 3 slots
//      while the reader ReadFrameStable-copies; every kOk copy must be
//      content-valid, and tears must actually be observed (phase 2 reads
//      the slot the writer overwrites next, so a tear is deterministic).
#include "thyme/check.h"
#include "thyme/record/record_mirror_ring_client.h"
#include "thyme/record/thyme_bin.h"
#include "thyme/record/triggered_recorder.h"
#include "thyme/runtime/pinned_slot_store.h"
#include "thyme/runtime/record_mirror_ring.h"
#include "thyme/runtime/shm_region.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <pthread.h>
#include <string>
#include <sys/stat.h>
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

void SleepNs(uint64_t ns) {
  struct timespec ts {};
  ts.tv_sec = static_cast<time_t>(ns / 1000000000ULL);
  ts.tv_nsec = static_cast<long>(ns % 1000000000ULL);
  nanosleep(&ts, nullptr);
}

void Fill(uint8_t* p, uint32_t n, uint8_t v) { std::memset(p, v, n); }

struct WriterArg {
  PinnedSlotWriter* w;
  std::atomic<uint64_t>* counter;
  std::atomic<bool>* stop;
  uint32_t len;
};

void* WriterRun(void* p) {
  auto* a = static_cast<WriterArg*>(p);
  uint64_t i = 0;
  while (!a->stop->load(std::memory_order_relaxed)) {
    PinnedSlotWriter::BeginView bv = a->w->Begin(a->len);
    if (bv.payload != nullptr) {
      Fill(bv.payload, a->len, static_cast<uint8_t>(i));
      a->w->Commit();
      a->counter->store(i, std::memory_order_release);
      ++i;
    }
  }
  return nullptr;
}

}  // namespace

int main() {
  char name[96];
  std::snprintf(name, sizeof(name), "/c9dtear_%d", static_cast<int>(getpid()));
  ShmRegion::UnlinkName(name);

  // ---- 1. ReadFrameStable unit semantics ----
  {
    PinnedSlotWriter w;
    CHECK(w.Create(name, 512, 4, 0));
    w.SetCrcEnabled(true);  // CRC defaults off; this test stomps it
    PinnedSlotWriter::BeginView bv = w.Begin(512);
    Fill(bv.payload, 512, 0x5A);
    w.Commit(42);

    PinnedSlotReader r;
    CHECK(r.Open(name, 1000000000ULL));
    std::vector<uint8_t> scratch(512);
    PinnedSlotReader::View v;
    CHECK(r.ReadFrameStable(0, scratch.data(), 512, v) ==
          PinnedSlotReader::ReadResult::kOk);
    CHECK(v.payload == scratch.data());
    CHECK(v.data_len == 512);
    CHECK(scratch[0] == 0x5A && scratch[511] == 0x5A);

    // Payload stomped after commit (bit rot / torn content): crc mismatch ->
    // kTorn, not kOk.
    {
      ShmRegion rw;
      CHECK(rw.OpenRetry(name, PinnedSlotWriter::RegionBytes(512, 4, 0),
                         1000000000ULL, false));
      auto* h = static_cast<PinnedSlotPageHeader*>(rw.data());
      uint8_t* payload =
          reinterpret_cast<uint8_t*>(h) + PinnedSlotWriter::SlotsOffset(0) +
          sizeof(PinnedSlotHeader);
      payload[17] ^= 0xFF;
      CHECK(r.ReadFrameStable(0, scratch.data(), 512, v) ==
            PinnedSlotReader::ReadResult::kTorn);
      payload[17] ^= 0xFF;  // restore
      rw.Unmap();
    }
    // Slot advanced past the frame -> kOverrun.
    for (uint64_t f = 1; f <= 4; ++f) {
      bv = w.Begin(512);
      Fill(bv.payload, 512, static_cast<uint8_t>(f));
      w.Commit(42 + f);
    }
    CHECK(r.ReadFrameStable(0, scratch.data(), 512, v) ==
          PinnedSlotReader::ReadResult::kOverrun);
    // The view borrows the slot and may arm a debug watch; drop it before
    // the channel mapping goes away.
    v = PinnedSlotReader::View{};
    r.Close();
    w.Close();
    ShmRegion::UnlinkName(name);
  }

  // ---- 2. engine drain after the producer laps the channel: all GAP ----
  {
    char page[96], chan[96], out[96];
    const int me = static_cast<int>(getpid());
    std::snprintf(page, sizeof(page), "/c9dtear_page_%d", me);
    std::snprintf(chan, sizeof(chan), "/c9dtear_chan_%d", me);
    std::snprintf(out, sizeof(out), "/tmp/c9dtear_%d", me);
    ShmRegion::UnlinkName(page);
    ShmRegion::UnlinkName(chan);

    ThymeStructDesc desc{};
    desc.struct_id = 0;
    desc.contract_version = 1;
    desc.payload_size = 16;
    std::snprintf(desc.name, sizeof(desc.name), "Pod16");
    RecordMirrorRingSpec spec{16, 64};
    record_mirror::RingClient mirror;
    CHECK(mirror.Create(page, &desc, &spec, 1, 1));
    PinnedSlotWriter w;
    CHECK(w.Create(chan, 1000, 4, 0));
    // 8 frames of history for the pre window.
    for (uint64_t f = 0; f < 8; ++f) {
      PinnedSlotWriter::BeginView bv = w.Begin(1000);
      Fill(bv.payload, 1000, static_cast<uint8_t>(f));
      w.Commit(NowNs());
      SleepNs(200000);
    }
    triggered::Config ecfg;
    ecfg.sources = {{triggered::SourceConfig::Kind::kPinnedSlot, chan, "cloud_1"}};
    ecfg.out_dir = out;
    triggered::TriggeredRecorder rec;
    CHECK(rec.Init(ecfg));
    CHECK(rec.Trigger(1, 60000000000ULL, 0));  // pre covers all 8 frames
    // Producer laps the 4-slot channel twice before the engine settles.
    for (uint64_t f = 8; f < 16; ++f) {
      PinnedSlotWriter::BeginView bv = w.Begin(1000);
      Fill(bv.payload, 1000, static_cast<uint8_t>(f));
      w.Commit(NowNs());
    }
    const uint64_t deadline = NowNs() + 10000000000ULL;
    while (rec.phase() != triggered::Phase::kArmed) {
      rec.Tick();
      CHECK(NowNs() < deadline);
      SleepNs(100000);
    }
    CHECK(triggered::TriggeredRecorder::PackageComplete(
        rec.last_package_dir()));
    // The index is all GAP (no ok data), and raw is absent or empty.
    const std::string idx_path =
        rec.last_package_dir() + "/frames/cloud_1.index.tbin";
    uint64_t gaps = 0, datas = 0;
    ThymeBinReader rd(idx_path.c_str());
    CHECK(rd.ok());
    ThymeBinDataEntry e;
    while (rd.ReadNext(e) == thyme::ThymeBinReader::ReadResult::kOk) {
      if (e.type == ThymeBinEntryType::kGap) ++gaps;
      if (e.type == ThymeBinEntryType::kData) ++datas;
    }
    CHECK(gaps == 4);  // a 4-slot ring leaves only 4 descriptors, all overwritten into GAP
    CHECK(datas == 0);  // not a single frame was written as valid
    struct stat st {};
    const std::string raw_path =
        rec.last_package_dir() + "/frames/cloud_1.raw";
    // The .raw may exist with only its 128 B channel header; no
    // payload frame was committed.
    CHECK(stat(raw_path.c_str(), &st) != 0 || st.st_size == 128);
    w.Close();
    mirror.Shutdown(true);
    rec.Shutdown();
  }

  // ---- 4. injected write failure on the default lease path: fatal, not disguised as GAP ----
  {
    char page[96], chan[96], out[96];
    const int me = static_cast<int>(getpid());
    std::snprintf(page, sizeof(page), "/c9dtear_p4_%d", me);
    std::snprintf(chan, sizeof(chan), "/c9dtear_c4_%d", me);
    std::snprintf(out, sizeof(out), "/tmp/c9dtear4_%d", me);
    ShmRegion::UnlinkName(page);
    ShmRegion::UnlinkName(chan);
    ShmRegion::UnlinkName(PinnedSlotWriter::LeaseShmName(chan).c_str());

    ThymeStructDesc desc{};
    desc.struct_id = 0;
    desc.contract_version = 1;
    desc.payload_size = 16;
    std::snprintf(desc.name, sizeof(desc.name), "Pod16");
    RecordMirrorRingSpec spec{16, 64};
    record_mirror::RingClient mirror;
    CHECK(mirror.Create(page, &desc, &spec, 1, 1));
    PinnedSlotWriter w;
    CHECK(w.Create(chan, 1000, 4, 0));
    for (uint64_t f = 0; f < 4; ++f) {
      PinnedSlotWriter::BeginView bv = w.Begin(1000);
      Fill(bv.payload, 1000, static_cast<uint8_t>(f));
      w.Commit(NowNs());
      SleepNs(200000);
    }
    triggered::Config ecfg;
    ecfg.sources = {{triggered::SourceConfig::Kind::kPinnedSlot, chan,
                     "cloud_1"}};
    ecfg.out_dir = out;
    ecfg.fault_ctx = nullptr;
    ecfg.fault_inject_write = [](void*) { return true; };
    triggered::TriggeredRecorder rec;
    CHECK(rec.Init(ecfg));
    CHECK(rec.Trigger(1, 60000000000ULL, 0));
    const uint64_t deadline = NowNs() + 10000000000ULL;
    while (rec.phase() != triggered::Phase::kArmed) {
      rec.Tick();
      CHECK(NowNs() < deadline);
      SleepNs(100000);
    }
    // Injected write failure -> fatal -> the package is actively invalidated
    // (ENOSPC semantics, not disguised as GAP).
    CHECK(rec.disk_full());
    CHECK(rec.stats().disk_errors == 1);
    CHECK(!triggered::TriggeredRecorder::PackageComplete(
        rec.last_package_dir()));
    struct stat st {};
    CHECK(stat(rec.last_package_dir().c_str(), &st) != 0);  // renamed
    CHECK(stat((rec.last_package_dir() + ".incomplete").c_str(), &st) == 0);
    w.Close();
    mirror.Shutdown(true);
    rec.Shutdown();
  }

  // ---- 3. race: MB-class frames through 3 slots while stable-copying ----
  {
    ShmRegion::UnlinkName(name);
    PinnedSlotWriter w;
    // 3 slots: cur-2 is the slot the writer's next Commit overwrites —
    // phase 2 below races that slot deterministically instead of relying on
    // scheduling luck.
    CHECK(w.Create(name, 4u * 1024 * 1024, 3, 0));
    // CRC defaults off; the stable-copy tear detection below relies
    // on the payload CRC catching mid-copy overwrites inside the seq window.
    w.SetCrcEnabled(true);
    PinnedSlotReader r;
    CHECK(r.Open(name, 1000000000ULL));
    std::atomic<uint64_t> counter{0};
    std::atomic<bool> stop{false};
    WriterArg arg{&w, &counter, &stop, 4u * 1024 * 1024};
    pthread_t th{};
    CHECK(pthread_create(&th, nullptr, WriterRun, &arg) == 0);

    std::vector<uint8_t> scratch(4u * 1024 * 1024);
    uint64_t ok_reads = 0, tears = 0;
    const uint64_t deadline = NowNs() + 3000000000ULL;
    // Two phases, neither relying on scheduling luck:
    //  1) read the newest frame cur (the writer only returns to its slot after
    //     3 Commits) to accumulate ok_reads, validating each kOk copy's content;
    //  2) read cur-2 — in a 3-slot ring that is exactly the slot the writer
    //     overwrites on its next Commit, so the copy+revalidate window must
    //     overlap the writer's 4MB memset and a tear must appear.
    for (int phase = 0; phase < 2; ++phase) {
      while (NowNs() < deadline && (phase == 0 ? ok_reads < 200 : tears == 0)) {
        const uint64_t cur = counter.load(std::memory_order_acquire);
        if (cur == 0) continue;
        const uint64_t target = (phase == 0 || cur < 2) ? cur : cur - 2;
        PinnedSlotReader::View v;
        const PinnedSlotReader::ReadResult rr =
            r.ReadFrameStable(target, scratch.data(),
                              static_cast<uint32_t>(scratch.size()), v);
        if (rr == PinnedSlotReader::ReadResult::kOk) {
          // A kOk copy must be content-valid: uniform byte == frame & 0xFF.
          const uint8_t expect = static_cast<uint8_t>(v.frame_index);
          for (uint32_t j = 0; j < v.data_len; j += 4096) {
            CHECK(scratch[j] == expect);
          }
          CHECK(scratch[v.data_len - 1] == expect);
          ++ok_reads;
        } else if (rr == PinnedSlotReader::ReadResult::kTorn) {
          ++tears;
        }
      }
    }
    stop.store(true, std::memory_order_relaxed);
    CHECK(pthread_join(th, nullptr) == 0);
    std::fprintf(stderr, "drain_tear race: ok=%llu tears=%llu\n",
                 static_cast<unsigned long long>(ok_reads),
                 static_cast<unsigned long long>(tears));
    CHECK(ok_reads >= 10);   // every kOk copy above was content-validated
    CHECK(tears > 0);        // the race window was actually exercised
    r.Close();
    w.Close();
    ShmRegion::UnlinkName(name);
  }
  return 0;
}
