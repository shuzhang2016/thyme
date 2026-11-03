// test_replay_cli.cpp — REP-08 (player CLI) / REC-07 (trigger-package replay
// tool).
//
// The fixture records a real trigger package in-process with meta.tbin,
// small.tbin and two frames/<channel> pairs (same pattern as
// test_replay_reader.cpp). The test then exercises the CLI *logic* (the same
// library the binary links) without spawning a process:
//
//   A: list/info on the package (channel table, geometry, rate/bytes, JSON);
//   B: verify on the package and on a deliberately corrupted copy (non-zero);
//   C: play --as-fast into fresh targets — per-channel counts and byte-for-
//      byte payload equality with the package (big frames + small ring);
//   D: --from/--to uses SeekMonoNs and selects exactly the expected window,
//      stopping before the first out-of-window event;
//   E: --step delivers exactly one event;
//   F: a missing target is skipped with a counted warning; --strict-targets
//      errors before creating any target;
//   G: an existing big-frame channel is refused without --force;
//   H: argv dispatch (RunCli) exit codes;
//   K: REP-13 absolute/stitch multi-slice discovery, joins, seek/windows/gates.

#include "replay_cli.h"
#include "replay_status.h"

#include "thyme/check.h"
#include "thyme/contracts/frame_header.h"
#include "thyme/record/record_mirror_ring_client.h"
#include "thyme/record/thyme_bin.h"
#include "thyme/record/triggered_recorder.h"
#include "thyme/replay/replay_reader.h"
#include "thyme/runtime/pinned_slot_store.h"
#include "thyme/runtime/record_mirror_ring.h"
#include "thyme/runtime/shm_region.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <atomic>
#include <fstream>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <vector>

using namespace thyme;

namespace {

constexpr uint64_t kPaceNs = 4000000;
constexpr uint64_t kBeforeNs = 100000000;
constexpr uint64_t kAfterNs = 200000000;
constexpr uint64_t kTriggerIter = 30;
constexpr uint64_t kTotalIters = 90;
constexpr uint32_t kFrameSlotBytes = 4096;
constexpr uint32_t kFrameSlots = 128;
constexpr uint32_t kRingDepth = 1024;

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

struct Fixture {
  std::string root;
  std::string out_dir;
  std::string page_name;
  std::string cloud_name;
  std::string aux_name;
  std::string pkg_dir;
  record_mirror::RingClient mirror;
  PinnedSlotWriter cloud;
  PinnedSlotWriter aux;
  triggered::TriggeredRecorder rec;
  uint64_t next_frame = 0;

  void Init() {
    char buf[160];
    const long me = static_cast<long>(getpid());
    std::snprintf(buf, sizeof(buf), "/tmp/thyme_rep08_%ld", me);
    root = buf;
    RmTree(root);
    for (const std::string& p : {root, root + "/out"}) {
      CHECK(mkdir(p.c_str(), 0700) == 0);
    }
    std::snprintf(buf, sizeof(buf), "/thyme_rep08_p_%ld", me);
    page_name = buf;
    std::snprintf(buf, sizeof(buf), "/thyme_rep08_c_%ld", me);
    cloud_name = buf;
    std::snprintf(buf, sizeof(buf), "/thyme_rep08_a_%ld", me);
    aux_name = buf;
    ShmRegion::UnlinkName(page_name.c_str());
    ShmRegion::UnlinkName(cloud_name.c_str());
    ShmRegion::UnlinkName(aux_name.c_str());
    out_dir = root + "/out";

    ThymeStructDesc desc{};
    desc.struct_id = 0;
    desc.contract_version = 1;
    desc.payload_size = sizeof(Pod16);
    std::snprintf(desc.name, sizeof(desc.name), "Pod16");
    RecordMirrorRingSpec spec{sizeof(Pod16), 256};
    CHECK(mirror.Create(page_name.c_str(), &desc, &spec, 1, 7));
    CHECK(cloud.Create(cloud_name.c_str(), kFrameSlotBytes, kFrameSlots, 0));
    CHECK(aux.Create(aux_name.c_str(), kFrameSlotBytes, kFrameSlots, 0));

    triggered::Config cfg;
    cfg.out_dir = out_dir;
    triggered::SourceConfig mirror_src;
    mirror_src.kind = triggered::SourceConfig::Kind::kMirrorRing;
    mirror_src.shm_name = page_name;
    triggered::SourceConfig frame_src;
    frame_src.kind = triggered::SourceConfig::Kind::kPinnedSlot;
    frame_src.shm_name = cloud_name;
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
    PinnedSlotWriter::BeginView bv = cloud.Begin(len);
    CHECK(bv.payload != nullptr);
    FillFrame(bv.payload, len, next_frame);
    bv.header->flags.store(next_frame % 4 == 0 ? 1u : 0u,
                           std::memory_order_relaxed);
    cloud.Commit(NowNs());
    PinnedSlotWriter::BeginView av = aux.Begin(len);
    CHECK(av.payload != nullptr);
    FillFrame(av.payload, len, next_frame + 1000);
    aux.Commit(NowNs());
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
    rec.Shutdown();
    cloud.Close();
    aux.Close();
    mirror.Shutdown(true);
  }

  void CleanupShm() {
    ShmRegion::UnlinkName(cloud_name.c_str());
    ShmRegion::UnlinkName(
        PinnedSlotWriter::LeaseShmName(cloud_name.c_str()).c_str());
    ShmRegion::UnlinkName(aux_name.c_str());
    ShmRegion::UnlinkName(
        PinnedSlotWriter::LeaseShmName(aux_name.c_str()).c_str());
  }

  void Cleanup() {
    CleanupShm();
    RmTree(root);
  }
};

// ---- expected package contents -----------------------------------------

struct FrameExp {
  uint64_t mono_ns = 0;
  uint64_t seq = 0;
  uint32_t len = 0;
  uint32_t flags = 0;
  std::vector<uint8_t> bytes;
};

struct SampleExp {
  uint64_t mono_ns = 0;
  uint32_t source_sequence = 0;
  std::vector<uint8_t> bytes;
};

struct Expected {
  uint32_t meta = 0;
  uint32_t small = 0;
  uint32_t cloud = 0;
  uint32_t aux = 0;
  std::vector<FrameExp> cloud_frames;
  std::vector<FrameExp> aux_frames;
  std::vector<SampleExp> samples;
  std::vector<uint64_t> mono;  // every event (for the window expectations)
  uint64_t meta_events = 0;
};

uint32_t ChannelIdByName(const replay::ReplayPackage& pkg,
                         const std::string& name) {
  for (const replay::ReplayChannel& c : pkg.channels()) {
    if (c.name == name) {
      return c.id;
    }
  }
  CHECK(false);
  return 0;
}

void Collect(const std::string& dir, Expected* e) {
  replay::ReplayPackage pkg;
  CHECK(pkg.Open(dir) == replay::ReplayStatus::kOk);
  e->meta = ChannelIdByName(pkg, "meta");
  e->small = ChannelIdByName(pkg, "small");
  e->cloud = ChannelIdByName(pkg, "cloud_1");
  e->aux = ChannelIdByName(pkg, "aux_0");
  replay::ReplayEvent ev;
  while (pkg.NextEvent(&ev) == replay::ReplayStatus::kOk) {
    e->mono.push_back(ev.mono_ns);
    if (ev.channel == e->meta) {
      e->meta_events += 1;
    }
    if (ev.kind != replay::ReplayKind::kData) {
      continue;
    }
    if (ev.channel == e->cloud || ev.channel == e->aux) {
      FrameExp f;
      f.mono_ns = ev.mono_ns;
      f.seq = ev.record_sequence;
      f.len = ev.len;
      f.flags = ev.frame_flags;
      f.bytes.assign(ev.payload, ev.payload + ev.len);
      if (ev.channel == e->cloud) {
        e->cloud_frames.push_back(std::move(f));
      } else {
        e->aux_frames.push_back(std::move(f));
      }
    } else if (ev.channel == e->small) {
      SampleExp s;
      s.mono_ns = ev.mono_ns;
      s.source_sequence = ev.source_sequence;
      s.bytes.assign(ev.payload, ev.payload + ev.len);
      e->samples.push_back(std::move(s));
    }
  }
  CHECK(e->mono.size() > 0);
}

// ---- fresh targets ------------------------------------------------------

int g_target_seq = 0;

std::string UniqueShmName(const char* tag) {
  char buf[160];
  g_target_seq += 1;
  std::snprintf(buf, sizeof(buf), "/thyme_rep08_%s_%d_%ld", tag, g_target_seq,
                static_cast<long>(getpid()));
  return buf;
}

struct TargetSet {
  std::string ring_name;
  std::string cloud_name;
  std::string aux_name;
  record_mirror::RingClient ring;

  void Create() {
    ring_name = UniqueShmName("ring");
    cloud_name = UniqueShmName("cloud");
    aux_name = UniqueShmName("aux");
    ShmRegion::UnlinkName(ring_name.c_str());
    UnlinkBig(cloud_name);
    UnlinkBig(aux_name);
    ThymeStructDesc desc{};
    desc.struct_id = 0;
    desc.contract_version = 1;
    desc.payload_size = sizeof(Pod16);
    std::snprintf(desc.name, sizeof(desc.name), "Pod16");
    RecordMirrorRingSpec spec{sizeof(Pod16), kRingDepth};
    CHECK(ring.Create(ring_name.c_str(), &desc, &spec, 1, 9));
  }

  static void UnlinkBig(const std::string& name) {
    ShmRegion::UnlinkName(name.c_str());
    ShmRegion::UnlinkName(PinnedSlotWriter::LeaseShmName(name.c_str()).c_str());
  }

  void Cleanup() {
    ring.Shutdown(true);
    UnlinkBig(cloud_name);
    UnlinkBig(aux_name);
  }
};

void AddPinnedSlotTarget(replaycli::PlayOptions* opt, const std::string& channel,
                       const std::string& shm) {
  replaycli::TargetSpec spec;
  std::string err;
  CHECK(replaycli::ParsePinnedSlotTarget(channel + "=" + shm, &spec, &err));
  opt->pinned_slots.push_back(spec);
}

void AddSmallTarget(replaycli::PlayOptions* opt, const std::string& channel,
                    uint16_t struct_id, const std::string& ring,
                    uint16_t target_struct_id) {
  replaycli::TargetSpec spec;
  std::string err;
  const std::string text = channel + ":" + std::to_string(struct_id) + "=" +
                           ring + ":" + std::to_string(target_struct_id);
  CHECK(replaycli::ParseSmallTarget(text, &spec, &err));
  opt->smalls.push_back(spec);
}

void CheckBigTarget(const std::string& name, const std::vector<FrameExp>& exp) {
  PinnedSlotReader reader;
  CHECK(reader.Open(name.c_str(), 2000000000ULL, /*lease_read_only=*/true));
  for (std::size_t i = 0; i < exp.size(); ++i) {
    const FrameExp& want = exp[i];
    PinnedSlotReader::View v;
    CHECK(reader.ReadFrame(i, v) == PinnedSlotReader::ReadResult::kOk);
    CHECK(v.data_len == want.len);
    CHECK(v.mono_ns == want.mono_ns);
    CHECK(v.flags == want.flags);
    CHECK(v.crc_ok);
    CHECK(std::memcmp(v.payload, want.bytes.data(), want.len) == 0);
  }
  PinnedSlotReader::View extra;
  CHECK(reader.ReadFrame(exp.size(), extra) ==
        PinnedSlotReader::ReadResult::kNotPublished);
}

void CheckSmallTarget(RecordMirrorRingPage* page,
                      const std::vector<SampleExp>& exp,
                      bool preserve_sequences = false) {
  CHECK(page != nullptr);
  CHECK(page->RingHead(0) == exp.size());
  uint8_t buf[256];
  for (std::size_t i = 0; i < exp.size(); ++i) {
    const SampleExp& want = exp[i];
    uint32_t size = 0;
    uint64_t mono = 0;
    uint32_t seq = 0;
    CHECK(page->ReadRingSample(0, static_cast<uint64_t>(i), buf, sizeof(buf),
                               size, mono, seq));
    CHECK(size == want.bytes.size());
    CHECK(mono == want.mono_ns);
    CHECK(seq == (preserve_sequences ? want.source_sequence : i));
    CHECK(std::memcmp(buf, want.bytes.data(), size) == 0);
  }
}

uint64_t SumFrameBytes(const std::vector<FrameExp>& frames) {
  uint64_t n = 0;
  for (const FrameExp& f : frames) {
    n += f.len;
  }
  return n;
}

void MakeFullOptions(const std::string& pkg, const TargetSet& t,
                     replaycli::PlayOptions* opt) {
  *opt = replaycli::PlayOptions{};
  opt->package_dir = pkg;
  AddPinnedSlotTarget(opt, "cloud_1", t.cloud_name);
  AddPinnedSlotTarget(opt, "aux_0", t.aux_name);
  AddSmallTarget(opt, "small", 0, t.ring_name, 0);
  opt->as_fast = true;
  opt->strict_targets = true;
  opt->pinned_slot_slots = 256;
}

struct SliceSpec {
  std::vector<uint64_t> small;
  std::vector<uint64_t> frames;
};

ThymeStructDesc SliceDesc(uint16_t id, const char* name, uint32_t size,
                          uint16_t version = 1) {
  ThymeStructDesc desc{};
  desc.struct_id = id;
  desc.contract_version = version;
  desc.payload_size = size;
  std::snprintf(desc.name, sizeof(desc.name), "%s", name);
  return desc;
}

void WriteSlicePackage(const std::string& dir, const SliceSpec& spec,
                       uint64_t seq) {
  CHECK(mkdir(dir.c_str(), 0700) == 0);
  CHECK(mkdir((dir + "/frames").c_str(), 0700) == 0);
  const uint64_t first = std::min(spec.small.front(), spec.frames.front()) - 500;
  const ThymeStructDesc meta = SliceDesc(63, "Meta", 8);
  const ThymeStructDesc small = SliceDesc(0, "Pod16", sizeof(Pod16));
  const ThymeStructDesc frame =
      SliceDesc(triggered::kFrameIndexStructId, "FrameIdx",
                sizeof(triggered::FrameIndexRecord));
  const ThymeStructDesc gap = SliceDesc(triggered::kFrameGapStructId,
                                        "FrameGap", 0);
  {
    ThymeBinFileSink sink((dir + "/meta.tbin").c_str());
    ThymeBinEncoder encoder(&sink);
    CHECK(encoder.WriteHeader(seq, first, &meta, 1));
    CHECK(encoder.WriteData(0, meta.struct_id, first, 0, &first, sizeof(first)));
    CHECK(encoder.Finish());
    CHECK(sink.Sync());
  }
  {
    ThymeBinFileSink sink((dir + "/small.tbin").c_str());
    ThymeBinEncoder encoder(&sink);
    CHECK(encoder.WriteHeader(seq, first, &small, 1));
    for (std::size_t i = 0; i < spec.small.size(); ++i) {
      Pod16 pod{spec.small[i], static_cast<uint32_t>(i), 0};
      CHECK(encoder.WriteData(i, small.struct_id, spec.small[i],
                              static_cast<uint32_t>(i), &pod, sizeof(pod)));
    }
    CHECK(encoder.Finish());
    CHECK(sink.Sync());
  }
  {
    std::FILE* raw = std::fopen((dir + "/frames/cam.raw").c_str(), "wb");
    CHECK(raw != nullptr);
    ThymeBinFileSink sink((dir + "/frames/cam.index.tbin").c_str());
    ThymeBinEncoder encoder(&sink);
    const ThymeStructDesc registry[2] = {frame, gap};
    CHECK(encoder.WriteHeader(seq, first, registry, 2));
    uint64_t offset = 0;
    for (std::size_t i = 0; i < spec.frames.size(); ++i) {
      uint8_t bytes[128]{};
      std::memcpy(bytes, &spec.frames[i], sizeof(spec.frames[i]));
      CHECK(std::fwrite(bytes, 1, sizeof(bytes), raw) == sizeof(bytes));
      triggered::FrameIndexRecord index{};
      index.offset = offset;
      index.len = sizeof(bytes);
      index.payload_crc = Crc32(bytes, sizeof(bytes));
      CHECK(encoder.WriteData(i, frame.struct_id, spec.frames[i],
                              static_cast<uint32_t>(i), &index, sizeof(index)));
      offset += sizeof(bytes);
    }
    CHECK(encoder.Finish());
    CHECK(sink.Sync());
    CHECK(std::fclose(raw) == 0);
  }
  std::FILE* wal = std::fopen((dir + "/wal.log").c_str(), "wb");
  CHECK(wal != nullptr);
  const triggered::WalHeader header{{'T', 'R', 'I', 'G', 'W', 'A', 'L', '1'},
                                    triggered::kContinuousWalMark | seq};
  const triggered::WalRow seal{triggered::kWalOpSeal, 0, 0, 0, 0, 0};
  CHECK(std::fwrite(&header, sizeof(header), 1, wal) == 1);
  CHECK(std::fwrite(&seal, sizeof(seal), 1, wal) == 1);
  CHECK(std::fclose(wal) == 0);
}

struct SliceTarget {
  std::string ring_name;
  std::string frame_name;
  record_mirror::RingClient ring;

  void Create() {
    ring_name = UniqueShmName("slice_ring");
    frame_name = UniqueShmName("slice_frame");
    ShmRegion::UnlinkName(ring_name.c_str());
    TargetSet::UnlinkBig(frame_name);
    const ThymeStructDesc desc = SliceDesc(0, "Pod16", sizeof(Pod16));
    RecordMirrorRingSpec spec{sizeof(Pod16), 128};
    CHECK(ring.Create(ring_name.c_str(), &desc, &spec, 1, 13));
  }

  replaycli::PlayOptions Options(const std::string& package) const {
    replaycli::PlayOptions opt;
    opt.package_dir = package;
    AddPinnedSlotTarget(&opt, "cam", frame_name);
    AddSmallTarget(&opt, "small", 0, ring_name, 0);
    opt.as_fast = true;
    opt.strict_targets = true;
    return opt;
  }

  void Check(const std::vector<uint64_t>& small,
             const std::vector<uint64_t>& frames,
             const std::vector<uint64_t>* source_small = nullptr,
             const std::vector<uint64_t>* source_frames = nullptr) {
    CHECK(ring.Page()->RingHead(0) == small.size());
    for (std::size_t i = 0; i < small.size(); ++i) {
      Pod16 pod{};
      uint32_t size = 0;
      uint64_t mono = 0;
      uint32_t sequence = 0;
      CHECK(ring.Page()->ReadRingSample(0, i, reinterpret_cast<uint8_t*>(&pod),
                                        sizeof(pod), size, mono, sequence));
      const uint64_t source_ns =
          source_small == nullptr ? small[i] : (*source_small)[i];
      CHECK(pod.a == source_ns && mono == small[i]);
    }
    PinnedSlotReader reader;
    CHECK(reader.Open(frame_name.c_str(), 1000000000ULL,
                      /*lease_read_only=*/true));
    for (std::size_t i = 0; i < frames.size(); ++i) {
      PinnedSlotReader::View view;
      CHECK(reader.ReadFrame(i, view) == PinnedSlotReader::ReadResult::kOk);
      uint64_t value = 0;
      std::memcpy(&value, view.payload, sizeof(value));
      const uint64_t source_ns =
          source_frames == nullptr ? frames[i] : (*source_frames)[i];
      CHECK(value == source_ns && view.mono_ns == frames[i]);
    }
    PinnedSlotReader::View extra;
    CHECK(reader.ReadFrame(frames.size(), extra) ==
          PinnedSlotReader::ReadResult::kNotPublished);
  }

  void Cleanup() {
    ring.Shutdown(true);
    TargetSet::UnlinkBig(frame_name);
  }
};

}  // namespace

int main() {
  Fixture fx;
  fx.Init();
  CHECK(fx.Record());
  fx.Shutdown();
  CHECK(triggered::TriggeredRecorder::PackageComplete(fx.pkg_dir));

  Expected exp;
  Collect(fx.pkg_dir, &exp);
  CHECK(exp.cloud_frames.size() > 0);
  CHECK(exp.aux_frames.size() > 0);
  CHECK(exp.samples.size() > 0);
  CHECK(exp.meta_events > 0);
  const uint64_t first_mono = exp.mono.front();
  const uint64_t last_mono = exp.mono.back();

  // ---- time parsing ----
  {
    replaycli::TimeSpec ts;
    std::string err;
    CHECK(replaycli::ParseTimeSpec("250ms", &ts, &err));
    CHECK(!ts.absolute && ts.ns == 250000000ULL);
    CHECK(replaycli::ParseTimeSpec("1.5s", &ts, &err));
    CHECK(!ts.absolute && ts.ns == 1500000000ULL);
    CHECK(replaycli::ParseTimeSpec("00:00:01.500", &ts, &err));
    CHECK(!ts.absolute && ts.ns == 1500000000ULL);
    CHECK(replaycli::ParseTimeSpec("02:03:04", &ts, &err));
    CHECK(ts.ns == (2ULL * 3600ULL + 3ULL * 60ULL + 4ULL) * 1000000000ULL);
    CHECK(replaycli::ParseTimeSpec("@1234", &ts, &err));
    CHECK(ts.absolute && ts.ns == 1234);
    CHECK(replaycli::ParseTimeSpec("123", &ts, &err));
    CHECK(!ts.absolute && ts.ns == 123);
    CHECK(!replaycli::ParseTimeSpec("nope", &ts, &err));
    CHECK(!replaycli::ParseTimeSpec("00:99:00", &ts, &err));
    CHECK(!replaycli::ParseTimeSpec("-5", &ts, &err));

    replaycli::TargetSpec spec;
    CHECK(!replaycli::ParsePinnedSlotTarget("cloud_1", &spec, &err));
    CHECK(replaycli::ParsePinnedSlotTarget("cloud_1=/x", &spec, &err));
    CHECK(spec.channel == "cloud_1" && spec.shm_name == "/x");
    CHECK(!replaycli::ParseSmallTarget("small:0=/x", &spec, &err));
    CHECK(replaycli::ParseSmallTarget("small:3=/x:2", &spec, &err));
    CHECK(spec.channel == "small" && spec.struct_id == 3 &&
          spec.target_struct_id == 2 && spec.shm_name == "/x");
    CHECK(!replaycli::ParseSmallTarget("small:70000=/x:0", &spec, &err));
  }

  // ---- A1: list ----
  {
    replaycli::PackageReport rep;
    std::ostringstream out;
    CHECK(replaycli::RunList(fx.pkg_dir, false, out, &rep) ==
          replaycli::kExitOk);
    CHECK(rep.ok && rep.wal_present && rep.package_complete);
    CHECK(rep.channels.size() == 4);
    CHECK(rep.events == exp.mono.size());
    const replaycli::ChannelReport* cloud = rep.Find("cloud_1");
    const replaycli::ChannelReport* aux = rep.Find("aux_0");
    const replaycli::ChannelReport* small = rep.Find("small");
    const replaycli::ChannelReport* meta = rep.Find("meta");
    CHECK(cloud != nullptr && aux != nullptr && small != nullptr &&
          meta != nullptr);
    CHECK(cloud->kind == "pinned_slot");
    CHECK(cloud->data == exp.cloud_frames.size());
    CHECK(aux->data == exp.aux_frames.size());
    CHECK(small->data == exp.samples.size());
    CHECK(meta->records == exp.meta_events);
    CHECK(cloud->max_payload_bytes >= 512);
    CHECK(cloud->tbin_bytes > 0 && cloud->raw_bytes > 0);
    CHECK(cloud->indexed);
    CHECK(rep.first_mono_ns == first_mono);
    CHECK(rep.last_mono_ns == last_mono);
    CHECK(rep.duration_ns == last_mono - first_mono);
    CHECK(out.str().find("package :") != std::string::npos);
    CHECK(out.str().find("pinned_slot") != std::string::npos);

    std::ostringstream jout;
    CHECK(replaycli::RunList(fx.pkg_dir, true, jout, &rep) ==
          replaycli::kExitOk);
    const std::string js = jout.str();
    CHECK(js.find("\"command\":\"list\"") != std::string::npos);
    CHECK(js.find("\"package_complete\":true") != std::string::npos);
    CHECK(js.find("\"name\":\"cloud_1\"") != std::string::npos);
    CHECK(js.find("\"indexed\":true") != std::string::npos);
  }

  // ---- A2: info ----
  {
    replaycli::PackageReport rep;
    std::ostringstream out;
    CHECK(replaycli::RunInfo(fx.pkg_dir, false, out, &rep) ==
          replaycli::kExitOk);
    CHECK(rep.events == exp.mono.size());
    const replaycli::ChannelReport* cloud = rep.Find("cloud_1");
    CHECK(cloud != nullptr);
    CHECK(cloud->data_bytes == SumFrameBytes(exp.cloud_frames));
    CHECK(cloud->rate_hz > 0.0);
    CHECK(cloud->bytes_per_s > 0.0);
    CHECK(rep.data_bytes >= cloud->data_bytes);
    CHECK(out.str().find("RATE_HZ") != std::string::npos);

    std::ostringstream jout;
    CHECK(replaycli::RunInfo(fx.pkg_dir, true, jout, &rep) ==
          replaycli::kExitOk);
    const std::string js = jout.str();
    CHECK(js.find("\"command\":\"info\"") != std::string::npos);
    CHECK(js.find("\"bytes_per_s\":") != std::string::npos);
    CHECK(js.find("\"rate_hz\":") != std::string::npos);
  }

  // ---- B: verify (clean + corrupted) ----
  {
    replaycli::PackageReport rep;
    std::ostringstream out;
    CHECK(replaycli::RunVerify(fx.pkg_dir, false, out, &rep) ==
          replaycli::kExitOk);
    CHECK(rep.ok && rep.error.empty());
    CHECK(rep.events == exp.mono.size());
    CHECK(rep.mono_regressions == 0);
    const replaycli::ChannelReport* cloud = rep.Find("cloud_1");
    CHECK(cloud != nullptr);
    CHECK(cloud->crc_checked == exp.cloud_frames.size());
    CHECK(cloud->crc_ok == exp.cloud_frames.size());
    CHECK(cloud->crc_bad == 0);
    CHECK(cloud->crc_unchecked == 0);
    CHECK(cloud->min_frame_len >= 512);
    CHECK(cloud->max_frame_len == cloud->max_payload_bytes);
    CHECK(out.str().find("verify  : OK") != std::string::npos);

    std::ostringstream jout;
    CHECK(replaycli::RunVerify(fx.pkg_dir, true, jout, &rep) ==
          replaycli::kExitOk);
    CHECK(jout.str().find("\"ok\":true") != std::string::npos);

    // Corrupt one payload byte inside the first small.tbin data record.
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

    replaycli::PackageReport bad;
    std::ostringstream bout;
    CHECK(replaycli::RunVerify(corrupt, false, bout, &bad) !=
          replaycli::kExitOk);
    CHECK(!bad.ok);
    CHECK(!bad.error.empty());
    CHECK(replaycli::RunVerify(corrupt, true, bout, &bad) !=
          replaycli::kExitOk);
    CHECK(bout.str().find("\"ok\":false") != std::string::npos);

    // Corrupt a big-frame .raw payload byte: the .tbin record CRC still
    // passes (the index record is untouched), so only the per-frame payload
    // CRC pass can catch it.
    const std::string raw_corrupt = fx.root + "/raw_corrupt";
    CopyTree(fx.pkg_dir, raw_corrupt);
    const std::string raw_path = raw_corrupt + "/frames/cloud_1.raw";
    std::FILE* rf = std::fopen(raw_path.c_str(), "r+b");
    CHECK(rf != nullptr);
    uint8_t rv = 0;
    // REC-26/27: skip the 128 B raw file header; corrupt the first payload byte,
    // which is what the per-frame CRC covers.
    CHECK(std::fseek(rf, 128, SEEK_SET) == 0);
    CHECK(std::fread(&rv, 1, 1, rf) == 1);
    CHECK(std::fseek(rf, 128, SEEK_SET) == 0);
    rv ^= 0x5A;
    CHECK(std::fwrite(&rv, 1, 1, rf) == 1);
    std::fclose(rf);
    replaycli::PackageReport raw_bad;
    std::ostringstream rout;
    CHECK(replaycli::RunVerify(raw_corrupt, false, rout, &raw_bad) !=
          replaycli::kExitOk);
    CHECK(!raw_bad.ok);
    const replaycli::ChannelReport* raw_cloud = raw_bad.Find("cloud_1");
    CHECK(raw_cloud != nullptr);
    CHECK(raw_cloud->crc_bad == 1);
    CHECK(raw_cloud->crc_ok + 1 == exp.cloud_frames.size());
    CHECK(raw_bad.error.find("CRC mismatch") != std::string::npos);
  }

  // ---- C: play --as-fast into fresh targets ----
  {
    TargetSet t;
    t.Create();
    replaycli::PlayOptions opt;
    MakeFullOptions(fx.pkg_dir, t, &opt);
    std::ostringstream out;
    replaycli::PlaySummary sum;
    CHECK(replaycli::RunPlay(opt, nullptr, out, &sum) == replaycli::kExitOk);
    CHECK(sum.ok);
    CHECK(sum.error.empty());
    CHECK(sum.warnings.empty());
    CHECK(sum.events == exp.mono.size());
    CHECK(sum.injected == exp.cloud_frames.size() + exp.aux_frames.size() +
                              exp.samples.size());
    CHECK(sum.skipped == 0);
    CHECK(sum.failed == 0);
    CHECK(sum.unmapped == 0);
    CHECK(sum.mono_regressions == 0);
    CHECK(sum.first_event_ns == first_mono);
    CHECK(sum.last_event_ns == last_mono);
    CHECK(sum.channels[exp.cloud].injected == exp.cloud_frames.size());
    CHECK(sum.channels[exp.cloud].bytes ==
          SumFrameBytes(exp.cloud_frames));
    CHECK(sum.channels[exp.aux].injected == exp.aux_frames.size());
    CHECK(sum.channels[exp.small].injected == exp.samples.size());
    CHECK(sum.channels[exp.meta].ignored == exp.meta_events);
    CHECK(out.str().find("result  : OK") != std::string::npos);

    CheckBigTarget(t.cloud_name, exp.cloud_frames);
    CheckBigTarget(t.aux_name, exp.aux_frames);
    CheckSmallTarget(t.ring.Page(), exp.samples);
    t.Cleanup();

    // JSON summary shape.
    TargetSet tj;
    tj.Create();
    replaycli::PlayOptions jopt;
    MakeFullOptions(fx.pkg_dir, tj, &jopt);
    jopt.json = true;
    std::ostringstream jout;
    replaycli::PlaySummary jsum;
    CHECK(replaycli::RunPlay(jopt, nullptr, jout, &jsum) ==
          replaycli::kExitOk);
    CHECK(jout.str().find("\"command\":\"play\"") != std::string::npos);
    CHECK(jout.str().find("\"injected\":") != std::string::npos);
    CHECK(jout.str().find("\"warnings\":[]") != std::string::npos);
    tj.Cleanup();
  }

  // ---- C2: --preserve-sequences keeps the recorded producer sequence ----
  {
    TargetSet t;
    t.Create();
    replaycli::PlayOptions opt;
    MakeFullOptions(fx.pkg_dir, t, &opt);
    opt.preserve_sequences = true;
    std::ostringstream out;
    replaycli::PlaySummary sum;
    CHECK(replaycli::RunPlay(opt, nullptr, out, &sum) == replaycli::kExitOk);
    CHECK(sum.ok);
    CheckSmallTarget(t.ring.Page(), exp.samples, true);
    t.Cleanup();
  }

  // ---- D: --from/--to window (relative offsets, SeekMonoNs) ----
  {
    const std::size_t n = exp.mono.size();
    const uint64_t from_abs = exp.mono[n / 3];
    const uint64_t to_abs = exp.mono[(n * 2) / 3];
    CHECK(to_abs > from_abs);

    std::vector<uint64_t> ev_per_channel(4, 0);
    std::vector<uint64_t> data_per_channel(4, 0);
    bool have_first_cloud = false;
    FrameExp first_cloud;
    {
      replay::ReplayPackage pkg;
      CHECK(pkg.Open(fx.pkg_dir) == replay::ReplayStatus::kOk);
      replay::ReplayEvent ev;
      while (pkg.NextEvent(&ev) == replay::ReplayStatus::kOk) {
        if (ev.mono_ns < from_abs || ev.mono_ns > to_abs) {
          continue;
        }
        ev_per_channel[ev.channel] += 1;
        if (ev.kind == replay::ReplayKind::kData) {
          data_per_channel[ev.channel] += 1;
          if (ev.channel == exp.cloud && !have_first_cloud) {
            have_first_cloud = true;
            first_cloud.mono_ns = ev.mono_ns;
            first_cloud.len = ev.len;
            first_cloud.flags = ev.frame_flags;
            first_cloud.bytes.assign(ev.payload, ev.payload + ev.len);
          }
        }
      }
    }
    uint64_t expected_events = 0;
    for (const uint64_t c : ev_per_channel) {
      expected_events += c;
    }
    CHECK(expected_events > 0);

    TargetSet t;
    t.Create();
    replaycli::PlayOptions opt;
    MakeFullOptions(fx.pkg_dir, t, &opt);
    opt.has_from = true;
    opt.from.absolute = false;
    opt.from.ns = from_abs - first_mono;
    opt.has_to = true;
    opt.to.absolute = false;
    opt.to.ns = to_abs - first_mono;
    std::ostringstream out;
    replaycli::PlaySummary sum;
    CHECK(replaycli::RunPlay(opt, nullptr, out, &sum) == replaycli::kExitOk);
    CHECK(sum.ok);
    CHECK(sum.events == expected_events);
    CHECK(sum.first_event_ns == from_abs);
    CHECK(sum.last_event_ns <= to_abs);
    CHECK(sum.window_closed);
    CHECK(!sum.stopped);
    CHECK(sum.injected == data_per_channel[exp.cloud] +
                              data_per_channel[exp.aux] +
                              data_per_channel[exp.small]);
    CHECK(sum.channels[exp.cloud].injected == data_per_channel[exp.cloud]);
    CHECK(sum.channels[exp.aux].injected == data_per_channel[exp.aux]);
    CHECK(sum.channels[exp.small].injected == data_per_channel[exp.small]);
    CHECK(sum.channels[exp.cloud].events == ev_per_channel[exp.cloud]);
    CHECK(out.str().find("closed by --to") != std::string::npos);

    if (have_first_cloud) {
      PinnedSlotReader reader;
      CHECK(reader.Open(t.cloud_name.c_str(), 2000000000ULL,
                        /*lease_read_only=*/true));
      PinnedSlotReader::View v;
      CHECK(reader.ReadFrame(0, v) == PinnedSlotReader::ReadResult::kOk);
      CHECK(v.mono_ns == first_cloud.mono_ns);
      CHECK(v.data_len == first_cloud.len);
      CHECK(v.flags == first_cloud.flags);
      CHECK(std::memcmp(v.payload, first_cloud.bytes.data(),
                        first_cloud.len) == 0);
    }
    t.Cleanup();
  }

  // ---- E: --step delivers exactly one event ----
  {
    TargetSet t;
    t.Create();
    replaycli::PlayOptions opt;
    MakeFullOptions(fx.pkg_dir, t, &opt);
    opt.step_once = true;
    std::ostringstream out;
    replaycli::PlaySummary sum;
    CHECK(replaycli::RunPlay(opt, nullptr, out, &sum) == replaycli::kExitOk);
    CHECK(sum.ok);
    CHECK(sum.step_done);
    CHECK(sum.events == 1);
    CHECK(sum.injected + sum.skipped + sum.ignored + sum.failed == 1);
    CHECK(out.str().find("exactly one event") != std::string::npos);
    t.Cleanup();
  }

  // ---- F: missing target: skip (default) vs --strict-targets ----
  {
    TargetSet t;
    t.Create();
    replaycli::PlayOptions opt;
    opt.package_dir = fx.pkg_dir;
    AddPinnedSlotTarget(&opt, "cloud_1", t.cloud_name);
    AddSmallTarget(&opt, "small", 0, t.ring_name, 0);
    opt.as_fast = true;
    opt.pinned_slot_slots = 256;
    std::ostringstream out;
    replaycli::PlaySummary sum;
    CHECK(replaycli::RunPlay(opt, nullptr, out, &sum) == replaycli::kExitOk);
    CHECK(sum.ok);
    CHECK(sum.warnings.size() == 1);
    CHECK(sum.warnings[0].find("aux_0") != std::string::npos);
    CHECK(sum.channels[exp.aux].skipped == exp.aux_frames.size());
    CHECK(sum.channels[exp.aux].unmapped == exp.aux_frames.size());
    CHECK(sum.unmapped == exp.aux_frames.size());
    CHECK(sum.skipped == exp.aux_frames.size());
    CHECK(sum.injected == exp.cloud_frames.size() + exp.samples.size());
    CHECK(out.str().find("warning:") != std::string::npos);
    t.Cleanup();

    TargetSet t2;
    t2.Create();
    replaycli::PlayOptions strict_opt;
    strict_opt.package_dir = fx.pkg_dir;
    AddPinnedSlotTarget(&strict_opt, "cloud_1", t2.cloud_name);
    AddSmallTarget(&strict_opt, "small", 0, t2.ring_name, 0);
    strict_opt.as_fast = true;
    strict_opt.pinned_slot_slots = 256;
    strict_opt.strict_targets = true;
    std::ostringstream sout;
    replaycli::PlaySummary ssum;
    CHECK(replaycli::RunPlay(strict_opt, nullptr, sout, &ssum) ==
          replaycli::kExitTargets);
    CHECK(!ssum.ok);
    CHECK(ssum.events == 0);
    CHECK(ssum.error.find("aux_0") != std::string::npos);
    CHECK(sout.str().find("--strict-targets") != std::string::npos);
    // No target was created for the failed strict run.
    {
      PinnedSlotReader probe;
      CHECK(!probe.Open(t2.cloud_name.c_str(), 20000000ULL,
                        /*lease_read_only=*/true));
    }
    t2.Cleanup();
  }

  // ---- G: an existing big-frame channel is refused without --force ----
  {
    const std::string live_name = UniqueShmName("live");
    TargetSet::UnlinkBig(live_name);
    PinnedSlotWriter live;
    CHECK(live.Create(live_name.c_str(), kFrameSlotBytes, 8, 0));
    replaycli::PlayOptions opt;
    opt.package_dir = fx.pkg_dir;
    AddPinnedSlotTarget(&opt, "cloud_1", live_name);
    opt.as_fast = true;
    std::ostringstream out;
    replaycli::PlaySummary sum;
    CHECK(replaycli::RunPlay(opt, nullptr, out, &sum) ==
          replaycli::kExitTargets);
    CHECK(!sum.ok);
    CHECK(sum.error.find("already exists") != std::string::npos);
    CHECK(out.str().find("--force") != std::string::npos);
    live.Close();
    TargetSet::UnlinkBig(live_name);
  }

  // ---- G1: --force attaches to a compatible live channel ----
  {
    const std::string live_name = UniqueShmName("attach");
    TargetSet::UnlinkBig(live_name);
    PinnedSlotWriter live;
    CHECK(live.Create(live_name.c_str(), kFrameSlotBytes - 16, 8, 0));
    replaycli::PlayOptions opt;
    opt.package_dir = fx.pkg_dir;
    AddPinnedSlotTarget(&opt, "cloud_1", live_name);
    opt.as_fast = true;
    opt.force = true;
    std::ostringstream out;
    replaycli::PlaySummary sum;
    CHECK(replaycli::RunPlay(opt, nullptr, out, &sum) == replaycli::kExitOk);
    CHECK(sum.ok);
    CHECK(sum.channels[exp.cloud].injected == exp.cloud_frames.size());
    CHECK(live.head() == exp.cloud_frames.size());
    live.Close();
    TargetSet::UnlinkBig(live_name);
  }

  // ---- G2 (REP-10): --force cannot steal a channel from a live replay ----
  {
    const std::string gated_name = UniqueShmName("gated");
    TargetSet::UnlinkBig(gated_name);
    PinnedSlotWriter holder;
    CHECK(holder.Create(gated_name.c_str(), kFrameSlotBytes, 8, 0));
    const uint64_t replay_token = ProducerGateNewToken();
    CHECK(ProducerGateAcquire(*holder.gate(), replay_token,
                              ProducerGateNowNs(), 60000000000ULL) ==
          GateResult::kOk);

    replaycli::PlayOptions opt;
    opt.package_dir = fx.pkg_dir;
    AddPinnedSlotTarget(&opt, "cloud_1", gated_name);
    opt.as_fast = true;
    opt.force = true;  // explicit replace, but a live gate still wins
    std::ostringstream out;
    replaycli::PlaySummary sum;
    CHECK(replaycli::RunPlay(opt, nullptr, out, &sum) ==
          replaycli::kExitTargets);
    CHECK(!sum.ok);
    CHECK(sum.error.find("gated") != std::string::npos);
    // The holder is untouched and still owns the gate: the failed replay must
    // not unlink a channel it did not create.
    CHECK(holder.gate()->owner_token.load(std::memory_order_acquire) ==
          replay_token);
    {
      PinnedSlotReader reader;
      CHECK(reader.Open(gated_name.c_str(), 200000000ULL,
                        /*lease_read_only=*/true));
    }
    CHECK(ProducerGateRelease(*holder.gate(), replay_token) ==
          GateResult::kOk);
    holder.Close();
    TargetSet::UnlinkBig(gated_name);
  }

  // ---- G3 (REP-10): --no-gate is an explicit, warned escape hatch ----
  {
    TargetSet t;
    t.Create();
    replaycli::PlayOptions opt;
    MakeFullOptions(fx.pkg_dir, t, &opt);
    opt.gate = false;
    std::ostringstream out;
    replaycli::PlaySummary sum;
    CHECK(replaycli::RunPlay(opt, nullptr, out, &sum) == replaycli::kExitOk);
    CHECK(sum.ok);
    bool warned = false;
    for (const std::string& w : sum.warnings) {
      if (w.find("--no-gate") != std::string::npos) warned = true;
    }
    CHECK(warned);
    CHECK(sum.gate_rejects == 0);
    t.Cleanup();
  }

  // ---- G4 (REP-10): setup errors release gates acquired for both types ----
  {
    TargetSet t;
    t.Create();
    replaycli::PlayOptions opt;
    MakeFullOptions(fx.pkg_dir, t, &opt);
    opt.status_file = fx.root + "/missing-status-dir/status.json";
    std::ostringstream out;
    replaycli::PlaySummary sum;
    CHECK(replaycli::RunPlay(opt, nullptr, out, &sum) ==
          replaycli::kExitFailure);
    CHECK(t.ring.Page()
              ->producer_gate(0)
              ->owner_token.load(std::memory_order_acquire) == 0);

    opt.status_file.clear();
    opt.force = true;
    replaycli::PlaySummary retry_sum;
    std::ostringstream retry_out;
    CHECK(replaycli::RunPlay(opt, nullptr, retry_out, &retry_sum) ==
          replaycli::kExitOk);
    t.Cleanup();
  }

  // ---- I (REP-08): status document, final summary, runtime controls ----
  {
    auto read_all = [](const std::string& path) {
      std::ifstream in(path, std::ios::binary);
      std::ostringstream body;
      body << in.rdbuf();
      return body.str();
    };
    const std::string status_path = fx.root + "/status.json";
    std::remove(status_path.c_str());

    replaycli::PlayControl control;
    replaycli::PlayOptions opt;
    opt.package_dir = fx.pkg_dir;
    opt.as_fast = true;
    opt.status_file = status_path;
    opt.status_interval_ms = 20;
    const std::size_t mid = exp.mono.size() / 2;
    const uint64_t seek_target = exp.mono[mid];
    uint64_t expected_first = exp.mono.back();
    for (const uint64_t m : exp.mono) {
      if (m >= seek_target) {
        expected_first = m;
        break;
      }
    }
    control.RequestSeek(seek_target);
    control.RequestRate(2.0);
    std::ostringstream out;
    replaycli::PlaySummary sum;
    CHECK(replaycli::RunPlay(opt, &control, out, &sum) == replaycli::kExitOk);
    CHECK(sum.ok);
    CHECK(sum.rate == 2.0);
    CHECK(sum.mode == "as-fast");
    CHECK(sum.state == "finished");
    CHECK(sum.first_event_ns == expected_first);
    CHECK(sum.status_file == status_path);
    std::string doc = read_all(status_path);
    CHECK(doc.find("\"state\":\"finished\"") != std::string::npos);
    CHECK(doc.find("\"mode\":\"as-fast\"") != std::string::npos);
    CHECK(doc.find("\"rate\":2.000") != std::string::npos);
    CHECK(doc.find("\"injected\":") != std::string::npos);
    CHECK(doc.find("\"channels\":[") != std::string::npos);
    CHECK(out.str().find("state=finished") != std::string::npos);

    std::ostringstream jout;
    std::ostringstream jerr;
    CHECK(replaycli::RunCli({"status", "--status-file", status_path, "--json"},
                            jout, jerr, nullptr) == replaycli::kExitOk);
    CHECK(jout.str().find("\"state\":\"finished\"") != std::string::npos);

    std::ostringstream tout;
    CHECK(replaycli::RunCli({"status", "--status-file", status_path}, tout,
                            jerr, nullptr) == replaycli::kExitOk);
    CHECK(tout.str().find("status  : finished") != std::string::npos);

    std::ostringstream missing;
    CHECK(replaycli::RunCli({"status", "--status-file",
                             fx.root + "/no-status.json"},
                            missing, jerr, nullptr) ==
          replaycli::kExitFailure);

    const std::string bogus_path = fx.root + "/bogus.json";
    {
      std::FILE* f = std::fopen(bogus_path.c_str(), "wb");
      CHECK(f != nullptr);
      CHECK(std::fwrite("{\"a\":1}", 1, 7, f) == 7);
      std::fclose(f);
    }
    std::ostringstream bogus;
    CHECK(replaycli::RunCli({"status", "--status-file", bogus_path}, bogus,
                            jerr, nullptr) == replaycli::kExitFailure);
  }

  // ---- J (REP-08): live paused status + runtime seek/rate/stop ----
  {
    auto read_all = [](const std::string& path) {
      std::ifstream in(path, std::ios::binary);
      std::ostringstream body;
      body << in.rdbuf();
      return body.str();
    };
    const std::string live_path = fx.root + "/live.json";
    std::remove(live_path.c_str());
    replaycli::PlayControl control;
    replaycli::PlayOptions opt;
    opt.package_dir = fx.pkg_dir;
    opt.as_fast = true;
    opt.start_paused = true;
    opt.status_file = live_path;
    opt.status_interval_ms = 10;
    std::ostringstream out;
    replaycli::PlaySummary sum;
    std::atomic<bool> done{false};
    std::thread play_thread([&] {
      replaycli::RunPlay(opt, &control, out, &sum);
      done.store(true, std::memory_order_release);
    });

    uint64_t deadline = NowNs() + 5000000000ULL;
    bool paused_seen = false;
    while (NowNs() < deadline) {
      if (read_all(live_path).find("\"state\":\"paused\"") !=
          std::string::npos) {
        paused_seen = true;
        break;
      }
      SleepNs(10000000);
    }
    CHECK(paused_seen);

    const uint64_t live_target = exp.mono[exp.mono.size() / 4];
    control.RequestSeek(live_target);
    control.RequestRate(4.0);
    bool applied = false;
    deadline = NowNs() + 5000000000ULL;
    while (NowNs() < deadline) {
      const std::string doc = read_all(live_path);
      if (doc.find("\"rate\":4.000") != std::string::npos &&
          doc.find("\"position_mono_ns\":" + std::to_string(live_target)) !=
              std::string::npos &&
          doc.find("\"state\":\"paused\"") != std::string::npos) {
        applied = true;
        break;
      }
      SleepNs(10000000);
    }
    CHECK(applied);

    control.RequestStop();
    play_thread.join();
    CHECK(done.load(std::memory_order_acquire));
    CHECK(sum.stopped);
    const std::string final_doc = read_all(live_path);
    CHECK(final_doc.find("\"state\":\"stopped\"") != std::string::npos);
  }

  // ---- H: argv dispatch ----
  {
    std::ostringstream out;
    std::ostringstream err;
    CHECK(replaycli::RunCli({"verify", fx.pkg_dir, "--json"}, out, err,
                            nullptr) == replaycli::kExitOk);
    CHECK(out.str().find("\"command\":\"verify\"") != std::string::npos);

    std::ostringstream o2;
    std::ostringstream e2;
    CHECK(replaycli::RunCli({"bogus"}, o2, e2, nullptr) ==
          replaycli::kExitUsage);
    CHECK(e2.str().find("unknown command") != std::string::npos);

    std::ostringstream o3;
    std::ostringstream e3;
    CHECK(replaycli::RunCli({"list", fx.pkg_dir, "--rate", "0"}, o3, e3,
                            nullptr) == replaycli::kExitUsage);

    std::ostringstream o4;
    std::ostringstream e4;
    CHECK(replaycli::RunCli({"list", fx.pkg_dir, "--nope"}, o4, e4, nullptr) ==
          replaycli::kExitUsage);

    std::ostringstream o5;
    std::ostringstream e5;
    CHECK(replaycli::RunCli({"help"}, o5, e5, nullptr) == replaycli::kExitOk);
    CHECK(o5.str().find("usage: thyme-replay") != std::string::npos);
    CHECK(o5.str().find("status") != std::string::npos);
    CHECK(o5.str().find("export") != std::string::npos);
    CHECK(o5.str().find("--status-file") != std::string::npos);
    CHECK(o5.str().find("--preserve-sequences") != std::string::npos);
    CHECK(o5.str().find("rate <r>") != std::string::npos);
    CHECK(o5.str().find("seek <t>") != std::string::npos);

    // The play argv surface wires --status-file through RunCli too.
    const std::string cli_status = fx.root + "/cli_status.json";
    std::remove(cli_status.c_str());
    std::ostringstream o6;
    CHECK(replaycli::RunCli({"play", fx.pkg_dir, "--as-fast", "--json",
                             "--preserve-sequences", "--status-file",
                             cli_status},
                            o6, e5, nullptr) == replaycli::kExitOk);
    CHECK(o6.str().find("\"state\":\"finished\"") != std::string::npos);
    struct stat sst {};
    CHECK(stat(cli_status.c_str(), &sst) == 0 && sst.st_size > 0);
  }

  // ---- K (REP-13): absolute directory/explicit slices and join accounting ----
  {
    const std::string root = fx.root + "/absolute";
    CHECK(mkdir(root.c_str(), 0700) == 0);
    const std::vector<std::string> dirs{
        root + "/continuous_z", root + "/continuous_a", root + "/continuous_m"};
    const std::vector<SliceSpec> specs{
        {{1000, 3000, 5000}, {2000, 4000, 6000}},
        {{4000, 6000, 8000}, {5000, 7000, 10000}},
        {{14000, 16000}, {15000, 17000}}};
    const uint64_t sequences[3] = {21, 20, 22};
    const std::size_t order[3] = {1, 0, 2};
    for (std::size_t i = 0; i < dirs.size(); ++i) {
      WriteSlicePackage(dirs[i], specs[order[i]], sequences[i]);
    }
    const std::vector<uint64_t> all_small{1000, 3000, 5000, 6000,
                                          8000, 14000, 16000};
    const std::vector<uint64_t> all_frames{2000, 4000, 6000, 7000,
                                           10000, 15000, 17000};

    SliceTarget discovered;
    discovered.Create();
    replaycli::PlayOptions opt = discovered.Options(root);
    opt.json = true;
    opt.status_file = root + "/absolute-status.json";
    std::ostringstream out;
    replaycli::PlaySummary sum;
    CHECK(replaycli::RunPlay(opt, nullptr, out, &sum) == replaycli::kExitOk);
    CHECK(sum.timeline == "absolute");
    CHECK(sum.events == 16 && sum.injected == 14 && sum.ignored == 2);
    CHECK(sum.slice_joins == 2 && sum.join_duplicates == 3 &&
          sum.join_holes == 1 && sum.mono_regressions == 0);
    CHECK(out.str().find("\"timeline\":\"absolute\"") != std::string::npos);
    CHECK(out.str().find("\"slice_joins\":2") != std::string::npos);
    CHECK(out.str().find("\"join_duplicates\":3") != std::string::npos);
    CHECK(out.str().find("\"join_holes\":1") != std::string::npos);
    discovered.Check(all_small, all_frames);
    std::ifstream status_in(opt.status_file);
    std::ostringstream status_body;
    status_body << status_in.rdbuf();
    CHECK(status_body.str().find("\"timeline\":\"absolute\"") !=
          std::string::npos);
    CHECK(status_body.str().find("\"slice_joins\":2") != std::string::npos);
    std::ostringstream status_text;
    std::ostringstream status_err;
    CHECK(replaycli::RunCli({"status", "--status-file", opt.status_file},
                            status_text, status_err, nullptr) ==
          replaycli::kExitOk);
    CHECK(status_text.str().find("timeline=absolute") != std::string::npos);
    CHECK(status_text.str().find("duplicates=3") != std::string::npos);
    discovered.Cleanup();

    SliceTarget explicit_target;
    explicit_target.Create();
    std::ostringstream explicit_out;
    std::ostringstream explicit_err;
    CHECK(replaycli::RunCli(
              {"play", dirs[0], dirs[1], dirs[2], "--timeline=absolute",
               "--as-fast", "--strict-targets", "--pinned_slot",
               "cam=" + explicit_target.frame_name, "--small",
               "small:0=" + explicit_target.ring_name + ":0"},
              explicit_out, explicit_err, nullptr) == replaycli::kExitOk);
    CHECK(explicit_out.str().find("timeline=absolute") != std::string::npos);
    CHECK(explicit_out.str().find("joins   : 2  duplicates=3  holes=1") !=
          std::string::npos);
    explicit_target.Check(all_small, all_frames);
    explicit_target.Cleanup();

    SliceTarget sought;
    sought.Create();
    replaycli::PlayOptions seek_opt = sought.Options(dirs[0]);
    seek_opt.package_dirs = dirs;
    seek_opt.has_seek = true;
    seek_opt.seek.absolute = true;
    seek_opt.seek.ns = 8000;
    seek_opt.has_to = true;
    seek_opt.to.absolute = true;
    seek_opt.to.ns = 14000;
    replaycli::PlaySummary seek_sum;
    std::ostringstream seek_out;
    CHECK(replaycli::RunPlay(seek_opt, nullptr, seek_out, &seek_sum) ==
          replaycli::kExitOk);
    CHECK(seek_sum.first_event_ns == 8000 && seek_sum.last_event_ns == 14000);
    CHECK(seek_sum.slice_joins == 1 && seek_sum.join_holes == 1 &&
          seek_sum.join_duplicates == 0 && seek_sum.mono_regressions == 0);
    sought.Check({8000, 14000}, {10000});
    sought.Cleanup();

    SliceTarget bounded;
    bounded.Create();
    replaycli::PlayOptions to_opt = bounded.Options(root);
    to_opt.has_to = true;
    to_opt.to.absolute = true;
    to_opt.to.ns = 7000;
    replaycli::PlaySummary to_sum;
    std::ostringstream to_out;
    CHECK(replaycli::RunPlay(to_opt, nullptr, to_out, &to_sum) ==
          replaycli::kExitOk);
    CHECK(to_sum.window_closed && to_sum.last_event_ns == 7000);
    CHECK(to_sum.injected == 8 && to_sum.join_duplicates == 3 &&
          to_sum.mono_regressions == 0);
    bounded.Check({1000, 3000, 5000, 6000}, {2000, 4000, 6000, 7000});
    bounded.Cleanup();

    SliceTarget gated;
    gated.Create();
    replaycli::PlayOptions first_opt = gated.Options(root);
    first_opt.timeline = "stitch";
    first_opt.start_paused = true;
    first_opt.status_file = root + "/busy-status.json";
    replaycli::PlayControl control;
    replaycli::PlaySummary first_sum;
    std::ostringstream first_out;
    std::thread first_thread([&] {
      CHECK(replaycli::RunPlay(first_opt, &control, first_out, &first_sum) ==
            replaycli::kExitOk);
    });
    bool paused = false;
    const uint64_t deadline = NowNs() + 5000000000ULL;
    while (NowNs() < deadline) {
      std::ifstream in(first_opt.status_file);
      std::ostringstream body;
      body << in.rdbuf();
      if (body.str().find("\"state\":\"paused\"") != std::string::npos) {
        paused = true;
        break;
      }
      SleepNs(10000000);
    }
    CHECK(paused);
    replaycli::PlayOptions second_opt = first_opt;
    second_opt.start_paused = false;
    second_opt.status_file.clear();
    second_opt.force = true;
    replaycli::PlaySummary second_sum;
    std::ostringstream second_out;
    CHECK(replaycli::RunPlay(second_opt, nullptr, second_out, &second_sum) ==
          replaycli::kExitTargets);
    CHECK(second_sum.injected == 0);
    CHECK(second_sum.error.find("gated") != std::string::npos ||
          second_sum.error.find("busy") != std::string::npos);
    control.RequestStop();
    first_thread.join();
    CHECK(first_sum.stopped && first_sum.injected == 0);
    CHECK(gated.ring.Page()
              ->producer_gate(0)
              ->owner_token.load(std::memory_order_acquire) == 0);
    replaycli::PlayOptions third_opt = second_opt;
    replaycli::PlaySummary third_sum;
    std::ostringstream third_out;
    CHECK(replaycli::RunPlay(third_opt, nullptr, third_out, &third_sum) ==
          replaycli::kExitOk);
    const std::vector<uint64_t> stitch_small{1000, 3000, 5000, 7000,
                                             9000, 11000, 14000, 16000};
    const std::vector<uint64_t> stitch_frames{2000, 4000, 6000, 8000,
                                              10000, 13000, 15000, 17000};
    const std::vector<uint64_t> stitch_source_small{
        1000, 3000, 5000, 4000, 6000, 8000, 14000, 16000};
    const std::vector<uint64_t> stitch_source_frames{
        2000, 4000, 6000, 5000, 7000, 10000, 15000, 17000};
    gated.Check(stitch_small, stitch_frames, &stitch_source_small,
                &stitch_source_frames);
    gated.Cleanup();

    SliceTarget stitched;
    stitched.Create();
    replaycli::PlayOptions stitch_opt = stitched.Options(root);
    stitch_opt.timeline = "stitch";
    stitch_opt.json = true;
    stitch_opt.status_file = root + "/stitch-status.json";
    replaycli::PlaySummary stitch_sum;
    std::ostringstream stitch_out;
    CHECK(replaycli::RunPlay(stitch_opt, nullptr, stitch_out, &stitch_sum) ==
          replaycli::kExitOk);
    CHECK(stitch_sum.timeline == "stitch");
    CHECK(stitch_sum.events == 19 && stitch_sum.injected == 16);
    CHECK(stitch_sum.first_event_ns == 500 && stitch_sum.last_event_ns == 17000);
    CHECK(stitch_sum.join_duplicates == 0 && stitch_sum.join_holes == 1 &&
          stitch_sum.mono_regressions == 0);
    CHECK(stitch_out.str().find("\"timeline\":\"stitch\"") != std::string::npos);
    stitched.Check(stitch_small, stitch_frames, &stitch_source_small,
                   &stitch_source_frames);
    stitched.Cleanup();

    SliceTarget stitch_text;
    stitch_text.Create();
    std::ostringstream stitch_text_out;
    std::ostringstream stitch_text_err;
    CHECK(replaycli::RunCli(
              {"play", root, "--timeline=stitch", "--as-fast",
               "--strict-targets", "--pinned_slot",
               "cam=" + stitch_text.frame_name, "--small",
               "small:0=" + stitch_text.ring_name + ":0"},
              stitch_text_out, stitch_text_err, nullptr) == replaycli::kExitOk);
    CHECK(stitch_text_out.str().find("timeline=stitch") != std::string::npos);
    stitch_text.Check(stitch_small, stitch_frames, &stitch_source_small,
                      &stitch_source_frames);
    stitch_text.Cleanup();

    SliceTarget stitch_sought;
    stitch_sought.Create();
    replaycli::PlayOptions stitch_seek = stitch_sought.Options(root);
    stitch_seek.timeline = "stitch";
    stitch_seek.has_seek = true;
    stitch_seek.seek.absolute = true;
    stitch_seek.seek.ns = 13000;
    stitch_seek.has_to = true;
    stitch_seek.to.absolute = true;
    stitch_seek.to.ns = 16000;
    replaycli::PlaySummary stitch_seek_sum;
    std::ostringstream stitch_seek_out;
    CHECK(replaycli::RunPlay(stitch_seek, nullptr, stitch_seek_out,
                             &stitch_seek_sum) == replaycli::kExitOk);
    CHECK(stitch_seek_sum.first_event_ns == 13000);
    CHECK(stitch_seek_sum.last_event_ns == 16000);
    CHECK(stitch_seek_sum.window_closed && stitch_seek_sum.slice_joins == 1);
    CHECK(stitch_seek_sum.join_duplicates == 0 &&
          stitch_seek_sum.mono_regressions == 0);
    const std::vector<uint64_t> seek_source_small{14000, 16000};
    const std::vector<uint64_t> seek_source_frames{10000, 15000};
    stitch_sought.Check({14000, 16000}, {13000, 15000}, &seek_source_small,
                        &seek_source_frames);
    stitch_sought.Cleanup();
  }

  // THYME_REP08_KEEP_PKG=1 keeps the recorded package for a manual
  // `thyme-replay list/info/verify/play` run (shm producers are still
  // unlinked; the package directory is otherwise deleted).
  fx.CleanupShm();
  if (std::getenv("THYME_REP08_KEEP_PKG") != nullptr) {
    std::printf("package: %s\n", fx.pkg_dir.c_str());
  } else {
    RmTree(fx.root);
  }
  std::printf("test_replay_cli: OK\n");
  return 0;
}
