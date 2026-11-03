// test_replay_reader.cpp — REP-01: package-level replay reader with a unified
// mono_ns timeline.
//
// The fixture records a real trigger package in-process (RecordMirrorRing +
// PinnedSlotStore producer, TriggeredRecorder as the engine) with meta.tbin,
// small.tbin and one frames/<channel> pair, then verifies:
//   A: discovery/channel table, merged order monotonic in mono_ns, per-channel
//      record counts identical to the individual .tbin files, per-channel
//      data sequences in file order, payload/CRC integrity;
//   B: SeekMonoNs lands on the first event with ts >= target and the remaining
//      sequence equals the suffix of a full scan (indexed and unindexed);
//   C: missing/truncated/corrupt/orphan files produce explicit statuses.
#include "thyme/check.h"
#include "thyme/contracts/frame_header.h"
#include "thyme/record/record_mirror_ring_client.h"
#include "thyme/record/thyme_bin.h"
#include "thyme/record/triggered_recorder.h"
#include "thyme/replay/replay_reader.h"
#include "thyme/runtime/pinned_slot_store.h"
#include "thyme/runtime/record_mirror_ring.h"
#include "thyme/runtime/shm_region.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <fcntl.h>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

using namespace thyme;

namespace {

constexpr uint64_t kPaceNs = 4000000;       // 4 ms between producer samples
constexpr uint64_t kBeforeNs = 100000000;   // 100 ms pre window
constexpr uint64_t kAfterNs = 200000000;    // 200 ms post window
constexpr uint64_t kTriggerIter = 30;       // 120 ms of history before t0
constexpr uint64_t kTotalIters = 90;        // 240 ms after t0
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

bool CopyFile(const std::string& src, const std::string& dst) {
  std::FILE* in = std::fopen(src.c_str(), "rb");
  if (in == nullptr) {
    return false;
  }
  std::FILE* out = std::fopen(dst.c_str(), "wb");
  if (out == nullptr) {
    std::fclose(in);
    return false;
  }
  uint8_t buf[65536];
  std::size_t n = 0;
  bool ok = true;
  while ((n = std::fread(buf, 1, sizeof(buf), in)) != 0) {
    if (std::fwrite(buf, 1, n, out) != n) {
      ok = false;
      break;
    }
  }
  std::fclose(in);
  ok = std::fclose(out) == 0 && ok;
  return ok;
}

void CopyTree(const std::string& src, const std::string& dst) {
  CHECK(mkdir(dst.c_str(), 0700) == 0);
  DIR* d = opendir(src.c_str());
  CHECK(d != nullptr);
  while (dirent* e = readdir(d)) {
    if (e->d_name[0] == '.') {
      continue;
    }
    const std::string s = src + "/" + e->d_name;
    const std::string t = dst + "/" + e->d_name;
    struct stat st {};
    CHECK(stat(s.c_str(), &st) == 0);
    if (S_ISDIR(st.st_mode)) {
      CopyTree(s, t);
    } else {
      CHECK(CopyFile(s, t));
    }
  }
  closedir(d);
}

struct TbinCounts {
  uint64_t total = 0;
  uint64_t data = 0;
  uint64_t gaps = 0;
};

TbinCounts CountTbin(const std::string& path) {
  ThymeBinReader rd(path.c_str());
  CHECK(rd.ok());
  TbinCounts c;
  ThymeBinDataEntry e;
  while (rd.ReadNext(e) == ThymeBinReader::ReadResult::kOk) {
    c.total += 1;
    if (e.type == ThymeBinEntryType::kData) {
      c.data += 1;
    }
    if (e.type == ThymeBinEntryType::kGap) {
      c.gaps += 1;
    }
  }
  return c;
}

// Records one package into `root` and leaves it on disk for the readers.
struct Fixture {
  std::string root;
  std::string out_dir;
  std::string page_name;
  std::string chan_name;
  std::string aux_name;
  std::string pkg_dir;
  record_mirror::RingClient mirror;
  PinnedSlotWriter frames;
  PinnedSlotWriter frames_aux;
  triggered::TriggeredRecorder rec;
  uint64_t next_frame = 0;
  bool shutdown_done = false;

  void Init(const std::string& tag) {
    char buf[160];
    const long me = static_cast<long>(getpid());
    std::snprintf(buf, sizeof(buf), "/tmp/thyme_rep01_%s_%ld", tag.c_str(), me);
    root = buf;
    RmTree(root);
    for (const std::string& p : {root, root + "/out"}) {
      CHECK(mkdir(p.c_str(), 0700) == 0);
    }
    std::snprintf(buf, sizeof(buf), "/thyme_rep01_%s_p_%ld", tag.c_str(), me);
    page_name = buf;
    std::snprintf(buf, sizeof(buf), "/thyme_rep01_%s_c_%ld", tag.c_str(), me);
    chan_name = buf;
    std::snprintf(buf, sizeof(buf), "/thyme_rep01_%s_a_%ld", tag.c_str(), me);
    aux_name = buf;
    ShmRegion::UnlinkName(page_name.c_str());
    ShmRegion::UnlinkName(chan_name.c_str());
    ShmRegion::UnlinkName(aux_name.c_str());
    out_dir = root + "/out";

    ThymeStructDesc desc{};
    desc.struct_id = 0;
    desc.contract_version = 1;
    desc.payload_size = sizeof(Pod16);
    std::snprintf(desc.name, sizeof(desc.name), "Pod16");
    RecordMirrorRingSpec spec{sizeof(Pod16), 256};
    CHECK(mirror.Create(page_name.c_str(), &desc, &spec, 1, 7));
    CHECK(frames.Create(chan_name.c_str(), kFrameSlotBytes, kFrameSlots, 0));
    CHECK(frames_aux.Create(aux_name.c_str(), kFrameSlotBytes, kFrameSlots, 0));

    triggered::Config cfg;
    cfg.out_dir = out_dir;
    triggered::SourceConfig mirror_src;
    mirror_src.kind = triggered::SourceConfig::Kind::kMirrorRing;
    mirror_src.shm_name = page_name;
    triggered::SourceConfig frame_src;
    frame_src.kind = triggered::SourceConfig::Kind::kPinnedSlot;
    frame_src.shm_name = chan_name;
    frame_src.channel_name = "cloud_1";
    triggered::SourceConfig aux_src;
    aux_src.kind = triggered::SourceConfig::Kind::kPinnedSlot;
    aux_src.shm_name = aux_name;
    aux_src.channel_name = "aux_0";
    cfg.sources = {mirror_src, frame_src, aux_src};
    CHECK(rec.Init(cfg));
  }

  void Publish() {
    Pod16 p{};
    p.a = next_frame;
    p.b = static_cast<uint32_t>(next_frame * 2);
    p.c = static_cast<uint32_t>(next_frame * 3);
    CHECK(mirror.Page()->TryWriteRing(0, &p, sizeof(p), NowNs(),
                                      static_cast<uint32_t>(next_frame)));
    const uint32_t len = FrameLen(next_frame);
    PinnedSlotWriter::BeginView bv = frames.Begin(len);
    CHECK(bv.payload != nullptr);
    FillFrame(bv.payload, len, next_frame);
    bv.header->flags.store(next_frame % 4 == 0 ? 1u : 0u,
                           std::memory_order_relaxed);
    frames.Commit(NowNs());
    PinnedSlotWriter::BeginView aux = frames_aux.Begin(len);
    CHECK(aux.payload != nullptr);
    FillFrame(aux.payload, len, next_frame + 1000);
    frames_aux.Commit(NowNs());
    next_frame += 1;
  }

  bool Record() {
    for (uint64_t i = 0; i < kTriggerIter; ++i) {
      Publish();
      rec.Tick();
      SleepNs(kPaceNs);
    }
    CHECK(rec.Trigger(1, kBeforeNs, kAfterNs));
    for (uint64_t i = kTriggerIter; i < kTotalIters; ++i) {
      Publish();
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
    if (shutdown_done) {
      return;
    }
    shutdown_done = true;
    rec.Shutdown();
    frames.Close();
    frames_aux.Close();
    mirror.Shutdown(true);
  }

  void Cleanup() {
    Shutdown();
    RmTree(root);
  }
};

struct Snap {
  uint64_t mono_ns = 0;
  uint32_t channel = 0;
  replay::ReplayKind kind = replay::ReplayKind::kData;
  uint32_t struct_id = 0;
  uint64_t seq = 0;
  uint32_t len = 0;
  uint64_t raw_offset = 0;

  bool operator==(const Snap& o) const {
    return mono_ns == o.mono_ns && channel == o.channel && kind == o.kind &&
           struct_id == o.struct_id && seq == o.seq && len == o.len &&
           raw_offset == o.raw_offset;
  }
};

std::vector<Snap> Collect(replay::ReplayPackage& pkg) {  std::vector<Snap> out;
  replay::ReplayEvent ev;
  replay::ReplayStatus st = replay::ReplayStatus::kOk;
  while ((st = pkg.NextEvent(&ev)) == replay::ReplayStatus::kOk) {
    Snap s;
    s.mono_ns = ev.mono_ns;
    s.channel = ev.channel;
    s.kind = ev.kind;
    s.struct_id = ev.struct_id;
    s.seq = ev.record_sequence;
    s.len = ev.len;
    s.raw_offset = ev.raw_offset;
    out.push_back(s);
  }
  CHECK(st == replay::ReplayStatus::kEnd);
  return out;
}

const replay::ReplayChannel* ChannelByName(const replay::ReplayPackage& pkg,
                                           const std::string& name) {
  for (const replay::ReplayChannel& c : pkg.channels()) {
    if (c.name == name) {
      return &c;
    }
  }
  return nullptr;
}

// Hand-built package used to cover GAP/kSession entries (the recorder never
// emits a session row): an unindexed version-1 meta.tbin plus a sealed wal.
void WriteSyntheticPackage(const std::string& dir) {
  CHECK(mkdir(dir.c_str(), 0700) == 0);
  const std::string path = dir + "/meta.tbin";
  std::FILE* f = std::fopen(path.c_str(), "wb");
  CHECK(f != nullptr);
  ThymeBinFileHeader hdr{};
  std::memcpy(hdr.magic, kThymeBinMagic, sizeof(hdr.magic));
  hdr.file_abi_version = kThymeBinFileAbiVersionUnindexed;
  hdr.session_id = 77;
  hdr.struct_count = 1;
  CHECK(std::fwrite(&hdr, sizeof(hdr), 1, f) == 1);
  ThymeStructDesc desc{};
  desc.struct_id = 0;
  desc.contract_version = 1;
  desc.payload_size = sizeof(Pod16);
  std::snprintf(desc.name, sizeof(desc.name), "Pod16");
  const ThymeBinLegacyStructDesc legacy = ToThymeBinLegacyDesc(desc);
  CHECK(std::fwrite(&legacy, sizeof(legacy), 1, f) == 1);

  auto put = [f](const void* p, std::size_t n) {
    CHECK(std::fwrite(p, 1, n, f) == n);
  };
  auto put_type = [&put](ThymeBinEntryType t) {
    const uint8_t b[4] = {static_cast<uint8_t>(t), 0, 0, 0};
    put(b, sizeof(b));
  };
  auto put_crc = [&put](uint32_t c) { put(&c, sizeof(c)); };

  // kTrigger: id=9, mono=1000, pre=100, post=200.
  {
    const ThymeBinEntryType t = ThymeBinEntryType::kTrigger;
    put_type(t);
    const uint64_t id = 9;
    const uint64_t mono = 1000;
    const uint64_t pre = 100;
    const uint64_t post = 200;
    const uint8_t type_byte = static_cast<uint8_t>(t);
    uint32_t c = Crc32Update(0xFFFFFFFFu, &type_byte, 1);
    c = Crc32Update(c, reinterpret_cast<const uint8_t*>(&id), sizeof(id));
    c = Crc32Update(c, reinterpret_cast<const uint8_t*>(&mono), sizeof(mono));
    c = Crc32Update(c, reinterpret_cast<const uint8_t*>(&pre), sizeof(pre));
    c = Crc32Update(c, reinterpret_cast<const uint8_t*>(&post), sizeof(post));
    put(&id, sizeof(id));
    put(&mono, sizeof(mono));
    put(&pre, sizeof(pre));
    put(&post, sizeof(post));
    put_crc(c ^ 0xFFFFFFFFu);
  }
  // kGap: struct 0, reason 1 (overrun), mono 1100, missed 2.
  {
    const ThymeBinEntryType t = ThymeBinEntryType::kGap;
    put_type(t);
    const uint16_t sid = 0;
    const uint16_t reason = static_cast<uint16_t>(triggered::kGapReasonOverrun);
    const uint64_t mono = 1100;
    const uint32_t missed = 2;
    const uint8_t type_byte = static_cast<uint8_t>(t);
    uint32_t c = Crc32Update(0xFFFFFFFFu, &type_byte, 1);
    c = Crc32Update(c, reinterpret_cast<const uint8_t*>(&sid), sizeof(sid));
    c = Crc32Update(c, reinterpret_cast<const uint8_t*>(&reason),
                    sizeof(reason));
    c = Crc32Update(c, reinterpret_cast<const uint8_t*>(&mono), sizeof(mono));
    c = Crc32Update(c, reinterpret_cast<const uint8_t*>(&missed),
                    sizeof(missed));
    put(&sid, sizeof(sid));
    put(&reason, sizeof(reason));
    put(&mono, sizeof(mono));
    put(&missed, sizeof(missed));
    put_crc(c ^ 0xFFFFFFFFu);
  }
  // kData: struct 0, mono 1200, producer sequence 7.
  {
    Pod16 payload{12345, 111, 222};
    const ThymeBinEntryType t = ThymeBinEntryType::kData;
    put_type(t);
    const uint64_t seq = 0;
    const uint16_t sid = 0;
    const uint16_t reserved = 0;
    const uint64_t mono = 1200;
    const uint32_t sseq = 7;
    const uint32_t size = sizeof(payload);
    const uint8_t type_byte = static_cast<uint8_t>(t);
    uint32_t c = Crc32Update(0xFFFFFFFFu, &type_byte, 1);
    c = Crc32Update(c, reinterpret_cast<const uint8_t*>(&seq), sizeof(seq));
    c = Crc32Update(c, reinterpret_cast<const uint8_t*>(&sid), sizeof(sid));
    c = Crc32Update(c, reinterpret_cast<const uint8_t*>(&mono), sizeof(mono));
    c = Crc32Update(c, reinterpret_cast<const uint8_t*>(&sseq), sizeof(sseq));
    c = Crc32Update(c, reinterpret_cast<const uint8_t*>(&size), sizeof(size));
    c = Crc32Update(c, reinterpret_cast<const uint8_t*>(&payload), sizeof(payload));
    put(&seq, sizeof(seq));
    put(&sid, sizeof(sid));
    put(&reserved, sizeof(reserved));
    put(&mono, sizeof(mono));
    put(&sseq, sizeof(sseq));
    put(&size, sizeof(size));
    put(&payload, sizeof(payload));
    put_crc(c ^ 0xFFFFFFFFu);
  }
  // kSession at the file tail with no timestamp (the format has none): the
  // merge must still order it by mono_ns=0, independent of file order.
  {
    const ThymeBinEntryType t = ThymeBinEntryType::kSession;
    put_type(t);
    const uint64_t session_id = 1234;
    const uint16_t reason = 3;
    const uint16_t reserved = 0;
    const uint8_t type_byte = static_cast<uint8_t>(t);
    uint32_t c = Crc32Update(0xFFFFFFFFu, &type_byte, 1);
    c = Crc32Update(c, reinterpret_cast<const uint8_t*>(&session_id),
                    sizeof(session_id));
    c = Crc32Update(c, reinterpret_cast<const uint8_t*>(&reason),
                    sizeof(reason));
    put(&session_id, sizeof(session_id));
    put(&reason, sizeof(reason));
    put(&reserved, sizeof(reserved));
    put_crc(c ^ 0xFFFFFFFFu);
  }
  CHECK(std::fclose(f) == 0);

  f = std::fopen((dir + "/wal.log").c_str(), "wb");
  CHECK(f != nullptr);
  const triggered::WalHeader wh{{'T', 'R', 'I', 'G', 'W', 'A', 'L', '1'}, 77};
  CHECK(std::fwrite(&wh, sizeof(wh), 1, f) == 1);
  const triggered::WalRow seal{triggered::kWalOpSeal, 0, 0, 0, 0, 0};
  CHECK(std::fwrite(&seal, sizeof(seal), 1, f) == 1);
  CHECK(std::fclose(f) == 0);
}

// Asserts the merged stream is monotonic and every payload is intact.
void CheckStream(replay::ReplayPackage& pkg,
                 const std::vector<replay::ReplayChannel>& channels,
                 uint64_t expected_events) {
  uint64_t events = 0;
  uint64_t samples = 0;
  uint64_t frames = 0;
  uint64_t last_mono = 0;
  replay::ReplayEvent ev;
  while (pkg.NextEvent(&ev) == replay::ReplayStatus::kOk) {
    CHECK(events == 0 || ev.mono_ns >= last_mono);
    last_mono = ev.mono_ns;
    events += 1;
    CHECK(ev.channel < channels.size());
    const replay::ReplayChannel& ch = channels[ev.channel];
    if (ev.kind == replay::ReplayKind::kData &&
        ch.kind == replay::ReplayChannelKind::kSmall) {
      CHECK(ev.struct_id == 0);
      CHECK(ev.len == sizeof(Pod16));
      CHECK(ev.payload != nullptr);
      Pod16 p{};
      std::memcpy(&p, ev.payload, sizeof(p));
      CHECK(p.b == static_cast<uint32_t>(p.a * 2));
      CHECK(p.c == static_cast<uint32_t>(p.a * 3));
      samples += 1;
    } else if (ev.kind == replay::ReplayKind::kData &&
               ch.kind == replay::ReplayChannelKind::kPinnedSlot) {
      CHECK(ev.struct_id == triggered::kFrameIndexStructId);
      CHECK(ev.len % 256 == 0);
      CHECK(ev.len >= 512 && ev.len <= 2304);
      CHECK(ev.payload != nullptr);
      CHECK(ev.payload_crc != 0);
      CHECK(Crc32(ev.payload, ev.len) == ev.payload_crc);
      for (uint32_t j = 0; j < ev.len; ++j) {
        const uint8_t want = static_cast<uint8_t>(
            static_cast<uint32_t>(ev.payload[0]) + j * 7);
        CHECK(ev.payload[j] == want);
      }
      frames += 1;
    } else if (ev.kind == replay::ReplayKind::kTrigger) {
      CHECK(ch.kind == replay::ReplayChannelKind::kMeta);
      CHECK(ev.trigger_id == 1);
      CHECK(ev.pre_roll_ns == kBeforeNs);
      CHECK(ev.post_roll_ns == kAfterNs);
    }
  }
  CHECK(pkg.status() == replay::ReplayStatus::kEnd);
  CHECK(events == expected_events);
  if (expected_events != 0) {
    CHECK(samples > 0);
    CHECK(frames > 0);
  }
}

}  // namespace

int main() {
  Fixture fx;
  fx.Init("r");
  CHECK(fx.Record());
  fx.Shutdown();
  CHECK(triggered::TriggeredRecorder::PackageComplete(fx.pkg_dir));

  replay::ReplayPackage pkg;
  CHECK(pkg.Open(fx.pkg_dir) == replay::ReplayStatus::kOk);
  CHECK(pkg.is_open());
  CHECK(pkg.wal_present());
  CHECK(pkg.package_complete());
  CHECK(pkg.error().empty());

  // ---- A: discovery, merge order, counts, payloads ----
  const std::vector<replay::ReplayChannel>& channels = pkg.channels();
  CHECK(channels.size() == 4);
  CHECK(channels[0].kind == replay::ReplayChannelKind::kMeta);
  CHECK(channels[0].name == "meta");
  CHECK(channels[1].kind == replay::ReplayChannelKind::kSmall);
  CHECK(channels[1].name == "small");
  // frames/*.index.tbin are ordered byte-wise by file name: aux_0 < cloud_1.
  CHECK(channels[2].kind == replay::ReplayChannelKind::kPinnedSlot);
  CHECK(channels[2].name == "aux_0");
  CHECK(channels[3].kind == replay::ReplayChannelKind::kPinnedSlot);
  CHECK(channels[3].name == "cloud_1");
  for (const replay::ReplayChannel& c : channels) {
    CHECK(c.has_footer_index);
  }
  for (uint32_t i = 2; i < 4; ++i) {
    CHECK(channels[i].registry.size() == 2);
    CHECK(channels[i].registry[0].struct_id == triggered::kFrameIndexStructId);
    CHECK(channels[i].registry[1].struct_id == triggered::kFrameGapStructId);
  }
  CHECK(pkg.FindChannel(2) != nullptr);
  CHECK(pkg.FindChannel(99) == nullptr);
  CHECK(ChannelByName(pkg, "cloud_1") == &channels[3]);
  CHECK(ChannelByName(pkg, "aux_0") == &channels[2]);
  CHECK(ChannelByName(pkg, "missing") == nullptr);

  const TbinCounts meta_counts = CountTbin(fx.pkg_dir + "/meta.tbin");
  const TbinCounts small_counts = CountTbin(fx.pkg_dir + "/small.tbin");
  const TbinCounts frame_counts =
      CountTbin(fx.pkg_dir + "/frames/cloud_1.index.tbin");
  const TbinCounts aux_counts = CountTbin(fx.pkg_dir + "/frames/aux_0.index.tbin");
  CHECK(channels[0].record_count == meta_counts.total);
  CHECK(channels[1].record_count == small_counts.total);
  CHECK(channels[2].record_count == aux_counts.total);
  CHECK(channels[3].record_count == frame_counts.total);
  // REC-27 writes one payload-profile row per big-frame channel; the fixture
  // has two (cloud_1 and aux_0).
  CHECK(meta_counts.total == 5);  // trigger + TrigMeta + 2 profiles + stats
  CHECK(small_counts.gaps == 0);
  CHECK(frame_counts.gaps == 0);
  CHECK(aux_counts.gaps == 0);

  const std::vector<Snap> full = Collect(pkg);
  CHECK(!full.empty());
  CHECK(pkg.first_mono_ns() == full.front().mono_ns);
  CHECK(pkg.last_mono_ns() == full.back().mono_ns);
  CHECK(pkg.last_mono_ns() > pkg.first_mono_ns());

  // Per-channel data sequences advance in file order and every channel's
  // event count equals its .tbin record count.
  std::vector<uint64_t> counts(channels.size(), 0);
  std::vector<uint64_t> last_data_seq(channels.size(), 0);
  std::vector<bool> saw_data(channels.size(), false);
  for (const Snap& s : full) {
    counts[s.channel] += 1;
    if (s.kind == replay::ReplayKind::kData) {
      if (saw_data[s.channel]) {
        CHECK(s.seq > last_data_seq[s.channel]);
      }
      saw_data[s.channel] = true;
      last_data_seq[s.channel] = s.seq;
    }
  }
  for (std::size_t i = 0; i < channels.size(); ++i) {
    CHECK(counts[i] == channels[i].record_count);
  }
  CHECK(counts[1] == small_counts.data);
  CHECK(counts[2] == aux_counts.data);
  CHECK(counts[3] == frame_counts.data);

  // Full payload and ordering verification on a second pass.
  CHECK(pkg.SeekMonoNs(0) == replay::ReplayStatus::kOk);
  CheckStream(pkg, channels, full.size());

  // ---- B: seek lands on the first event with ts >= target ----
  const std::vector<uint64_t> targets = {
      0,
      fx.pkg_dir.empty() ? 0 : full.front().mono_ns,
      full[full.size() / 3].mono_ns,
      full[full.size() / 2].mono_ns + 1,
      full.back().mono_ns,
      full.back().mono_ns + 1000000000ULL,
  };
  for (const uint64_t target : targets) {
    replay::ReplayPackage seek_pkg;
    CHECK(seek_pkg.Open(fx.pkg_dir) == replay::ReplayStatus::kOk);
    CHECK(seek_pkg.SeekMonoNs(target) == replay::ReplayStatus::kOk);
    const std::vector<Snap> rest = Collect(seek_pkg);
    std::size_t expect = 0;
    while (expect < full.size() && full[expect].mono_ns < target) {
      expect += 1;
    }
    CHECK(rest.size() == full.size() - expect);
    for (std::size_t i = 0; i < rest.size(); ++i) {
      CHECK(rest[i] == full[expect + i]);
    }
    if (target > full.back().mono_ns) {
      CHECK(rest.empty());
    }
  }

  // ---- B2: an unindexed frame file still seeks (sequential block table) ----
  {
    const std::string copy = fx.root + "/unindexed";
    CopyTree(fx.pkg_dir, copy);
    const std::string idx = copy + "/frames/cloud_1.index.tbin";
    struct stat st {};
    CHECK(stat(idx.c_str(), &st) == 0);
    CHECK(st.st_size > static_cast<off_t>(sizeof(ThymeBinIndexFooter)));
    ThymeBinIndexFooter footer{};
    std::FILE* f = std::fopen(idx.c_str(), "rb");
    CHECK(f != nullptr);
    CHECK(std::fseek(f, -static_cast<long>(sizeof(footer)), SEEK_END) == 0);
    CHECK(std::fread(&footer, sizeof(footer), 1, f) == 1);
    std::fclose(f);
    const bool had_footer =
        std::memcmp(footer.magic, kThymeBinIndexMagic, sizeof(footer.magic)) ==
        0;
    CHECK(had_footer);
    CHECK(truncate(idx.c_str(), static_cast<off_t>(footer.entries_offset)) == 0);

    replay::ReplayPackage flat;
    CHECK(flat.Open(copy) == replay::ReplayStatus::kOk);
    CHECK(!flat.channels()[3].has_footer_index);
    CHECK(flat.channels()[3].record_count == channels[3].record_count);
    const std::vector<Snap> flat_full = Collect(flat);
    CHECK(flat_full.size() == full.size());
    for (std::size_t i = 0; i < full.size(); ++i) {
      CHECK(flat_full[i] == full[i]);
    }
    const uint64_t target = full[full.size() / 2].mono_ns + 1;
    CHECK(flat.SeekMonoNs(target) == replay::ReplayStatus::kOk);
    const std::vector<Snap> rest = Collect(flat);
    std::size_t expect = 0;
    while (expect < full.size() && full[expect].mono_ns < target) {
      expect += 1;
    }
    CHECK(rest.size() == full.size() - expect);
    for (std::size_t i = 0; i < rest.size(); ++i) {
      CHECK(rest[i] == full[expect + i]);
    }
  }

  // ---- C: explicit errors, never a crash ----
  {
    replay::ReplayPackage bad;
    CHECK(bad.Open(fx.root + "/does_not_exist") ==
          replay::ReplayStatus::kNotADirectory);
    CHECK(!bad.error().empty());

    const std::string empty = fx.root + "/empty";
    CHECK(mkdir(empty.c_str(), 0700) == 0);
    CHECK(bad.Open(empty) == replay::ReplayStatus::kMissingMeta);

    const std::string no_meta = fx.root + "/no_meta";
    CopyTree(fx.pkg_dir, no_meta);
    CHECK(std::remove((no_meta + "/meta.tbin").c_str()) == 0);
    CHECK(bad.Open(no_meta) == replay::ReplayStatus::kMissingMeta);

    const std::string no_raw = fx.root + "/no_raw";
    CopyTree(fx.pkg_dir, no_raw);
    CHECK(std::remove((no_raw + "/frames/cloud_1.raw").c_str()) == 0);
    CHECK(bad.Open(no_raw) == replay::ReplayStatus::kMissingRaw);

    const std::string no_aux_raw = fx.root + "/no_aux_raw";
    CopyTree(fx.pkg_dir, no_aux_raw);
    CHECK(std::remove((no_aux_raw + "/frames/aux_0.raw").c_str()) == 0);
    CHECK(bad.Open(no_aux_raw) == replay::ReplayStatus::kMissingRaw);

    const std::string orphan = fx.root + "/orphan";
    CopyTree(fx.pkg_dir, orphan);
    CHECK(CopyFile(orphan + "/frames/cloud_1.raw",
                   orphan + "/frames/orphan.raw"));
    CHECK(bad.Open(orphan) == replay::ReplayStatus::kOrphanRaw);

    const std::string short_meta = fx.root + "/short_meta";
    CopyTree(fx.pkg_dir, short_meta);
    CHECK(truncate((short_meta + "/meta.tbin").c_str(), 20) == 0);
    CHECK(bad.Open(short_meta) == replay::ReplayStatus::kTruncated);

    const std::string short_raw = fx.root + "/short_raw";
    CopyTree(fx.pkg_dir, short_raw);
    CHECK(truncate((short_raw + "/frames/cloud_1.raw").c_str(), 32) == 0);
    const replay::ReplayStatus raw_st = bad.Open(short_raw);
    CHECK(raw_st == replay::ReplayStatus::kTruncated ||
          raw_st == replay::ReplayStatus::kCorrupt);

    // Flip one byte inside the first small.tbin payload: record CRC fails.
    const std::string corrupt = fx.root + "/corrupt";
    CopyTree(fx.pkg_dir, corrupt);
    const std::string small_path = corrupt + "/small.tbin";
    ThymeBinFileHeader hdr{};
    std::FILE* sf = std::fopen(small_path.c_str(), "r+b");
    CHECK(sf != nullptr);
    CHECK(std::fread(&hdr, sizeof(hdr), 1, sf) == 1);
    const uint64_t entry_bytes =
        hdr.file_abi_version >= kThymeBinFileAbiVersionKeyedRegistry
            ? sizeof(ThymeStructDesc)
            : static_cast<uint64_t>(kThymeStructDescLegacySize);
    const long record_start =
        static_cast<long>(sizeof(hdr)) +
        static_cast<long>(hdr.struct_count * entry_bytes);
    const long payload_off = record_start + 32;
    CHECK(std::fseek(sf, record_start, SEEK_SET) == 0);
    int got_type = 0;
    CHECK(std::fread(&got_type, 1, 1, sf) == 1);
    CHECK(got_type == static_cast<int>(ThymeBinEntryType::kData));
    CHECK(std::fseek(sf, payload_off, SEEK_SET) == 0);
    uint8_t v = 0;
    CHECK(std::fread(&v, 1, 1, sf) == 1);
    CHECK(std::fseek(sf, payload_off, SEEK_SET) == 0);
    v ^= 0x5A;
    CHECK(std::fwrite(&v, 1, 1, sf) == 1);
    std::fclose(sf);
    CHECK(bad.Open(corrupt) == replay::ReplayStatus::kCorrupt);

    // A failed Open leaves nothing playing.
    CHECK(!bad.is_open());
    replay::ReplayEvent ev;
    CHECK(bad.NextEvent(&ev) == replay::ReplayStatus::kInvalidArgument);
  }

  // ---- D: hand-built package with kGap/kSession entries ----
  {
    const std::string dir = fx.root + "/synthetic";
    WriteSyntheticPackage(dir);
    replay::ReplayPackage sp;
    CHECK(sp.Open(dir) == replay::ReplayStatus::kOk);
    CHECK(sp.channels().size() == 1);  // meta only: no small.tbin, no frames/
    CHECK(!sp.channels()[0].has_footer_index);
    CHECK(sp.wal_present());
    CHECK(sp.package_complete());
    CHECK(sp.first_mono_ns() == 1000);
    CHECK(sp.last_mono_ns() == 1200);

    replay::ReplayEvent ev;
    uint64_t last_mono = 0;
    CHECK(sp.NextEvent(&ev) == replay::ReplayStatus::kOk);
    CHECK(ev.kind == replay::ReplayKind::kTrigger);
    CHECK(ev.mono_ns == 1000 && ev.mono_ns >= last_mono);
    CHECK(ev.trigger_id == 9);
    CHECK(ev.pre_roll_ns == 100);
    CHECK(ev.post_roll_ns == 200);
    last_mono = ev.mono_ns;

    CHECK(sp.NextEvent(&ev) == replay::ReplayStatus::kOk);
    CHECK(ev.kind == replay::ReplayKind::kGap);
    CHECK(ev.mono_ns == 1100 && ev.mono_ns >= last_mono);
    CHECK(ev.struct_id == 0);
    CHECK(ev.gap_reason == triggered::kGapReasonOverrun);
    CHECK(ev.source_sequence == 2);  // missed count
    CHECK(ev.len == 0);
    CHECK(ev.payload == nullptr);
    last_mono = ev.mono_ns;

    CHECK(sp.NextEvent(&ev) == replay::ReplayStatus::kOk);
    CHECK(ev.kind == replay::ReplayKind::kData);
    CHECK(ev.mono_ns == 1200 && ev.mono_ns >= last_mono);
    CHECK(ev.struct_id == 0);
    CHECK(ev.record_sequence == 0);
    CHECK(ev.source_sequence == 7);
    CHECK(ev.len == sizeof(Pod16));
    Pod16 p{};
    std::memcpy(&p, ev.payload, sizeof(p));
    CHECK(p.a == 12345 && p.b == 111 && p.c == 222);
    last_mono = ev.mono_ns;

    // The kSession row is the last record in the file and has no timestamp:
    // it inherits the preceding record's position (1200) and sorts after the
    // data row (same mono_ns, larger record_sequence).
    CHECK(sp.NextEvent(&ev) == replay::ReplayStatus::kOk);
    CHECK(ev.kind == replay::ReplayKind::kSession);
    CHECK(ev.mono_ns == 1200 && ev.mono_ns >= last_mono);
    CHECK(ev.session_id == 1234);
    CHECK(ev.session_reason == 3);
    CHECK(ev.record_sequence == 1234);

    CHECK(sp.NextEvent(&ev) == replay::ReplayStatus::kEnd);
    CHECK(sp.SeekMonoNs(1100) == replay::ReplayStatus::kOk);
    CHECK(sp.NextEvent(&ev) == replay::ReplayStatus::kOk);
    CHECK(ev.kind == replay::ReplayKind::kGap);
    CHECK(ev.mono_ns == 1100);
  }

  fx.Cleanup();
  std::printf("test_replay_reader: OK\n");
  return 0;
}
