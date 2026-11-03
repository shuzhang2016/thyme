// test_continuous_lease_tear.cpp — P1 regression: on the continuous drain's
// lease path the pin must cover the WHOLE write sequence (fault check +
// fwrite + CRC + index row). A hammering writer races the drain on a 2-slot
// channel; every kData frame landed in .raw must be byte-identical to what
// the producer committed for that frame_index — a torn frame can only reach
// .raw if the slot was overwritten while unpinned. Acquire failures under
// the race are GAPs in the index (never torn data), and the writer's
// pinned_skip proves pins actually forced it to detour.
#include "thyme/check.h"
#include "thyme/record/thyme_bin.h"
#include "thyme/record/triggered_recorder.h"
#include "thyme/runtime/pinned_slot_store.h"
#include "thyme/runtime/shm_region.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <pthread.h>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

using namespace thyme;

namespace {

constexpr uint32_t kLen = 4096;

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

uint8_t Pat(uint64_t frame, uint32_t j) {
  return static_cast<uint8_t>(frame * 13 + j * 7);
}

void RmTree(const std::string& path) {
  DIR* d = opendir(path.c_str());
  if (d == nullptr) return;
  while (dirent* e = readdir(d)) {
    if (e->d_name[0] == '.') continue;
    const std::string sub = path + "/" + e->d_name;
    if (e->d_type == DT_DIR) RmTree(sub);
    else std::remove(sub.c_str());
  }
  closedir(d);
  rmdir(path.c_str());
}

struct WriterArg {
  PinnedSlotWriter* w;
  std::atomic<bool>* stop;
  std::atomic<uint64_t>* committed;
};

void* WriterRun(void* p) {
  auto* a = static_cast<WriterArg*>(p);
  while (!a->stop->load(std::memory_order_relaxed)) {
    // Absolute frame_index: the head read by Begin. A pinned skip also advances
    // head, so the pattern must come from head, not the commit count.
    const uint64_t f = a->w->head();
    PinnedSlotWriter::BeginView bv = a->w->Begin(kLen);
    if (bv.payload == nullptr) {  // pinned skip: slot pinned by the drain
      a->w->Commit();
      continue;
    }
    for (uint32_t j = 0; j < kLen; ++j) bv.payload[j] = Pat(f, j);
    bv.header->flags.store(0, std::memory_order_relaxed);
    a->w->Commit();
    a->committed->fetch_add(1, std::memory_order_relaxed);
  }
  return nullptr;
}

}  // namespace

int main() {
  const int me = static_cast<int>(getpid());
  char chan[96], out[96];
  std::snprintf(chan, sizeof(chan), "/c9cleartear_%d", me);
  std::snprintf(out, sizeof(out), "/tmp/c9cleartear_%d", me);
  ShmRegion::UnlinkName(chan);
  ShmRegion::UnlinkName(PinnedSlotWriter::LeaseShmName(chan).c_str());
  RmTree(out);

  PinnedSlotWriter w;
  CHECK(w.Create(chan, kLen, 2, 0));  // 2 slots: tightest pin/skip collisions
  triggered::Config ecfg;
  ecfg.sources = {{triggered::SourceConfig::Kind::kPinnedSlot, chan, "cloud_1"}};
  ecfg.out_dir = out;
  triggered::TriggeredRecorder rec;
  CHECK(rec.Init(ecfg));
  triggered::ContinuousConfig ccfg;
  ccfg.channels = {"pinned_slot:*"};
  ccfg.slice_bytes = 64ull * 1024 * 1024;  // no rolling
  CHECK(rec.ArmContinuous(ccfg));

  std::atomic<bool> stop{false};
  std::atomic<uint64_t> committed{0};
  WriterArg wa{&w, &stop, &committed};
  pthread_t th{};
  CHECK(pthread_create(&th, nullptr, WriterRun, &wa) == 0);

  const uint64_t end = NowNs() + 800000000ULL;  // 0.8 s contention window
  while (NowNs() < end) {
    rec.Tick();
    SleepNs(50000);
  }
  rec.DisarmContinuous();
  stop.store(true, std::memory_order_relaxed);
  CHECK(pthread_join(th, nullptr) == 0);

  // Check: every kData in .raw must match the committed content of its frame_index.
  // Slice dirs are named continuous_<seq>_<now>; find them by prefix.
  std::string slice;
  {
    DIR* d = opendir(out);
    CHECK(d != nullptr);
    while (dirent* de = readdir(d)) {
      if (std::strncmp(de->d_name, "continuous_", 11) == 0) {
        slice = std::string(out) + "/" + de->d_name;
      }
    }
    closedir(d);
  }
  CHECK(!slice.empty());
  CHECK(triggered::TriggeredRecorder::PackageComplete(slice));
  const std::string base = slice + "/frames/cloud_1";
  ThymeBinReader rd((base + ".index.tbin").c_str());
  CHECK(rd.ok());
  FILE* raw = std::fopen((base + ".raw").c_str(), "rb");
  CHECK(raw != nullptr);
  uint64_t datas = 0, gaps = 0;
  uint64_t next_off = 128;  // .raw starts with a 128 B header
  ThymeBinDataEntry e;
  while (rd.ReadNext(e) == thyme::ThymeBinReader::ReadResult::kOk) {
    if (e.type == ThymeBinEntryType::kGap) {
      ++gaps;
      continue;
    }
    CHECK(e.type == ThymeBinEntryType::kData);
    triggered::FrameIndexRecord ir{};
    std::memcpy(&ir, e.payload.data(), sizeof(ir));
    CHECK(ir.len == kLen);
    // A post-write mismatch must roll .raw back and leave the next
    // valid frame's segment contiguous with the last committed one.
    CHECK(ir.offset == next_off);
    next_off = ir.offset + ir.len;
    std::vector<uint8_t> seg(kLen);
    CHECK(fseeko(raw, static_cast<off_t>(ir.offset), SEEK_SET) == 0);
    CHECK(std::fread(seg.data(), 1, kLen, raw) == kLen);
    const uint64_t f = e.source_sequence;  // frame_index
    for (uint32_t j = 0; j < kLen; ++j) {
      if (seg[j] != Pat(f, j)) {
        // Diagnose: which frames these bytes actually contain (whole-segment
        // match on g), plus offset/length.
        std::string match;
        for (uint64_t g = f > 8 ? f - 8 : 0; g < f + 16; ++g) {
          uint32_t k = 0;
          while (k < kLen && seg[k] == Pat(g, k)) ++k;
          if (k == kLen) {
            match += " full=" + std::to_string(g);
          } else if (k > 0) {
            match += " partial=" + std::to_string(g) + "(" +
                     std::to_string(k) + "B)";
          }
        }
        std::fprintf(stderr,
                     "TORN frame %llu at byte %u: got=%02x want=%02x "
                     "off=%llu len=%u mono=%llu |%s\n",
                     static_cast<unsigned long long>(f), j, seg[j], Pat(f, j),
                     static_cast<unsigned long long>(ir.offset), ir.len,
                     static_cast<unsigned long long>(e.mono_ns),
                     match.c_str());
        std::fprintf(stderr,
                     "diag: head=%llu skips=%llu steal=%llu crc_idx=%08x "
                     "crc_seg=%08x\n",
                     static_cast<unsigned long long>(w.head()),
                     static_cast<unsigned long long>(w.pinned_skip_count()),
                     static_cast<unsigned long long>(
                         w.lease()->lease_steal.load(std::memory_order_acquire)),
                     ir.payload_crc, Crc32(seg.data(), kLen));
        CHECK(false);  // torn frame in .raw: the lease did not cover the write-to-disk sequence
      }
    }
    CHECK(Crc32(seg.data(), kLen) == ir.payload_crc);
    ++datas;
  }
  CHECK(std::fclose(raw) == 0);
  const uint64_t skips = w.pinned_skip_count();
  std::fprintf(stderr,
               "continuous lease tear: data=%llu gaps=%llu pinned_skip=%llu "
               "committed=%llu\n",
               static_cast<unsigned long long>(datas),
               static_cast<unsigned long long>(gaps),
               static_cast<unsigned long long>(skips),
               static_cast<unsigned long long>(
                   committed.load(std::memory_order_acquire)));
  CHECK(datas >= 20);           // frames were actually written and all content is valid
  CHECK(gaps > 0 || skips > 0); // contention did occur (acquire failure and/or writer yielding)

  rec.Shutdown();
  w.Close();
  ShmRegion::UnlinkName(chan);
  ShmRegion::UnlinkName(PinnedSlotWriter::LeaseShmName(chan).c_str());
  RmTree(out);
  return 0;
}
