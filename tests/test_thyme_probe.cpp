// test_thyme_probe.cpp — ECO-09/ECO-11: the read-only probe API over the four
// SHM containers. Asserts discovery, geometry, rate/bandwidth sanity, latency
// percentiles, echo decoding, not-found/bad-geometry statuses, and — most
// importantly — that probing leaves producer counters and page bytes
// untouched (no writes, no lease pins, no shared cursor advance).
#include "test_shm.h"
#include "thyme/app/mp_control.h"
#include "thyme/check.h"
#include "thyme/contracts/frame.h"
#include "thyme/probe/probe.h"
#include "thyme/runtime/pinned_slot_store.h"
#include "thyme/runtime/latest_slot.h"
#include "thyme/runtime/latest_slot_page.h"
#include "thyme/runtime/shared_health.h"
#include "thyme/runtime/shm_region.h"
#include "thyme/runtime/shm_spmc_ring.h"
#include "thyme/runtime/shm_spsc_ring.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <sys/mman.h>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <vector>

using namespace thyme;
using namespace thyme::probe;

namespace {

constexpr uint64_t kMs = 1000000ULL;

struct Pod {
  uint32_t a;
  uint32_t b;
};
using Msg = Frame<Pod>;

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

std::string Unique(const char* tag) { return test::ShmTestName(tag); }

void SetTestOptions(ProbeApi* api) {
  Options options;
  options.prefixes = {test::ShmTestScanPrefix()};
  api->SetOptions(options);
}

uint64_t HashShm(const std::string& name) {
  const int fd = shm_open(name.c_str(), O_RDONLY, 0600);
  CHECK(fd >= 0);
  struct stat st {};
  CHECK(fstat(fd, &st) == 0);
  const auto size = static_cast<std::size_t>(st.st_size);
  void* p = mmap(nullptr, size, PROT_READ, MAP_SHARED, fd, 0);
  CHECK(p != MAP_FAILED);
  const uint8_t* b = static_cast<const uint8_t*>(p);
  uint64_t h = 1469598103934665603ULL;
  for (std::size_t i = 0; i < size; ++i) {
    h ^= b[i];
    h *= 1099511628211ULL;
  }
  munmap(p, size);
  close(fd);
  return h;
}

uint64_t RawU64(const std::string& name, std::size_t offset) {
  const int fd = shm_open(name.c_str(), O_RDONLY, 0600);
  CHECK(fd >= 0);
  uint64_t v = 0;
  CHECK(pread(fd, &v, sizeof(v), static_cast<off_t>(offset)) ==
        static_cast<ssize_t>(sizeof(v)));
  close(fd);
  return v;
}

uint32_t RawU32(const std::string& name, std::size_t offset) {
  const int fd = shm_open(name.c_str(), O_RDONLY, 0600);
  CHECK(fd >= 0);
  uint32_t v = 0;
  CHECK(pread(fd, &v, sizeof(v), static_cast<off_t>(offset)) ==
        static_cast<ssize_t>(sizeof(v)));
  close(fd);
  return v;
}

Msg MakeFrame(uint32_t seq, uint64_t stamp, uint32_t a, uint32_t b,
              uint32_t magic) {
  Msg m{};
  FrameInit(m, seq, stamp, 7);
  m.payload.a = a;
  m.payload.b = b;
  FrameSeal(m, magic, 3);
  return m;
}

const RegionInfo* Find(const std::vector<RegionInfo>& rows,
                       const std::string& name) {
  for (const auto& r : rows) {
    if (r.name == name) return &r;
  }
  return nullptr;
}

uint64_t HistogramTotal(const LatencyStats& s) {
  uint64_t total = 0;
  for (std::size_t i = 0; i < Histogram::kBuckets; ++i) {
    total += s.histogram.count[i];
  }
  return total;
}

void TestLatestSlot() {
  const std::string name = Unique("latest");
  ShmRegion::UnlinkName(name.c_str());
  using Model = LatestSlotLayoutModel<Msg>;
  constexpr uint64_t kPayloadHash = 0xA1B2C3D4E5F60718ULL;
  ShmRegion region;
  auto* page = CreateLatestSlotPage<Msg>(region, name.c_str(), kPayloadHash);
  CHECK(page != nullptr);

  ProbeApi api;
  SetTestOptions(&api);
  api.AddManifestEntry(
      ManifestEntry{name, Kind::kLatestSlot, static_cast<uint32_t>(sizeof(Msg)), 2, 7, "Msg", true});

  const uint64_t stamp = NowNs();
  const Msg m = MakeFrame(1, stamp, 0xAAAAu, 0xBBBBu, 0x12345678u);
  page->slot.Write(m);

  const uint64_t hash_before = HashShm(name);

  std::vector<RegionInfo> rows;
  CHECK(api.List(rows) == Status::kOk);
  const RegionInfo* row = Find(rows, name);
  CHECK(row != nullptr);
  CHECK(row->kind == Kind::kLatestSlot);
  CHECK(row->slots == 2);
  CHECK(row->slot_bytes == sizeof(Msg));
  CHECK(row->stride == Model::kCellStride);
  CHECK(row->head == 1);
  CHECK(row->abi_version == kLatestSlotPageAbiVersion);
  CHECK(row->region_bytes >= Model::kRegionBytes);
  CHECK(row->geometry_source == "page+manifest");

  RegionInfo info;
  CHECK(api.Info(name, info) == Status::kOk);
  CHECK(info.kind == Kind::kLatestSlot);
  CHECK(info.slot_bytes == sizeof(Msg));
  CHECK(info.stride == Model::kCellStride);

  Sample s;
  CHECK(api.Newest(name, &s) == Status::kOk);
  CHECK(s.sequence == 1);
  CHECK(s.producer_ns == stamp);
  CHECK(s.size == sizeof(Msg));

  // The seqlock was not advanced and no writer state moved: byte-for-byte
  // identity of the whole page.
  CHECK(HashShm(name) == hash_before);

  // A consumer still sees the value (probe did not "consume" it).
  uint64_t seen = 0;
  Msg out{};
  CHECK(page->slot.ReadIfNew(seen, out));
  CHECK(out.payload.a == 0xAAAAu && out.payload.b == 0xBBBBu);

  // delay = age at observation. The only write is `stamp`; sleep first so the
  // age is comfortably above the sampling period.
  SleepNs(20 * kMs);
  LatencyStats first;
  CHECK(api.Delay(name, 60 * kMs, 5 * kMs, &first) == Status::kOk);
  CHECK(first.count > 0);
  CHECK(first.stamp_source == "frame_header");
  CHECK(first.min_ns >= 15 * kMs);
  CHECK(first.p50_ns <= first.p90_ns);
  CHECK(first.p90_ns <= first.p99_ns);
  CHECK(first.p99_ns <= first.max_ns);
  CHECK(HistogramTotal(first) == first.count);

  SleepNs(30 * kMs);
  LatencyStats second;
  CHECK(api.Delay(name, 60 * kMs, 5 * kMs, &second) == Status::kOk);
  // Monotonic input (no new writes) => strictly larger ages.
  CHECK(second.p50_ns > first.p50_ns);

  // Echo decodes the FrameHeader and returns the exact bytes.
  std::vector<EchoSample> echoes;
  CHECK(api.Echo(name, 1, 64, 500 * kMs, &echoes) == Status::kOk);
  CHECK(echoes.size() == 1);
  CHECK(echoes[0].has_header);
  CHECK(echoes[0].header.sequence == 1);
  CHECK(echoes[0].header.mono_ns == stamp);
  CHECK(echoes[0].size == sizeof(Msg));
  CHECK(echoes[0].bytes.size() == sizeof(Msg));
  CHECK(std::memcmp(echoes[0].bytes.data(), &m, sizeof(Msg)) == 0);
  CHECK(echoes[0].struct_id == 7);
  CHECK(echoes[0].struct_name == "Msg");

  CHECK(HashShm(name) == hash_before);
  ShmRegion::UnlinkName(name.c_str());
}

void TestFrameHeaderMetadata() {
  const std::string name = Unique("frame_metadata");
  ShmRegion::UnlinkName(name.c_str());
  ShmRegion region;
  auto* page = CreateLatestSlotPage<Msg>(region, name.c_str(), 0x1234);
  CHECK(page != nullptr);

  ProbeApi api;
  SetTestOptions(&api);
  api.AddManifestEntry(ManifestEntry{name, Kind::kLatestSlot,
                                     static_cast<uint32_t>(sizeof(Msg)), 2, 7,
                                     "Msg", true});
  const uint64_t stamp = NowNs();
  Sample newest;
  std::vector<EchoSample> echoes;
  LatencyStats delay;

  page->slot.Write(MakeFrame(1, stamp, 1, 2, 0x12345678u));
  CHECK(api.Newest(name, &newest) == Status::kOk);
  CHECK(newest.producer_ns == stamp);
  CHECK(api.Echo(name, 1, sizeof(Msg), 0, &echoes) == Status::kOk);
  CHECK(echoes.size() == 1);
  CHECK(echoes[0].has_header);
  CHECK(echoes[0].header.valid == 1);

  // Metadata inspection accepts a structurally sound unfinished frame even
  // though its old CRC no longer matches; valid == 0 still forbids payload use.
  Msg pending = MakeFrame(2, stamp, 3, 4, 0x12345678u);
  pending.hdr.valid = 0;
  page->slot.Write(pending);
  CHECK(api.Newest(name, &newest) == Status::kOk);
  CHECK(newest.producer_ns == stamp);
  CHECK(api.Echo(name, 1, sizeof(Msg), 0, &echoes) == Status::kOk);
  CHECK(echoes.size() == 1);
  CHECK(echoes[0].has_header);
  CHECK(echoes[0].header.valid == 0);

  Msg bad_reserved = MakeFrame(3, stamp, 5, 6, 0x12345678u);
  bad_reserved.hdr.reserved[0] = 0x80;
  page->slot.Write(bad_reserved);
  CHECK(api.Newest(name, &newest) == Status::kOk);
  CHECK(newest.producer_ns == 0);  // RawSample::has_stamp is false.
  CHECK(api.Delay(name, 0, kMs, &delay) == Status::kNoData);
  CHECK(api.Echo(name, 1, sizeof(Msg), 0, &echoes) == Status::kOk);
  CHECK(echoes.size() == 1);
  CHECK(!echoes[0].has_header);
  CHECK(echoes[0].producer_ns == 0);

  Msg bad_valid = MakeFrame(4, stamp, 7, 8, 0x12345678u);
  bad_valid.hdr.valid = 2;
  page->slot.Write(bad_valid);
  CHECK(api.Newest(name, &newest) == Status::kOk);
  CHECK(newest.producer_ns == 0);  // RawSample::has_stamp is false.
  CHECK(api.Delay(name, 0, kMs, &delay) == Status::kNoData);
  CHECK(api.Echo(name, 1, sizeof(Msg), 0, &echoes) == Status::kOk);
  CHECK(echoes.size() == 1);
  CHECK(!echoes[0].has_header);
  CHECK(echoes[0].producer_ns == 0);

  ShmRegion::UnlinkName(name.c_str());
}

void TestSpscRing() {
  const std::string name = Unique("spsc");
  ShmRegion::UnlinkName(name.c_str());
  ShmSpscRing<Msg, 256> ring;
  CHECK(ring.Create(name.c_str()));
  using RingLayout = ShmSpscRing<Msg, 256>::Layout;

  ProbeApi api;
  SetTestOptions(&api);
  api.AddManifestEntry(
      ManifestEntry{name, Kind::kSpscRing, static_cast<uint32_t>(sizeof(Msg)), 256, 9, "Msg", true});

  std::atomic<bool> stop{false};
  std::thread producer([&]() {
    uint32_t seq = 0;
    while (!stop.load(std::memory_order_acquire)) {
      const Msg f = MakeFrame(seq, NowNs(), seq, seq * 3u, 0x22334455u);
      (void)ring.Push(f);
      ++seq;
      SleepNs(10 * kMs);  // ~100 Hz
    }
  });
  SleepNs(60 * kMs);

  RegionInfo info;
  CHECK(api.Info(name, info) == Status::kOk);
  CHECK(info.kind == Kind::kSpscRing);
  CHECK(info.slots == 256);
  CHECK(info.slot_bytes == sizeof(Msg));
  CHECK(info.stride == 64);
  CHECK(info.head > 0);
  CHECK(info.region_bytes > 0);
  CHECK(info.tail == 0);  // nobody consumed

  RateStats hz;
  CHECK(api.Hz(name, 300 * kMs, 5 * kMs, &hz) == Status::kOk);
  CHECK(hz.hz_avg > 30.0);
  CHECK(hz.hz_avg < 300.0);
  CHECK(hz.published >= 3);

  RateStats bw;
  CHECK(api.Bw(name, 300 * kMs, 5 * kMs, false, &bw) == Status::kOk);
  CHECK(bw.bytes_per_s > 0.0);
  CHECK(bw.bytes_per_s_min <= bw.bytes_per_s);
  CHECK(bw.bytes_per_s <= bw.bytes_per_s_max);

  int seen = 0;
  double last_rate = 0.0;
  SampleOptions so;
  so.count = 3;
  so.period_ns = 5 * kMs;
  so.max_duration_ns = 2000 * kMs;
  CHECK(api.Watch(name, so,
                  [&](const Sample& sample) {
                    ++seen;
                    if (seen > 1) last_rate = sample.rate_hz;
                    return true;
                  }) == Status::kOk);
  CHECK(seen == 3);
  CHECK(last_rate > 0.0);

  LatencyStats lat;
  CHECK(api.Delay(name, 100 * kMs, 5 * kMs, &lat) == Status::kOk);
  CHECK(lat.count > 0);
  CHECK(lat.stamp_source == "frame_header");
  CHECK(lat.p50_ns <= lat.p99_ns);
  CHECK(lat.p99_ns <= lat.max_ns);
  CHECK(HistogramTotal(lat) == lat.count);

  std::vector<EchoSample> echoes;
  CHECK(api.Echo(name, 1, 64, 1000 * kMs, &echoes) == Status::kOk);
  CHECK(echoes.size() == 1);
  CHECK(echoes[0].has_header);
  CHECK(echoes[0].size == sizeof(Msg));

  stop.store(true, std::memory_order_release);
  producer.join();

  const uint64_t head = RawU32(name, offsetof(RingLayout, head));
  const uint64_t tail = RawU32(name, offsetof(RingLayout, tail));
  CHECK(head > 0);
  CHECK(tail == 0);  // probe never popped
  const uint64_t hash_before = HashShm(name);

  // All probe operations again on a quiet page: the shared cursor and every
  // byte must stay put.
  RegionInfo info2;
  CHECK(api.Info(name, info2) == Status::kOk);
  CHECK(info2.drop_count == ring.push_fail_count());
  Sample s;
  CHECK(api.Newest(name, &s) == Status::kOk);
  std::vector<EchoSample> echoes2;
  CHECK(api.Echo(name, 1, 64, 50 * kMs, &echoes2) == Status::kOk);
  CHECK(echoes2[0].drop_count == ring.push_fail_count());
  LatencyStats lat2;
  (void)api.Delay(name, 30 * kMs, 5 * kMs, &lat2);
  CHECK(HashShm(name) == hash_before);
  CHECK(RawU32(name, offsetof(RingLayout, tail)) == 0);

  // A real consumer can still drain every frame the producer pushed.
  uint64_t popped = 0;
  Msg got{};
  while (ring.TryPop(got)) ++popped;
  CHECK(popped == head);
  ShmRegion::UnlinkName(name.c_str());
}

void TestSpmcRing() {
  const std::string name = Unique("spmc");
  ShmRegion::UnlinkName(name.c_str());
  ShmSpmcRing<Msg, 32> ring;
  CHECK(ring.Create(name.c_str()));
  for (uint32_t i = 0; i < 3; ++i) {
    ring.Push(MakeFrame(i, NowNs(), i, i + 100u, 0x33445566u));
  }

  ProbeApi api;
  SetTestOptions(&api);
  api.AddManifestEntry(
      ManifestEntry{name, Kind::kSpmcRing, static_cast<uint32_t>(sizeof(Msg)), 32, 10, "Msg", true});

  const uint64_t hash_before = HashShm(name);
  std::vector<RegionInfo> rows;
  CHECK(api.List(rows) == Status::kOk);
  const RegionInfo* row = Find(rows, name);
  CHECK(row != nullptr);
  CHECK(row->kind == Kind::kSpmcRing);
  CHECK(row->slots == 32);
  CHECK(row->slot_bytes == sizeof(Msg));
  CHECK(row->stride == 64);  // seq(8) + 44 rounded to 64
  CHECK(row->head == 3);
  CHECK(row->drop_count == 0);

  Sample s;
  CHECK(api.Newest(name, &s) == Status::kOk);
  CHECK(s.sequence == 3);

  LatencyStats lat;
  CHECK(api.Delay(name, 40 * kMs, 5 * kMs, &lat) == Status::kOk);
  CHECK(lat.count > 0);
  CHECK(lat.p50_ns <= lat.p99_ns);

  std::vector<EchoSample> echoes;
  CHECK(api.Echo(name, 1, 64, 100 * kMs, &echoes) == Status::kOk);
  CHECK(echoes.size() == 1);
  CHECK(echoes[0].has_header);
  CHECK(echoes[0].drop_count == 0);

  CHECK(HashShm(name) == hash_before);

  // Readers (per-reader cursors are process-local) can still follow all 3.
  SpmcReader<Msg, 32> reader;
  CHECK(reader.Bind(&ring, SpmcRead::Follow));
  uint32_t n = 0;
  Msg got{};
  while (reader.TryPop(got)) ++n;
  CHECK(n == 3);

  PushResult result{};
  for (uint32_t i = 0; i < 33; ++i) {
    result = ring.PushWithResult(MakeFrame(i, NowNs(), i, i, 0x33445566u));
    CHECK(result.ok);
  }
  CHECK(result.overwritten == 1);
  uint64_t dropped = 0;
  CHECK(reader.TryPop(got, &dropped));
  CHECK(dropped == 1);
  RegionInfo spmc_info;
  CHECK(api.Info(name, spmc_info) == Status::kOk);
  CHECK(spmc_info.drop_count == 1);
  CHECK(spmc_info.reader_drop_slots.size() == kAbiMaxSpmcReaders);
  CHECK(spmc_info.reader_drop_slots[reader.reader_slot()] == 1);
  CHECK(api.Echo(name, 1, 64, 100 * kMs, &echoes) == Status::kOk);
  CHECK(echoes[0].drop_count == 1);

  const std::string health_name = Unique("health");
  ShmRegion::UnlinkName(health_name.c_str());
  ShmRegion health_region;
  CHECK(health_region.Create(health_name.c_str(), sizeof(SharedHealthPage)));
  auto* health = static_cast<SharedHealthPage*>(health_region.data());
  CHECK(health->Init(7, 2));
  uint64_t seen = 0;
  CreditSpmcReaderDrops(reader, &health->slots[0], seen);
  CHECK(health->slots[0].drop_cnt.load() == 1);
  health->slots[1].AddDrops(2);
  health->slots[0].SetMemoryFlags(kMemoryMlockDisabled);
  health->slots[1].SetMemoryFlags(kMemoryMlockFailed | kMemoryMalloptFailed);
  const uint64_t health_hash = HashShm(health_name);

  RegionInfo health_info;
  CHECK(api.Info(health_name, health_info) == Status::kOk);
  CHECK(health_info.kind == Kind::kSharedHealth);
  CHECK(health_info.drop_count == 3);
  CHECK(api.Echo(health_name, 2, sizeof(HealthSlot), 0, &echoes) == Status::kOk);
  CHECK(echoes.size() == 2);
  CHECK(echoes[0].has_worker && echoes[0].worker == 0);
  CHECK(echoes[0].drop_count == 1);
  CHECK(echoes[0].has_memory_flags);
  CHECK(echoes[0].memory_flags == kMemoryMlockDisabled);
  CHECK(std::string(MemoryHealthStateName(echoes[0].memory_flags)) == "disabled");
  CHECK(echoes[0].bytes.size() == sizeof(HealthSlot));
  CHECK(echoes[1].has_worker && echoes[1].worker == 1);
  CHECK(echoes[1].drop_count == 2);
  CHECK(echoes[1].has_memory_flags);
  CHECK(echoes[1].memory_flags == (kMemoryMlockFailed | kMemoryMalloptFailed));
  CHECK(std::string(MemoryHealthStateName(echoes[1].memory_flags)) == "degraded");
  CHECK(HashShm(health_name) == health_hash);

  // ARCH-11: MpControl is self-describing now (magic "THYME_MP" + super
  // header). Info validates the full page description and must not write it.
  const std::string ctrl_name = Unique("mpctl");
  ShmRegion::UnlinkName(ctrl_name.c_str());
  ShmRegion ctrl_region;
  CHECK(ctrl_region.CreateDescribed(ctrl_name.c_str(),
                                    app::MpControl::Spec()));
  auto* ctrl = static_cast<app::MpControl*>(ctrl_region.data());
  ctrl->Init();
  ctrl->proc_loops[0].store(42, std::memory_order_release);
  const uint64_t ctrl_hash = HashShm(ctrl_name);
  RegionInfo ctrl_info;
  CHECK(api.Info(ctrl_name, ctrl_info) == Status::kOk);
  CHECK(ctrl_info.kind == Kind::kMpControl);
  CHECK(ctrl_info.abi_version == app::MpControl::kMpControlAbiVersion);
  CHECK(ctrl_info.slots == app::MpControl::kMaxProcSlots);
  CHECK(ctrl_info.compatible);
  CHECK(HashShm(ctrl_name) == ctrl_hash);
  ShmRegion::UnlinkName(ctrl_name.c_str());

  ShmRegion::UnlinkName(health_name.c_str());
  ShmRegion::UnlinkName(name.c_str());
}

void TestPinnedSlot() {
  const std::string name = Unique("big");
  ShmRegion::UnlinkName(name.c_str());
  ShmRegion::UnlinkName((name + ".lease").c_str());
  PinnedSlotWriter writer;
  CHECK(writer.Create(name.c_str(), 128, 64, 0));

  constexpr uint32_t kFrames = 5;
  constexpr uint32_t kLiveFrames = 35;
  auto publish = [&writer](uint32_t index) {
    PinnedSlotWriter::BeginView v = writer.Begin(64);
    CHECK(v.payload != nullptr);
    for (uint32_t j = 0; j < 64; ++j) {
      v.payload[j] = static_cast<uint8_t>(index * 31u + j);
    }
    writer.Commit(NowNs());
  };
  for (uint32_t i = 0; i < kFrames; ++i) publish(i);

  ProbeApi api;
  SetTestOptions(&api);  // PinnedSlot is self-describing; no manifest needed
  std::vector<RegionInfo> rows;
  CHECK(api.List(rows) == Status::kOk);
  const RegionInfo* row = Find(rows, name);
  CHECK(row != nullptr);
  CHECK(row->kind == Kind::kPinnedSlot);
  CHECK(row->slots == 64);
  CHECK(row->slot_bytes == 128);
  CHECK(row->stride == PinnedSlotWriter::SlotStride(128));
  CHECK(row->head == kFrames);
  CHECK(row->region_bytes > 0);
  CHECK(row->drop_count == 0);
  CHECK(row->skip_count == 0);
  CHECK(row->steal_count == 0);

  RegionInfo info;
  CHECK(api.Info(name, info) == Status::kOk);
  CHECK(info.kind == Kind::kPinnedSlot);
  CHECK(info.slot_bytes == 128);

  LatencyStats lat;
  CHECK(api.Delay(name, 60 * kMs, 5 * kMs, &lat) == Status::kOk);
  CHECK(lat.count > 0);
  CHECK(lat.stamp_source == "pinned_slot_slot");
  CHECK(lat.p50_ns <= lat.p90_ns);
  CHECK(lat.p90_ns <= lat.p99_ns);
  CHECK(lat.p99_ns <= lat.max_ns);
  CHECK(HistogramTotal(lat) == lat.count);

  std::vector<EchoSample> echoes;
  CHECK(api.Echo(name, 1, 64, 500 * kMs, &echoes) == Status::kOk);
  CHECK(echoes.size() == 1);
  CHECK(echoes[0].size == 64);
  CHECK(!echoes[0].has_header);  // raw PinnedSlot payload
  CHECK(echoes[0].bytes.size() == 64);
  const uint32_t frame_index = static_cast<uint32_t>(echoes[0].sequence - 1);
  CHECK(echoes[0].bytes[0] == static_cast<uint8_t>(frame_index * 31u));
  CHECK(echoes[0].bytes[63] == static_cast<uint8_t>(frame_index * 31u + 63u));

  // Rate/bandwidth need publishes inside the sampling window.
  std::thread producer([&]() {
    for (uint32_t i = kFrames; i < kFrames + kLiveFrames; ++i) {
      publish(i);
      SleepNs(20 * kMs);  // ~50 Hz
    }
  });
  RateStats hz;
  CHECK(api.Hz(name, 300 * kMs, 5 * kMs, &hz) == Status::kOk);
  CHECK(hz.published > 3);
  CHECK(hz.hz_avg > 10.0);
  CHECK(hz.hz_avg < 300.0);

  RateStats bw;
  CHECK(api.Bw(name, 300 * kMs, 5 * kMs, true, &bw) == Status::kOk);
  CHECK(bw.exact_bytes);
  CHECK(bw.bytes_per_s > 0.0);
  CHECK(bw.published > 3);
  CHECK(bw.jitter_p50_ns <= bw.jitter_p99_ns);
  producer.join();

  const uint64_t total_frames = kFrames + kLiveFrames;
  CHECK(writer.head() == total_frames);
  CHECK(RawU64(name, offsetof(PinnedSlotPageHeader, head)) == total_frames);

  // Read-only on a quiet page: same bytes, no pins, no skipped frames.
  const uint64_t hash_before = HashShm(name);
  RegionInfo info2;
  CHECK(api.Info(name, info2) == Status::kOk);
  Sample s;
  CHECK(api.Newest(name, &s) == Status::kOk);
  CHECK(s.sequence == total_frames);
  std::vector<EchoSample> echoes2;
  CHECK(api.Echo(name, 1, 64, 50 * kMs, &echoes2) == Status::kOk);
  LatencyStats lat2;
  CHECK(api.Delay(name, 30 * kMs, 5 * kMs, &lat2) == Status::kOk);
  CHECK(HashShm(name) == hash_before);

  // A read-only reader can still consume every frame (probe did not pin or
  // advance anything shared).
  PinnedSlotReader reader;
  CHECK(reader.Open(name.c_str(), 100 * kMs, /*lease_read_only=*/true));
  uint32_t n = 0;
  PinnedSlotReader::View v;
  while (reader.TryRead(v)) ++n;
  CHECK(n == total_frames);
  CHECK(writer.head() == total_frames);
  CHECK(writer.pinned_skip_count() == 0);
  CHECK(writer.lease() != nullptr);
  CHECK(writer.lease()->lease_steal.load(std::memory_order_relaxed) == 0);

  ShmRegion::UnlinkName(name.c_str());
  ShmRegion::UnlinkName((name + ".lease").c_str());
}

void TestNotFoundAndBadGeometry() {
  ProbeApi api;
  SetTestOptions(&api);
  const std::string missing = Unique("does_not_exist");
  RegionInfo info;
  CHECK(api.Info(missing, info) == Status::kNotFound);
  Sample s;
  CHECK(api.Newest(missing, &s) == Status::kNotFound);
  RateStats rs;
  CHECK(api.Hz(missing, 20 * kMs, 5 * kMs, &rs) == Status::kNotFound);
  std::vector<EchoSample> es;
  CHECK(api.Echo(missing, 1, 64, 20 * kMs, &es) == Status::kNotFound);

  // A page with a known magic but an unsupported version is reported as
  // bad geometry (exit code 3 at the CLI), not "not found".
  const std::string bad = Unique("badver");
  ShmRegion::UnlinkName(bad.c_str());
  ShmRegion region;
  CHECK(region.Create(bad.c_str(), 4096));
  std::memset(region.data(), 0, 4096);
  auto* hdr = static_cast<PinnedSlotPageHeader*>(region.data());
  hdr->super.abi_version.store(99, std::memory_order_relaxed);
  hdr->super.magic.store(PinnedSlotPageHeader::kMagic,
                         std::memory_order_release);
  CHECK(api.Info(bad, info) == Status::kBadGeometry);
  CHECK(info.kind == Kind::kPinnedSlot);
  CHECK(info.abi_version == 99);
  CHECK(!info.compatible);
  CHECK(!info.note.empty());
  CHECK(api.Newest(bad, &s) == Status::kBadGeometry);

  // Too small for the header is impossible in a file-backed shm object
  // (ftruncate rounds to a page), so the second negative case is an
  // unsupported version with a valid magic.
  const std::string old = Unique("oldver");
  ShmRegion::UnlinkName(old.c_str());
  ShmRegion legacy;
  CHECK(legacy.Create(old.c_str(), 4096));
  std::memset(legacy.data(), 0, 4096);
  auto* oh = static_cast<PinnedSlotPageHeader*>(legacy.data());
  oh->super.abi_version.store(1, std::memory_order_relaxed);
  oh->super.magic.store(PinnedSlotPageHeader::kMagic,
                        std::memory_order_release);
  CHECK(api.Info(old, info) == Status::kBadGeometry);
  CHECK(info.abi_version == 1);

  // ARCH-11 migration: a v2 SharedHealthPage image is still classified by its
  // magic but rejected by the super-header version check (no dual-read). A v2
  // MpControl has no magic, so the probe cannot classify it at all.
  const std::string old_health = Unique("old_health_v2");
  ShmRegion::UnlinkName(old_health.c_str());
  ShmRegion old_health_region;
  CHECK(old_health_region.Create(old_health.c_str(), 4160));
  std::memset(old_health_region.data(), 0, 4160);
  {
    auto* bytes = static_cast<uint8_t*>(old_health_region.data());
    const uint64_t magic = SharedHealthPage::kMagic;
    const uint32_t version = 2;
    const uint32_t session = static_cast<uint32_t>(getpid());
    const uint32_t workers = 2;
    std::memcpy(bytes + 0, &magic, sizeof(magic));
    std::memcpy(bytes + 8, &version, sizeof(version));
    std::memcpy(bytes + 12, &session, sizeof(session));
    std::memcpy(bytes + 16, &workers, sizeof(workers));
  }
  CHECK(api.Info(old_health, info) == Status::kBadGeometry);
  CHECK(info.kind == Kind::kSharedHealth);
  CHECK(info.abi_version == 2);
  CHECK(!info.compatible);
  CHECK(info.note.find("bad_abi_version") != std::string::npos);
  ShmRegion::UnlinkName(old_health.c_str());

  const std::string old_ctrl = Unique("old_control_v2");
  ShmRegion::UnlinkName(old_ctrl.c_str());
  ShmRegion old_ctrl_region;
  CHECK(old_ctrl_region.Create(old_ctrl.c_str(), 232));
  std::memset(old_ctrl_region.data(), 0, 232);
  {
    const uint32_t version = 2;
    std::memcpy(old_ctrl_region.data(), &version, sizeof(version));
  }
  CHECK(api.Info(old_ctrl, info) == Status::kNotFound);
  ShmRegion::UnlinkName(old_ctrl.c_str());

  // A LatestSlot page without a manifest entry is not guessable (its page has
  // no magic/version at all).
  const std::string naked = Unique("naked");
  ShmRegion::UnlinkName(naked.c_str());
  ShmRegion slot;
  CHECK(slot.Create<LatestSlot<Msg>>(naked.c_str(), 4096) != nullptr);
  CHECK(api.Info(naked, info) == Status::kNotFound);
  // Declared with an impossible slot size => bad geometry, not a crash.
  api.AddManifestEntry(
      ManifestEntry{naked, Kind::kLatestSlot, 1u << 20, 2, 0, "", false});
  CHECK(api.Info(naked, info) == Status::kBadGeometry);
  CHECK(!info.note.empty());

  ShmRegion::UnlinkName(bad.c_str());
  ShmRegion::UnlinkName(old.c_str());
  ShmRegion::UnlinkName(naked.c_str());
}

void TestListSkipsLeaseCompanion() {
  const std::string name = Unique("lease_pair");
  ShmRegion::UnlinkName(name.c_str());
  PinnedSlotWriter writer;
  CHECK(writer.Create(name.c_str(), 64, 4, 0));
  ProbeApi api;
  SetTestOptions(&api);
  std::vector<RegionInfo> rows;
  (void)api.List(rows);
  for (const auto& r : rows) {
    CHECK(r.name.find(".lease") == std::string::npos);
  }
  CHECK(Find(rows, name) != nullptr);
  ShmRegion::UnlinkName(name.c_str());
  ShmRegion::UnlinkName((name + ".lease").c_str());
}

}  // namespace

int main() {
  TestLatestSlot();
  TestFrameHeaderMetadata();
  TestSpscRing();
  TestSpmcRing();
  TestPinnedSlot();
  TestNotFoundAndBadGeometry();
  TestListSkipsLeaseCompanion();
  std::printf("test_thyme_probe: all checks passed\n");
  return 0;
}
