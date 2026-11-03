// test_pinned_slot_injector.cpp — REP-04: replay big-frame segments into a live
// PinnedSlotStore.
//
// The fixture records a real trigger package in-process (mirror ring + one
// big-frame channel, small samples at 4 ms and frames at 8 ms), then the
// injector replays the package's frames from the .raw mapping into a fresh
// target channel:
//
//   A: Open/geometry reporting, full injection (count/bytes/mono/flags/crc,
//      read-back equals the package segments byte for byte);
//   B: BF-03 pinned target slot -> explicit skip counter, head advances, the
//      stream continues (no spin, no overwrite);
//   C: oversize frame -> explicit counter, never truncated, writer's own
//      dropped_oversize stays 0 (the injector never asks Begin for it);
//   D: CRC mismatch is refused before the write; unmapped channels and bad
//      events are counted; mapping errors fail Open.
#include "thyme/check.h"
#include "thyme/contracts/frame_header.h"
#include "thyme/record/record_mirror_ring_client.h"
#include "thyme/record/triggered_recorder.h"
#include "thyme/replay/pinned_slot_injector.h"
#include "thyme/replay/replay_reader.h"
#include "thyme/runtime/pinned_slot_store.h"
#include "thyme/runtime/record_mirror_ring.h"
#include "thyme/runtime/shm_region.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

using namespace thyme;

namespace {

constexpr uint64_t kPaceNs = 4000000;      // 4 ms between samples
constexpr uint64_t kBeforeNs = 100000000;  // 100 ms pre window
constexpr uint64_t kAfterNs = 200000000;   // 200 ms post window
constexpr uint64_t kTriggerIter = 30;
constexpr uint64_t kTotalIters = 90;
constexpr uint32_t kFrameSlotBytes = 4096;
constexpr uint32_t kFrameSlots = 128;

struct Pod16 {
  uint64_t a;
  uint32_t b;
  uint32_t c;
};

uint32_t FrameLen(uint64_t frame) {
  return 512 + static_cast<uint32_t>(frame % 8) * 256;
}

void FillFrame(uint8_t* p, uint32_t len, uint64_t frame) {
  for (uint32_t j = 0; j < len; ++j) {
    p[j] = static_cast<uint8_t>(frame * 13 + j * 7);
  }
}

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

void RmTree(const std::string& path) {
  DIR* d = opendir(path.c_str());
  if (d == nullptr) {
    return;
  }
  while (dirent* e = readdir(d)) {
    if (e->d_name[0] == '.') {
      continue;
    }
    const std::string sub = path + "/" + e->d_name;
    struct stat st {};
    if (stat(sub.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) {
      RmTree(sub);
    } else {
      std::remove(sub.c_str());
    }
  }
  closedir(d);
  rmdir(path.c_str());
}

struct Fixture {
  std::string root;
  std::string page_name;
  std::string chan_name;
  std::string pkg_dir;
  record_mirror::RingClient mirror;
  PinnedSlotWriter frames;
  triggered::TriggeredRecorder rec;
  uint64_t sample_seq = 0;
  uint64_t frame_seq = 0;

  void Init() {
    char buf[160];
    const long me = static_cast<long>(getpid());
    std::snprintf(buf, sizeof(buf), "/tmp/thyme_rep04_%ld", me);
    root = buf;
    RmTree(root);
    CHECK(mkdir(root.c_str(), 0700) == 0);
    CHECK(mkdir((root + "/out").c_str(), 0700) == 0);
    std::snprintf(buf, sizeof(buf), "/thyme_rep04_p_%ld", me);
    page_name = buf;
    std::snprintf(buf, sizeof(buf), "/thyme_rep04_c_%ld", me);
    chan_name = buf;
    ShmRegion::UnlinkName(page_name.c_str());
    ShmRegion::UnlinkName(chan_name.c_str());

    ThymeStructDesc desc{};
    desc.struct_id = 0;
    desc.contract_version = 1;
    desc.payload_size = sizeof(Pod16);
    std::snprintf(desc.name, sizeof(desc.name), "Pod16");
    RecordMirrorRingSpec spec{sizeof(Pod16), 256};
    CHECK(mirror.Create(page_name.c_str(), &desc, &spec, 1, 7));
    CHECK(frames.Create(chan_name.c_str(), kFrameSlotBytes, kFrameSlots, 0));
    frames.SetCrcEnabled(true);  // REC-21: transport CRC defaults off

    triggered::Config cfg;
    cfg.out_dir = root + "/out";
    triggered::SourceConfig mirror_src;
    mirror_src.kind = triggered::SourceConfig::Kind::kMirrorRing;
    mirror_src.shm_name = page_name;
    triggered::SourceConfig frame_src;
    frame_src.kind = triggered::SourceConfig::Kind::kPinnedSlot;
    frame_src.shm_name = chan_name;
    frame_src.channel_name = "cloud_1";
    cfg.sources = {mirror_src, frame_src};
    CHECK(rec.Init(cfg));
  }

  void Publish(uint64_t iter) {
    Pod16 p{};
    p.a = sample_seq;
    p.b = static_cast<uint32_t>(sample_seq * 2);
    p.c = static_cast<uint32_t>(sample_seq * 3);
    CHECK(mirror.Page()->TryWriteRing(0, &p, sizeof(p), NowNs(),
                                      static_cast<uint32_t>(sample_seq)));
    sample_seq += 1;
    // Different rate: one frame every other sample period (8 ms).
    if (iter % 2 != 0) {
      return;
    }
    const uint32_t len = FrameLen(frame_seq);
    PinnedSlotWriter::BeginView bv = frames.Begin(len);
    CHECK(bv.payload != nullptr);
    FillFrame(bv.payload, len, frame_seq);
    // IDR bit every 4th frame, exactly what replay must preserve.
    bv.header->flags.store(frame_seq % 4 == 0 ? 1u : 0u,
                           std::memory_order_relaxed);
    frames.Commit(NowNs());
    frame_seq += 1;
  }

  bool Record() {
    for (uint64_t i = 0; i < kTriggerIter; ++i) {
      Publish(i);
      rec.Tick();
      SleepNs(kPaceNs);
    }
    CHECK(rec.Trigger(1, kBeforeNs, kAfterNs));
    for (uint64_t i = kTriggerIter; i < kTotalIters; ++i) {
      Publish(i);
      rec.Tick();
      SleepNs(kPaceNs);
    }
    const uint64_t deadline = NowNs() + 15000000000ULL;
    while (rec.phase() != triggered::Phase::kArmed) {
      rec.Tick();
      if (NowNs() >= deadline) {
        return false;
      }
      SleepNs(100000);
    }
    pkg_dir = rec.last_package_dir();
    return !pkg_dir.empty();
  }

  void Shutdown() {
    rec.Shutdown();
    frames.Close();
    mirror.Shutdown(true);
  }

  void Cleanup() {
    Shutdown();
    RmTree(root);
  }
};

struct ExpectedFrame {
  uint64_t mono_ns = 0;
  uint32_t len = 0;
  uint32_t flags = 0;
  uint32_t crc = 0;
  std::vector<uint8_t> bytes;
};

std::vector<ExpectedFrame> CollectFrames(const std::string& dir,
                                         uint32_t channel) {
  std::vector<ExpectedFrame> out;
  replay::ReplayPackage pkg;
  CHECK(pkg.Open(dir) == replay::ReplayStatus::kOk);
  replay::ReplayEvent ev;
  while (pkg.NextEvent(&ev) == replay::ReplayStatus::kOk) {
    if (ev.channel != channel || ev.kind != replay::ReplayKind::kData) {
      continue;
    }
    ExpectedFrame f;
    f.mono_ns = ev.mono_ns;
    f.len = ev.len;
    f.flags = ev.frame_flags;
    f.crc = ev.payload_crc;
    f.bytes.assign(ev.payload, ev.payload + ev.len);
    out.push_back(std::move(f));
  }
  return out;
}

uint32_t FindChannel(replay::ReplayPackage& pkg, const std::string& name) {
  for (const replay::ReplayChannel& c : pkg.channels()) {
    if (c.name == name) {
      return c.id;
    }
  }
  CHECK(false);
  return 0;
}

std::string TargetName(const char* tag, uint32_t slot_bytes,
                       uint32_t slot_count) {
  char buf[128];
  std::snprintf(buf, sizeof(buf), "/thyme_rep04_%s_%ld_%u_%u", tag,
                static_cast<long>(getpid()), slot_bytes, slot_count);
  ShmRegion::UnlinkName(buf);
  ShmRegion::UnlinkName(PinnedSlotWriter::LeaseShmName(buf).c_str());
  return buf;
}

// Reads frame `index` back from an attached reader and compares it with
// `want`.
void CheckFrame(PinnedSlotReader& reader, uint64_t index,
                const ExpectedFrame& want) {
  PinnedSlotReader::View v;
  CHECK(reader.ReadFrame(index, v) == PinnedSlotReader::ReadResult::kOk);
  CHECK(v.frame_index == index);
  CHECK(v.data_len == want.len);
  CHECK(v.mono_ns == want.mono_ns);
  CHECK(v.flags == want.flags);
  CHECK(v.crc32 == want.crc);
  CHECK(v.crc_ok);
  CHECK(std::memcmp(v.payload, want.bytes.data(), want.len) == 0);
}

}  // namespace

int main() {
  Fixture fx;
  fx.Init();
  CHECK(fx.Record());
  fx.Shutdown();

  replay::ReplayPackage pkg;
  CHECK(pkg.Open(fx.pkg_dir) == replay::ReplayStatus::kOk);
  const uint32_t big_ch = FindChannel(pkg, "cloud_1");
  const uint32_t small_ch = FindChannel(pkg, "small");
  const std::vector<ExpectedFrame> frames = CollectFrames(fx.pkg_dir, big_ch);
  CHECK(frames.size() > 20);

  uint64_t total_bytes = 0;
  for (const ExpectedFrame& f : frames) {
    total_bytes += f.len;
  }

  // ---- A: full injection into a fresh target channel ----
  {
    const std::string target = TargetName("a", kFrameSlotBytes, kFrameSlots);
    PinnedSlotWriter writer;
    CHECK(writer.Create(target.c_str(), kFrameSlotBytes, kFrameSlots, 0));
    writer.SetCrcEnabled(true);  // REC-21: transport CRC defaults off
    replay::PinnedSlotTarget map{big_ch, &writer};

    replay::PinnedSlotInjector injector;
    CHECK(injector.Open(pkg, &map, 1) == replay::ReplayStatus::kOk);
    CHECK(injector.is_open());
    CHECK(injector.verify_crc());
    // REP-10: the injector owns the target's producer gate.
    CHECK(writer.owns_producer_gate());
    CHECK(writer.gate()->owner_token.load(std::memory_order_acquire) != 0);
    {
      // A second injector on the same target must be refused, not share it.
      replay::PinnedSlotInjector second;
      CHECK(second.Open(pkg, &map, 1) == replay::ReplayStatus::kBusy);
      CHECK(second.error().find("gated") != std::string::npos);
      CHECK(writer.owns_producer_gate());  // the first owner is untouched
    }
    const replay::PinnedSlotStoreStats* cs = injector.FindChannel(big_ch);
    CHECK(cs != nullptr);
    CHECK(cs->slot_bytes == kFrameSlotBytes);
    CHECK(cs->slot_count == kFrameSlots);
    CHECK(cs->max_frame_len == 2304);  // FrameLen max
    CHECK(cs->geometry_fits);
    CHECK(injector.stats().geometry_oversize_channels == 0);

    replay::ReplayPackage live;
    CHECK(live.Open(fx.pkg_dir) == replay::ReplayStatus::kOk);
    replay::ReplayEvent ev;
    uint64_t big_events = 0;
    while (live.NextEvent(&ev) == replay::ReplayStatus::kOk) {
      const replay::InjectStatus is = injector.Inject(ev);
      if (ev.channel == big_ch && ev.kind == replay::ReplayKind::kData) {
        big_events += 1;
        CHECK(is == replay::InjectStatus::kOk);
      } else if (ev.channel == big_ch) {
        CHECK(is == replay::InjectStatus::kIgnored);
      } else if (ev.channel == small_ch && ev.kind == replay::ReplayKind::kData) {
        CHECK(is == replay::InjectStatus::kUnmapped);
      }
    }
    CHECK(big_events == frames.size());
    const replay::PinnedSlotInjectorStats& st = injector.stats();
    CHECK(st.injected == frames.size());
    CHECK(st.bytes == total_bytes);
    CHECK(st.skipped_pinned == 0);
    CHECK(st.oversize == 0);
    CHECK(st.crc_mismatches == 0);
    CHECK(st.sequence_errors == 0);
    CHECK(st.unmapped > 0);  // the unmapped small channel
    CHECK(st.last_mono_ns == frames.back().mono_ns);
    CHECK(writer.head() == frames.size());
    CHECK(writer.dropped_oversize() == 0);

    PinnedSlotReader reader;
    CHECK(reader.Open(target.c_str(), 2000000000ULL));
    for (uint64_t i = 0; i < frames.size(); ++i) {
      CheckFrame(reader, i, frames[i]);
    }
    // REP-10: if another holder takes the gate (e.g. an expired lease was
    // reclaimed), the injector reports kGated and does not overwrite the new
    // producer.
    {
      writer.gate()->heartbeat_ns.store(0, std::memory_order_relaxed);
      const uint64_t thief = ProducerGateNewToken();
      CHECK(ProducerGateAcquire(*writer.gate(), thief, ProducerGateNowNs(),
                                10000000ULL) == GateResult::kStolen);
      replay::ReplayPackage live2;
      CHECK(live2.Open(fx.pkg_dir) == replay::ReplayStatus::kOk);
      replay::ReplayEvent ev2;
      bool got = false;
      while (live2.NextEvent(&ev2) == replay::ReplayStatus::kOk) {
        if (ev2.channel == big_ch && ev2.kind == replay::ReplayKind::kData) {
          got = true;
          break;
        }
      }
      CHECK(got);
      CHECK(injector.Inject(ev2) == replay::InjectStatus::kGated);
      CHECK(injector.stats().gated == 1);
      CHECK(ProducerGateRelease(*writer.gate(), thief) == GateResult::kOk);
    }

    // REP-10: replay end releases the gate; a real producer can write again.
    injector.Close();
    CHECK(!writer.owns_producer_gate());
    CHECK(!writer.producer_gate_held_by_other());
    PinnedSlotWriter::BeginView resumed = writer.Begin(64);
    CHECK(resumed.status == PinnedSlotWriter::BeginStatus::kOk);
    CHECK(resumed.payload != nullptr);
    writer.Commit();
    writer.Close();
  }

  // ---- B: pinned target slot is skipped, counted, and the stream goes on ----
  {
    // One slot: every frame maps to slot 0, so pinning slot 0 pins them all.
    const std::string target = TargetName("b", kFrameSlotBytes, 1);
    PinnedSlotWriter writer;
    CHECK(writer.Create(target.c_str(), kFrameSlotBytes, 1, 0));
    writer.SetCrcEnabled(true);
    replay::PinnedSlotTarget map{big_ch, &writer};
    replay::PinnedSlotInjector injector;
    CHECK(injector.Open(pkg, &map, 1) == replay::ReplayStatus::kOk);

    replay::ReplayPackage live;
    CHECK(live.Open(fx.pkg_dir) == replay::ReplayStatus::kOk);
    replay::ReplayEvent ev;

    // First frame publishes normally.
    while (live.NextEvent(&ev) == replay::ReplayStatus::kOk) {
      if (ev.channel == big_ch && ev.kind == replay::ReplayKind::kData) {
        break;
      }
    }
    CHECK(ev.len != 0);
    CHECK(injector.Inject(ev) == replay::InjectStatus::kOk);
    CHECK(writer.head() == 1);

    // A reader pins the only slot.
    PinnedSlotReader reader;
    CHECK(reader.Open(target.c_str(), 2000000000ULL));
    PinnedSlotReader::View view;
    uint32_t gen = 0;
    CHECK(reader.AcquireLease(0, view, &gen) ==
          PinnedSlotReader::ReadResult::kOk);

    // The next two frames must skip (head advances, no publish), not spin.
    uint32_t skipped = 0;
    while (skipped < 2 && live.NextEvent(&ev) == replay::ReplayStatus::kOk) {
      if (ev.channel != big_ch || ev.kind != replay::ReplayKind::kData) {
        continue;
      }
      CHECK(injector.Inject(ev) == replay::InjectStatus::kSkippedPinned);
      skipped += 1;
    }
    CHECK(skipped == 2);
    CHECK(injector.stats().skipped_pinned == 2);
    CHECK(injector.stats().injected == 1);
    CHECK(writer.head() == 3);
    CHECK(writer.pinned_skip_count() == 2);

    // Release the pin: injection resumes on the same slot.
    CHECK(reader.ReleaseLeaseStatus(0, gen) == LeaseReleaseResult::kOk);
    while (live.NextEvent(&ev) == replay::ReplayStatus::kOk) {
      if (ev.channel == big_ch && ev.kind == replay::ReplayKind::kData) {
        break;
      }
    }
    CHECK(injector.Inject(ev) == replay::InjectStatus::kOk);
    CHECK(injector.stats().injected == 2);
    CHECK(writer.head() == 4);
    writer.Close();
  }

  // ---- C: oversize frame -> explicit counter, no truncation ----
  {
    const std::string target = TargetName("c", 1024, 64);
    PinnedSlotWriter writer;
    CHECK(writer.Create(target.c_str(), 1024, 64, 0));
    writer.SetCrcEnabled(true);
    replay::PinnedSlotTarget map{big_ch, &writer};
    replay::PinnedSlotInjector injector;
    CHECK(injector.Open(pkg, &map, 1) == replay::ReplayStatus::kOk);
    const replay::PinnedSlotStoreStats* cs = injector.FindChannel(big_ch);
    CHECK(cs != nullptr);
    CHECK(!cs->geometry_fits);  // 2304 > 1024, flagged at Open
    CHECK(injector.stats().geometry_oversize_channels == 1);

    uint64_t expect_fit = 0;
    uint64_t expect_oversize = 0;
    for (const ExpectedFrame& f : frames) {
      if (f.len <= 1024) {
        expect_fit += 1;
      } else {
        expect_oversize += 1;
      }
    }
    CHECK(expect_oversize > 0);

    replay::ReplayPackage live;
    CHECK(live.Open(fx.pkg_dir) == replay::ReplayStatus::kOk);
    replay::ReplayEvent ev;
    while (live.NextEvent(&ev) == replay::ReplayStatus::kOk) {
      if (ev.channel != big_ch || ev.kind != replay::ReplayKind::kData) {
        continue;
      }
      const replay::InjectStatus is = injector.Inject(ev);
      if (ev.len <= 1024) {
        CHECK(is == replay::InjectStatus::kOk);
      } else {
        CHECK(is == replay::InjectStatus::kOversize);
      }
    }
    CHECK(injector.stats().injected == expect_fit);
    CHECK(injector.stats().oversize == expect_oversize);
    CHECK(writer.head() == expect_fit);
    // The injector refused before Begin, so the writer never saw an oversize
    // call and never truncated.
    CHECK(writer.dropped_oversize() == 0);

    // Every published frame is one of the fitting package frames, intact.
    uint64_t published = 0;
    PinnedSlotReader reader;
    CHECK(reader.Open(target.c_str(), 2000000000ULL));
    for (const ExpectedFrame& f : frames) {
      if (f.len > 1024) {
        continue;
      }
      CheckFrame(reader, published, f);
      published += 1;
    }
    CHECK(published == expect_fit);
    writer.Close();
  }

  // ---- D: CRC mismatch, bad events, mapping errors ----
  {
    const std::string target = TargetName("d", kFrameSlotBytes, 8);
    PinnedSlotWriter writer;
    CHECK(writer.Create(target.c_str(), kFrameSlotBytes, 8, 0));
    writer.SetCrcEnabled(true);
    replay::PinnedSlotTarget map{big_ch, &writer};
    replay::PinnedSlotInjector injector;
    CHECK(injector.Open(pkg, &map, 1) == replay::ReplayStatus::kOk);

    replay::ReplayEvent ev;
    replay::ReplayPackage live;
    CHECK(live.Open(fx.pkg_dir) == replay::ReplayStatus::kOk);
    // First big-frame data event (payload stays mapped while `live` is open).
    while (live.NextEvent(&ev) == replay::ReplayStatus::kOk) {
      if (ev.channel == big_ch && ev.kind == replay::ReplayKind::kData) {
        break;
      }
    }
    CHECK(ev.payload_crc != 0);
    replay::ReplayEvent bad = ev;
    bad.payload_crc ^= 1u;
    CHECK(injector.Inject(bad) == replay::InjectStatus::kCrcMismatch);
    CHECK(injector.stats().crc_mismatches == 1);
    CHECK(writer.head() == 0);  // nothing published

    // Same event, verification off: it is injected.
    injector.SetVerifyCrc(false);
    CHECK(injector.Inject(bad) == replay::InjectStatus::kOk);
    CHECK(writer.head() == 1);
    injector.SetVerifyCrc(true);

    // Bad events.
    replay::ReplayEvent empty = ev;
    empty.payload = nullptr;
    CHECK(injector.Inject(empty) == replay::InjectStatus::kBadEvent);
    CHECK(injector.stats().bad_events == 1);

    // Unmapped channel.
    replay::ReplayEvent other = ev;
    other.channel = small_ch;
    CHECK(injector.Inject(other) == replay::InjectStatus::kUnmapped);

    // Mapping errors fail Open.
    replay::PinnedSlotTarget wrong_kind{small_ch, &writer};
    CHECK(injector.Open(pkg, &wrong_kind, 1) ==
          replay::ReplayStatus::kInvalidArgument);
    CHECK(!injector.is_open());
    CHECK(!injector.error().empty());
    replay::PinnedSlotTarget null_writer{big_ch, nullptr};
    CHECK(injector.Open(pkg, &null_writer, 1) ==
          replay::ReplayStatus::kInvalidArgument);
    // Not open any more.
    CHECK(injector.Inject(ev) == replay::InjectStatus::kNotOpen);
    writer.Close();
  }

  fx.Cleanup();
  std::printf("test_pinned_slot_injector: OK (%zu frames, %llu bytes)\n",
              frames.size(), static_cast<unsigned long long>(total_bytes));
  return 0;
}
