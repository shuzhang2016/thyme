#include <dirent.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <limits>
#include <string>
#include <vector>

#include "thyme/check.h"
#include "thyme/contracts/frame_header.h"
#include "thyme/record/record_mirror_ring_client.h"
#include "thyme/record/thyme_bin.h"
#include "thyme/record/triggered_recorder.h"
#include "thyme/replay/pinned_slot_injector.h"
#include "thyme/replay/replay_playlist.h"
#include "thyme/runtime/pinned_slot_store.h"
#include "thyme/runtime/shm_region.h"

using namespace thyme;

namespace {

struct Pod16 {
  uint64_t a;
  uint32_t b;
  uint32_t c;
};

struct SliceSpec {
  std::vector<uint64_t> small;
  std::vector<uint64_t> frames;
};

uint64_t NowNs() {
  timespec ts {};
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<uint64_t>(ts.tv_sec) * 1000000000ULL + ts.tv_nsec;
}

void RmTree(const std::string& path) {
  DIR* dir = opendir(path.c_str());
  if (dir == nullptr) {
    return;
  }
  while (dirent* entry = readdir(dir)) {
    if (entry->d_name[0] == '.') {
      continue;
    }
    const std::string child = path + "/" + entry->d_name;
    struct stat st {};
    if (stat(child.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) {
      RmTree(child);
    } else {
      std::remove(child.c_str());
    }
  }
  closedir(dir);
  rmdir(path.c_str());
}

ThymeStructDesc Desc(uint16_t id, const char* name, uint32_t size,
                     uint16_t version = 1) {
  ThymeStructDesc desc{};
  desc.struct_id = id;
  desc.contract_version = version;
  desc.payload_size = size;
  std::snprintf(desc.name, sizeof(desc.name), "%s", name);
  return desc;
}

void WriteWal(const std::string& dir, uint64_t seq) {
  std::FILE* file = std::fopen((dir + "/wal.log").c_str(), "wb");
  CHECK(file != nullptr);
  const triggered::WalHeader header{{'T', 'R', 'I', 'G', 'W', 'A', 'L', '1'},
                                    triggered::kContinuousWalMark | seq};
  const triggered::WalRow seal{triggered::kWalOpSeal, 0, 0, 0, 0, 0};
  CHECK(std::fwrite(&header, sizeof(header), 1, file) == 1);
  CHECK(std::fwrite(&seal, sizeof(seal), 1, file) == 1);
  CHECK(std::fclose(file) == 0);
}

std::vector<uint8_t> FramedPayload(uint32_t body_size, uint64_t source_ns) {
  std::vector<uint8_t> bytes(sizeof(FrameHeader) + body_size, 0);
  std::memcpy(bytes.data() + sizeof(FrameHeader), &source_ns, sizeof(source_ns));
  FrameHeader header{};
  header.session_id = 0x1313;
  header.mono_ns = source_ns - 1;
  header.sequence = 77;
  header.size = static_cast<uint16_t>(bytes.size());
  FrameHeaderSeal(header, bytes.data() + sizeof(FrameHeader), body_size,
                  0x70426B21u, 3);
  std::memcpy(bytes.data(), &header, sizeof(header));
  return bytes;
}

void WritePackage(const std::string& dir, const SliceSpec& spec, uint64_t seq,
                  uint16_t small_version = 1, bool framed = false) {
  CHECK(mkdir(dir.c_str(), 0700) == 0);
  CHECK(mkdir((dir + "/frames").c_str(), 0700) == 0);
  const uint64_t first =
      std::min(spec.small.front(), spec.frames.front()) - 500;
  const ThymeStructDesc meta = Desc(63, "Meta", 8);
  const uint32_t small_size =
      framed ? sizeof(FrameHeader) + sizeof(Pod16) : sizeof(Pod16);
  const ThymeStructDesc small = Desc(0, "Pod16", small_size, small_version);
  const ThymeStructDesc frame = Desc(triggered::kFrameIndexStructId, "FrameIdx",
                                     sizeof(triggered::FrameIndexRecord));
  const ThymeStructDesc gap = Desc(triggered::kFrameGapStructId, "FrameGap", 0);
  {
    ThymeBinFileSink sink((dir + "/meta.tbin").c_str());
    ThymeBinEncoder encoder(&sink);
    CHECK(encoder.WriteHeader(seq, first, &meta, 1));
    if (framed && seq != 40) {
      CHECK(encoder.WriteTrigger(seq, first, 0, 0));
    } else {
      CHECK(encoder.WriteData(0, meta.struct_id, first, 0, &first,
                              sizeof(first)));
    }
    CHECK(encoder.Finish());
    CHECK(sink.Sync());
  }
  {
    ThymeBinFileSink sink((dir + "/small.tbin").c_str());
    ThymeBinEncoder encoder(&sink);
    CHECK(encoder.WriteHeader(seq, first, &small, 1));
    for (std::size_t i = 0; i < spec.small.size(); ++i) {
      Pod16 pod{spec.small[i], static_cast<uint32_t>(i), 0};
      const std::vector<uint8_t> framed_bytes =
          framed ? FramedPayload(sizeof(Pod16), spec.small[i])
                 : std::vector<uint8_t>();
      const void *payload = framed
                                ? static_cast<const void *>(framed_bytes.data())
                                : static_cast<const void *>(&pod);
      CHECK(encoder.WriteData(i, small.struct_id, spec.small[i],
                              static_cast<uint32_t>(i), payload, small_size));
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
      const std::vector<uint8_t> framed_bytes =
          framed ? FramedPayload(128 - sizeof(FrameHeader), spec.frames[i])
                 : std::vector<uint8_t>();
      uint8_t bytes[128]{};
      if (framed) {
        std::memcpy(bytes, framed_bytes.data(), sizeof(bytes));
      } else {
      std::memcpy(bytes, &spec.frames[i], sizeof(spec.frames[i]));
      }
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
  WriteWal(dir, seq);
}

struct Target {
  std::string frame_name;
  std::string ring_name;
  PinnedSlotWriter writer;
  record_mirror::RingClient ring;

  void Init(const char *tag, bool framed = false) {
    char name[128];
    std::snprintf(name, sizeof(name), "/thyme_rep13_%s_bf_%ld", tag,
                  static_cast<long>(getpid()));
    frame_name = name;
    std::snprintf(name, sizeof(name), "/thyme_rep13_%s_ring_%ld", tag,
                  static_cast<long>(getpid()));
    ring_name = name;
    ShmRegion::UnlinkName(frame_name.c_str());
    ShmRegion::UnlinkName(
        PinnedSlotWriter::LeaseShmName(frame_name.c_str()).c_str());
    ShmRegion::UnlinkName(ring_name.c_str());
    CHECK(writer.Create(frame_name.c_str(), 1024, 64, 0));
    writer.SetCrcEnabled(true);
    const uint32_t payload_size =
        framed ? sizeof(FrameHeader) + sizeof(Pod16) : sizeof(Pod16);
    const ThymeStructDesc desc = Desc(0, "Pod16", payload_size);
    RecordMirrorRingSpec spec{payload_size, 128};
    CHECK(ring.Create(ring_name.c_str(), &desc, &spec, 1, 13));
  }

  replay::ReplayPlayerConfig Config() {
    replay::ReplayPlayerConfig cfg;
    cfg.pinned_slots.push_back(replay::PinnedSlotTarget{2, &writer});
    replay::SmallStructTarget small;
    small.channel = 1;
    small.struct_id = 0;
    small.ring = ring.Page();
    small.target_struct_id = 0;
    cfg.small_structs.push_back(small);
    cfg.header_policy.session_id = 0x131313;
    return cfg;
  }

  void CheckData(uint64_t small_count, uint64_t frame_count) {
    CHECK(ring.Page()->RingHead(0) == small_count);
    CHECK(writer.head() == frame_count);
    std::vector<uint64_t> expected_small{1000, 3000,  5000, 6000,
                                         8000, 14000, 16000};
    for (uint64_t i = 0; i < small_count && i < expected_small.size(); ++i) {
      Pod16 pod{};
      uint32_t size = 0;
      uint64_t mono = 0;
      uint32_t sequence = 0;
      CHECK(ring.Page()->ReadRingSample(0, i, reinterpret_cast<uint8_t*>(&pod),
                                        sizeof(pod), size, mono, sequence));
      CHECK(size == sizeof(pod));
      CHECK(pod.a == expected_small[i]);
      CHECK(mono == expected_small[i]);
      CHECK(sequence == i);
    }
    PinnedSlotReader reader;
    CHECK(reader.Open(frame_name.c_str(), 1000000000ULL));
    uint64_t previous = 0;
    for (uint64_t i = 0; i < frame_count; ++i) {
      PinnedSlotReader::View view;
      CHECK(reader.ReadFrame(i, view) == PinnedSlotReader::ReadResult::kOk);
      CHECK(view.data_len == 128);
      CHECK(view.mono_ns >= previous);
      uint64_t value = 0;
      std::memcpy(&value, view.payload, sizeof(value));
      CHECK(value == view.mono_ns);
      previous = view.mono_ns;
    }
  }

  void CheckStitched(const std::vector<uint64_t>& small,
                     const std::vector<uint64_t>& frames,
                     const std::vector<uint64_t>& source_small,
                     const std::vector<uint64_t>& source_frames) {
    CHECK(ring.Page()->RingHead(0) == small.size());
    for (std::size_t i = 0; i < small.size(); ++i) {
      uint8_t bytes[sizeof(FrameHeader) + sizeof(Pod16)]{};
      uint32_t size = 0;
      uint64_t mono = 0;
      uint32_t sequence = 0;
      CHECK(ring.Page()->ReadRingSample(0, i, bytes, sizeof(bytes), size, mono,
                                        sequence));
      CHECK(size == sizeof(bytes) && mono == small[i]);
      FrameHeader header{};
      std::memcpy(&header, bytes, sizeof(header));
      CHECK(header.mono_ns == small[i]);
      CHECK(header.session_id == 0x131313);
      CHECK(FrameHeaderOk(header, bytes + sizeof(FrameHeader), sizeof(Pod16),
                          0x70426B21u, 3));
      uint64_t source_ns = 0;
      std::memcpy(&source_ns, bytes + sizeof(FrameHeader), sizeof(source_ns));
      CHECK(source_ns == source_small[i]);
    }
    PinnedSlotReader reader;
    CHECK(reader.Open(frame_name.c_str(), 1000000000ULL));
    for (std::size_t i = 0; i < frames.size(); ++i) {
      PinnedSlotReader::View view;
      CHECK(reader.ReadFrame(i, view) == PinnedSlotReader::ReadResult::kOk);
      CHECK(view.mono_ns == frames[i]);
      FrameHeader header{};
      std::memcpy(&header, view.payload, sizeof(header));
      CHECK(header.mono_ns == frames[i]);
      CHECK(header.session_id == 0x131313);
      CHECK(FrameHeaderOk(header, view.payload + sizeof(FrameHeader),
                          view.data_len - sizeof(FrameHeader), 0x70426B21u, 3));
      uint64_t source_ns = 0;
      std::memcpy(&source_ns, view.payload + sizeof(FrameHeader),
                  sizeof(source_ns));
      CHECK(source_ns == source_frames[i]);
    }
  }

  void Close() {
    writer.Close();
    ring.Shutdown(true);
  }
};

void WriteSinglePackage(const std::string& dir, uint64_t mono_ns) {
  CHECK(mkdir(dir.c_str(), 0700) == 0);
  const ThymeStructDesc meta = Desc(63, "Meta", 8);
  ThymeBinFileSink sink((dir + "/meta.tbin").c_str());
  ThymeBinEncoder encoder(&sink);
  CHECK(encoder.WriteHeader(1, mono_ns, &meta, 1));
  CHECK(encoder.WriteData(0, meta.struct_id, mono_ns, 0, &mono_ns,
                          sizeof(mono_ns)));
  CHECK(encoder.Finish());
  CHECK(sink.Sync());
}

void CheckReleased(Target* target) {
  CHECK(target->writer.gate()->owner_token.load(std::memory_order_acquire) ==
        0);
  PinnedSlotWriter::BeginView view = target->writer.Begin(128);
  CHECK(view.status == PinnedSlotWriter::BeginStatus::kOk);
  CHECK(view.payload != nullptr);
  std::memset(view.payload, 0, 128);
  target->writer.Commit(20000);

  ProducerGate* small_gate = target->ring.Page()->producer_gate(0);
  CHECK(small_gate->owner_token.load(std::memory_order_acquire) == 0);
  Pod16 pod{20000, 0, 0};
  CHECK(target->ring.Page()->TryWriteRing(0, &pod, sizeof(pod), 20000, 0));
}

}  // namespace

int main() {
  const std::string root =
      "/tmp/thyme_rep13_" + std::to_string(static_cast<long>(getpid()));
  RmTree(root);
  CHECK(mkdir(root.c_str(), 0700) == 0);
  const std::vector<std::string> dirs{
      root + "/continuous_z", root + "/continuous_a", root + "/continuous_m"};
  const std::vector<SliceSpec> specs{{{1000, 3000, 5000}, {2000, 4000, 6000}},
                                     {{4000, 6000, 8000}, {5000, 7000, 10000}},
                                     {{14000, 16000}, {15000, 17000}}};
  const uint64_t sequences[3] = {21, 20, 22};
  const std::size_t spec_order[3] = {1, 0, 2};
  for (std::size_t i = 0; i < dirs.size(); ++i) {
    WritePackage(dirs[i], specs[spec_order[i]], sequences[i]);
  }
  const std::string stitch_root = root + "/stitch";
  CHECK(mkdir(stitch_root.c_str(), 0700) == 0);
  const std::vector<std::string> stitch_dirs{stitch_root + "/continuous_a",
                                             stitch_root + "/continuous_b",
                                             stitch_root + "/continuous_c"};
  for (std::size_t i = 0; i < stitch_dirs.size(); ++i) {
    WritePackage(stitch_dirs[i], specs[i], 40 + i, 1, true);
  }

  Target target;
  target.Init("main");
  {
    replay::ReplayPlaylist playlist;
    CHECK(playlist.Open(root, target.Config()) == replay::ReplayStatus::kOk);
    CHECK(playlist.slices().size() == 3);
    CHECK(playlist.slices()[0].dir == dirs[1]);
    CHECK(playlist.slices()[1].dir == dirs[0]);
    CHECK(playlist.slices()[2].dir == dirs[2]);
    CHECK(playlist.slices()[0].slice_seq == 20);
    CHECK(playlist.slices()[0].first_mono_ns == 500);
    CHECK(playlist.slices()[2].last_mono_ns == 17000);
    const uint64_t token =
        target.writer.gate()->owner_token.load(std::memory_order_acquire);
    const uint64_t generation =
        target.writer.gate()->generation.load(std::memory_order_acquire);
    CHECK(token != 0);

    replay::ReplayPlaylist second;
    CHECK(second.Open(dirs, target.Config()) == replay::ReplayStatus::kBusy);
    CHECK(target.writer.head() == 0);
    second.Close();

    CHECK(ProducerGateAcquire(*target.writer.gate(), ProducerGateNewToken(), 1,
                              1000000000ULL) == GateResult::kBusy);
    playlist.player().AsFast(true);
    CHECK(playlist.player().SetRate(4.0) == replay::ReplayStatus::kOk);
    CHECK(playlist.Run() == replay::ReplayStatus::kOk);
    CHECK(playlist.player().as_fast());
    CHECK(playlist.player().rate() == 4.0);
    CHECK(playlist.current_slice() == 2);
    CHECK(playlist.stats().events == 16);
    CHECK(playlist.stats().injected == 14);
    CHECK(playlist.stats().ignored == 2);
    CHECK(playlist.stats().slice_joins == 2);
    CHECK(playlist.stats().join_duplicates == 3);
    CHECK(playlist.stats().join_holes == 1);
    CHECK(playlist.stats().mono_regressions == 0);
    CHECK(playlist.stats().first_event_ns == 500);
    CHECK(playlist.stats().last_event_ns == 17000);
    CHECK(playlist.player().replay_session_id() == 0x131313);
    CHECK(target.writer.gate()->owner_token.load(std::memory_order_acquire) ==
          token);
    CHECK(target.writer.gate()->generation.load(std::memory_order_acquire) ==
          generation);
    target.CheckData(7, 7);

    CHECK(playlist.Seek(8000) == replay::ReplayStatus::kOk);
    CHECK(playlist.current_slice() == 1);
    CHECK(playlist.stats().slice_joins == 2);
    CHECK(playlist.stats().join_holes == 1);
    CHECK(playlist.Run() == replay::ReplayStatus::kOk);
    CHECK(playlist.stats().slice_joins == 3);
    CHECK(playlist.stats().join_holes == 2);
    CHECK(playlist.stats().mono_regressions == 0);
    CHECK(target.writer.head() == 10);
    CHECK(target.ring.Page()->RingHead(0) == 10);
    CHECK(target.writer.gate()->owner_token.load(std::memory_order_acquire) ==
          token);
    playlist.Close();
    CheckReleased(&target);
  }
  target.Close();

  Target paused;
  paused.Init("pause");
  {
    replay::ReplayPlayer player;
    replay::ReplayPlayerConfig cfg = paused.Config();
    CHECK(player.Open(dirs[0], cfg) == replay::ReplayStatus::kOk);
    player.Pause();
    ProducerGate* big_gate = paused.writer.gate();
    ProducerGate* small_gate = paused.ring.Page()->producer_gate(0);
    const uint64_t big_token =
        big_gate->owner_token.load(std::memory_order_acquire);
    const uint64_t small_token =
        small_gate->owner_token.load(std::memory_order_acquire);
    const uint64_t big_heartbeat =
        big_gate->heartbeat_ns.load(std::memory_order_acquire);
    const uint64_t small_heartbeat =
        small_gate->heartbeat_ns.load(std::memory_order_acquire);
    CHECK(big_token != 0);
    CHECK(small_token != 0);
    big_gate->timeout_ns.store(30000000ULL, std::memory_order_release);
    small_gate->timeout_ns.store(30000000ULL, std::memory_order_release);
    for (uint32_t i = 0; i < 12; ++i) {
      usleep(10000);
      player.Heartbeat();
    }
    CHECK(big_gate->owner_token.load(std::memory_order_acquire) == big_token);
    CHECK(small_gate->owner_token.load(std::memory_order_acquire) ==
          small_token);
    CHECK(big_gate->heartbeat_ns.load(std::memory_order_acquire) >
          big_heartbeat);
    CHECK(small_gate->heartbeat_ns.load(std::memory_order_acquire) >
          small_heartbeat);

    CHECK(!ProducerGateAllowsWrite(*big_gate, 0, NowNs()));
    Pod16 pod{20000, 0, 0};
    CHECK(!paused.ring.Page()->TryWriteRing(0, &pod, sizeof(pod), 20000, 0));
    CHECK(player.pinned_slot_injector().gate_rejects() == 1);
    CHECK(player.small_struct_injector().gate_rejects() == 1);
    CHECK(player.pinned_slot_injector().gate_steals() == 0);
    CHECK(player.small_struct_injector().gate_steals() == 0);
    player.Close();
    CheckReleased(&paused);
  }
  paused.Close();

  Target legacy;
  legacy.Init("legacy");
  {
    replay::ReplayPlayer player;
    player.SetGateEnabled(false);
    replay::ReplayPlayerConfig cfg = legacy.Config();
    CHECK(player.Open(dirs[0], cfg) == replay::ReplayStatus::kOk);
    CHECK(legacy.writer.gate()->owner_token.load(std::memory_order_acquire) ==
          0);
    CHECK(legacy.ring.Page()
              ->producer_gate(0)
              ->owner_token.load(std::memory_order_acquire) == 0);
    PinnedSlotWriter::BeginView view = legacy.writer.Begin(128);
    CHECK(view.status == PinnedSlotWriter::BeginStatus::kOk);
    std::memset(view.payload, 0, 128);
    legacy.writer.Commit(20000);
    Pod16 pod{20000, 0, 0};
    CHECK(legacy.ring.Page()->TryWriteRing(0, &pod, sizeof(pod), 20000, 0));
    player.Close();
  }
  legacy.Close();

  Target stopped;
  stopped.Init("stop");
  {
    replay::ReplayPlaylist playlist;
    CHECK(playlist.Open(dirs, stopped.Config()) == replay::ReplayStatus::kOk);
    playlist.player().AsFast(true);
    CHECK(playlist.Pump() == replay::ReplayStatus::kOk);
    playlist.Stop();
    CHECK(playlist.Pump() == replay::ReplayStatus::kStopped);
    playlist.Close();
    CheckReleased(&stopped);
  }
  stopped.Close();

  const std::string bad = root + "/bad";
  WritePackage(bad, specs[2], 23, 2);
  Target incompatible;
  incompatible.Init("bad");
  {
    replay::ReplayPlaylist playlist;
    CHECK(playlist.Open(std::vector<std::string>{dirs[1], bad},
                        incompatible.Config()) ==
          replay::ReplayStatus::kManifestMismatch);
    CHECK(!playlist.error().empty());
    CHECK(incompatible.writer.head() == 0);
    CHECK(incompatible.writer.gate()->owner_token.load(
              std::memory_order_acquire) == 0);
  }
  {
    replay::ReplayPlayer player;
    replay::ReplayPlayerConfig cfg = incompatible.Config();
    CHECK(player.Open(dirs[1], cfg) == replay::ReplayStatus::kOk);
    CHECK(player.OpenNextPackage(bad) ==
          replay::ReplayStatus::kManifestMismatch);
    CHECK(player.stats().injected == 0);
    CHECK(player.stats().slice_joins == 0);
    CHECK(incompatible.writer.head() == 0);
    player.Close();
    CheckReleased(&incompatible);
  }
  incompatible.Close();

  const std::string multi_a = root + "/multi_a";
  const std::string multi_b = root + "/multi_b";
  WritePackage(multi_a, {{1000, 2000, 3000}, {1000, 2000, 3000}}, 30);
  WritePackage(multi_b, {{500, 1500, 2500, 4000}, {500, 1500, 2500, 4000}}, 31);
  Target multi;
  multi.Init("multi");
  {
    replay::ReplayPlaylist playlist;
    CHECK(playlist.Open(std::vector<std::string>{multi_a, multi_b},
                        multi.Config()) == replay::ReplayStatus::kOk);
    playlist.player().AsFast(true);
    CHECK(playlist.Run() == replay::ReplayStatus::kOk);
    CHECK(playlist.stats().join_duplicates == 7);
    CHECK(playlist.stats().injected == 8);
    CHECK(playlist.stats().mono_regressions == 0);
    CHECK(multi.writer.head() == 4);
    CHECK(multi.ring.Page()->RingHead(0) == 4);
    playlist.Close();
    CheckReleased(&multi);
  }
  {
    replay::ReplayPlayer player;
    replay::ReplayPlayerConfig cfg = multi.Config();
    cfg.skip_overlaps = false;
    CHECK(player.Open(multi_a, cfg) == replay::ReplayStatus::kOk);
    player.AsFast(true);
    CHECK(player.Run() == replay::ReplayStatus::kOk);
    CHECK(player.OpenNextPackage(multi_b) == replay::ReplayStatus::kOk);
    CHECK(player.Run() == replay::ReplayStatus::kOk);
    CHECK(player.stats().join_duplicates == 0);
    CHECK(player.stats().injected == 14);
    CHECK(player.stats().mono_regressions > 0);
    player.Close();
    CheckReleased(&multi);
  }
  multi.Close();

  Target stitched;
  stitched.Init("stitched", true);
  {
    replay::ReplayPlayerConfig cfg = stitched.Config();
    cfg.stitch_timeline = true;
    replay::ReplayPlaylist playlist;
    CHECK(playlist.Open(stitch_root, cfg) == replay::ReplayStatus::kOk);
    CHECK(playlist.slices()[0].sample_period_ns == 500);
    CHECK(playlist.slices()[1].sample_period_ns == 1000);
    CHECK(playlist.slices()[0].timeline_first_ns == 500);
    CHECK(playlist.slices()[0].timeline_last_ns == 6000);
    CHECK(playlist.slices()[1].timeline_first_ns == 7000);
    CHECK(playlist.slices()[1].timeline_last_ns == 13500);
    CHECK(playlist.slices()[2].timeline_first_ns == 14500);
    CHECK(playlist.slices()[2].timeline_last_ns == 18000);
    CHECK(playlist.slices()[1].offset_ns == 3500);
    replay::ReplayPlaylist second;
    CHECK(second.Open(stitch_root, cfg) == replay::ReplayStatus::kBusy);
    second.Close();
    playlist.player().AsFast(true);
    CHECK(playlist.Run() == replay::ReplayStatus::kOk);
    CHECK(playlist.stats().events == 19);
    CHECK(playlist.stats().injected == 16);
    CHECK(playlist.stats().join_duplicates == 0);
    CHECK(playlist.stats().join_holes == 1);
    CHECK(playlist.stats().mono_regressions == 0);
    stitched.CheckStitched({1000, 3000, 5000, 7500, 9500, 11500, 15000, 17000},
                           {2000, 4000, 6000, 8500, 10500, 13500, 16000, 18000},
                           {1000, 3000, 5000, 4000, 6000, 8000, 14000, 16000},
                           {2000, 4000, 6000, 5000, 7000, 10000, 15000, 17000});
    CHECK(playlist.Seek(14500) == replay::ReplayStatus::kOk);
    CHECK(playlist.current_slice() == 2);
    CHECK(playlist.Run() == replay::ReplayStatus::kOk);
    CHECK(playlist.stats().mono_regressions == 0);
    playlist.Close();
    CheckReleased(&stitched);
  }
  {
    replay::ReplayPlayer player;
    replay::ReplayPlayerConfig cfg = stitched.Config();
    cfg.stitch_timeline = true;
    cfg.skip_overlaps = true;
    CHECK(player.Open(stitch_dirs[0], cfg) ==
          replay::ReplayStatus::kInvalidArgument);
  }
  stitched.Close();

  const std::string negative_root = root + "/negative";
  CHECK(mkdir(negative_root.c_str(), 0700) == 0);
  WriteSinglePackage(negative_root + "/a", 10000);
  WriteSinglePackage(negative_root + "/b", 20000);
  {
    std::vector<replay::ReplayPlaylistSlice> plan(2);
    plan[0].dir = negative_root + "/a";
    plan[1].dir = negative_root + "/b";
    std::string error;
    CHECK(replay::BuildReplayStitchPlan(&plan, &error) ==
          replay::ReplayStatus::kOk);
    CHECK(plan[1].sample_period_ns == 1);
    CHECK(plan[1].offset_ns == -9999);
    CHECK(plan[1].timeline_first_ns == 10001);
    CHECK(plan[1].timeline_last_ns == 10001);
  }
  const std::string overflow_root = root + "/overflow";
  CHECK(mkdir(overflow_root.c_str(), 0700) == 0);
  WriteSinglePackage(overflow_root + "/a",
                     std::numeric_limits<uint64_t>::max());
  WriteSinglePackage(overflow_root + "/b", 0);
  {
    std::vector<replay::ReplayPlaylistSlice> plan(2);
    plan[0].dir = overflow_root + "/a";
    plan[1].dir = overflow_root + "/b";
    std::string error;
    CHECK(replay::BuildReplayStitchPlan(&plan, &error) ==
          replay::ReplayStatus::kInvalidArgument);
    CHECK(error.find("overflow") != std::string::npos);
    replay::ReplayPlaylist playlist;
    replay::ReplayPlayerConfig cfg;
    cfg.stitch_timeline = true;
    CHECK(playlist.Open(std::vector<std::string>{plan[0].dir, plan[1].dir},
                        cfg) == replay::ReplayStatus::kInvalidArgument);
    CHECK(playlist.error().find("overflow") != std::string::npos);
  }

  const std::string crash_a = root + "/crash_a";
  const std::string crash_b = root + "/crash_b";
  WritePackage(crash_a, {{1000, 2000}, {1000, 2000}}, 40);
  WritePackage(crash_b, {{3000, 4000}, {3000, 4000}}, 41);
  Target crashed;
  crashed.Init("crash");
  const pid_t child = fork();
  CHECK(child >= 0);
  if (child == 0) {
    replay::ReplayPlaylist playlist;
    if (playlist.Open(std::vector<std::string>{crash_a, crash_b},
                      crashed.Config()) != replay::ReplayStatus::kOk) {
      _exit(2);
    }
    if (playlist.player().SetRate(0.001) != replay::ReplayStatus::kOk) {
      _exit(3);
    }
    (void)playlist.Run();
    _exit(0);
  }
  uint64_t gate_token = 0;
  const uint64_t wait_deadline = NowNs() + 2000000000ULL;
  while (gate_token == 0 && NowNs() < wait_deadline) {
    gate_token = crashed.writer.gate()->owner_token.load(
        std::memory_order_acquire);
    if (gate_token == 0) usleep(1000);
  }
  CHECK(gate_token != 0);
  CHECK(kill(child, SIGKILL) == 0);
  int child_status = 0;
  CHECK(waitpid(child, &child_status, 0) == child);
  const GateResult reclaimed = ProducerGateAcquire(
      *crashed.writer.gate(), ProducerGateNewToken(), 2, 1000000000ULL);
  CHECK(reclaimed == GateResult::kStolen || reclaimed == GateResult::kOk);
  PinnedSlotWriter::BeginView recovered = crashed.writer.Begin(128);
  CHECK(recovered.status == PinnedSlotWriter::BeginStatus::kOk);
  CHECK(recovered.payload != nullptr);
  std::memset(recovered.payload, 0, 128);
  crashed.writer.Commit(5000);
  crashed.Close();

  RmTree(root);
  std::printf("test_replay_playlist: OK\n");
  return 0;
}
