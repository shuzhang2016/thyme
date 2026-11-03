#pragma once

// replay_synthetic_package.h — shared fixture for the REP-06/REP-09 tests.
//
// Writes a small but real capture package without recording anything:
//
//   <dir>/meta.tbin                     struct 63 (8-byte markers) + trigger
//   <dir>/small.tbin                    struct 0 (Pod16), 1 ms grid
//   <dir>/frames/cam_a.index.tbin       FrameIdx rows (+ one FrameGap row)
//   <dir>/frames/cam_a.raw              deterministic frame bytes
//   <dir>/schema.json                   ABI-09 sidecar (optional)
//
// Every .tbin is written through ThymeBinEncoder, so the indexed variant ends
// with a REC-04 footer index; MakeUnindexed() truncates the footers to produce
// the legacy/unindexed variant the reader must seek with its scan-built block
// table. Timestamps are deterministic (t0 + fixed periods), which is what the
// two tests compare byte-for-byte.

#include "thyme/check.h"
#include "thyme/contracts/frame_header.h"
#include "thyme/record/record_schema.h"
#include "thyme/record/thyme_bin.h"
#include "thyme/record/triggered_recorder.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace replay_synth {

using namespace thyme;  // NOLINT: test fixture, keeps the literals readable

inline constexpr uint16_t kSmallStructId = 0;
inline constexpr uint64_t kT0 = 1000000000ULL;
inline constexpr uint64_t kMetaPeriodNs = 5000;
inline constexpr uint64_t kSmallPeriodNs = 1000;
inline constexpr uint64_t kFramePeriodNs = 2000;

struct Pod16 {
  uint64_t a;
  uint32_t b;
  uint32_t c;
};

struct Spec {
  uint32_t small_rows = 300;
  uint32_t frame_rows = 200;
  bool schema = true;
};

inline uint32_t FrameLen(uint32_t i) {
  return 512 + (i % 5) * 128;
}

inline void FillFrame(uint8_t* p, uint32_t len, uint32_t i) {
  for (uint32_t j = 0; j < len; ++j) {
    p[j] = static_cast<uint8_t>((i * 37u + j * 11u) & 0xFFu);
  }
}

inline uint64_t SmallMono(uint32_t i) {
  return kT0 + 1000 + static_cast<uint64_t>(i) * kSmallPeriodNs;
}

inline uint64_t FrameMono(uint32_t i) {
  return kT0 + 1500 + static_cast<uint64_t>(i) * kFramePeriodNs;
}

inline uint64_t MetaMono(uint32_t i) {
  return kT0 + static_cast<uint64_t>(i) * kMetaPeriodNs;
}

inline void MakeDirs(const std::string& dir) {
  CHECK(mkdir(dir.c_str(), 0700) == 0 || errno == EEXIST);
  const std::string frames = dir + "/frames";
  CHECK(mkdir(frames.c_str(), 0700) == 0 || errno == EEXIST);
}

inline ThymeStructDesc Desc(uint16_t id, const char* name, uint32_t size,
                            uint16_t version = 1) {
  ThymeStructDesc d{};
  d.struct_id = id;
  d.contract_version = version;
  d.payload_size = size;
  std::snprintf(d.name, sizeof(d.name), "%s", name);
  return d;
}

inline void WriteTrigger(ThymeBinEncoder* enc, uint64_t id, uint64_t mono) {
  CHECK(enc->WriteTrigger(id, mono, 100000000ULL, 200000000ULL));
}

inline void WriteGap(ThymeBinEncoder* enc, uint16_t sid, uint64_t mono,
                     uint16_t reason, uint32_t missed) {
  CHECK(enc->WriteGap(sid, mono, reason, missed));
}

inline bool WritePackage(const std::string& dir, const Spec& spec,
                         std::string* err) {
  MakeDirs(dir);
  const ThymeStructDesc meta_desc = Desc(63, "TrigMeta", 8);
  const ThymeStructDesc small_desc =
      Desc(kSmallStructId, "Pod16", sizeof(Pod16));
  const ThymeStructDesc frame_desc =
      Desc(triggered::kFrameIndexStructId, "FrameIdx",
           sizeof(triggered::FrameIndexRecord));
  const ThymeStructDesc gap_desc =
      Desc(triggered::kFrameGapStructId, "FrameGap", 0);

  // ---- meta.tbin: marker rows at a 5 us grid + one trigger ----
  {
    ThymeBinFileSink sink((dir + "/meta.tbin").c_str());
    ThymeBinEncoder enc(&sink);
    CHECK(enc.WriteHeader(0x5151, kT0, &meta_desc, 1));
    uint64_t seq = 0;
    for (uint32_t i = 0; i < 4; ++i) {
      if (i == 2) {
        WriteTrigger(&enc, 7, MetaMono(2));
      }
      const uint64_t marker = i;
      CHECK(enc.WriteData(seq++, meta_desc.struct_id, MetaMono(i), i, &marker,
                          sizeof(marker)));
    }
    CHECK(enc.Finish());
    CHECK(sink.Sync());
  }

  // ---- small.tbin ----
  {
    ThymeBinFileSink sink((dir + "/small.tbin").c_str());
    ThymeBinEncoder enc(&sink);
    CHECK(enc.WriteHeader(0x5151, kT0, &small_desc, 1));
    for (uint32_t i = 0; i < spec.small_rows; ++i) {
      Pod16 p{};
      p.a = i;
      p.b = i * 2u;
      p.c = i * 3u;
      CHECK(enc.WriteData(i, small_desc.struct_id, SmallMono(i), i, &p,
                          sizeof(p)));
    }
    CHECK(enc.Finish());
    CHECK(sink.Sync());
  }

  // ---- frames/cam_a.{index.tbin,raw} ----
  {
    const std::string raw_path = dir + "/frames/cam_a.raw";
    std::FILE* raw = std::fopen(raw_path.c_str(), "wb");
    CHECK(raw != nullptr);
    std::vector<uint8_t> frame;
    uint64_t raw_offset = 0;
    ThymeBinFileSink sink((dir + "/frames/cam_a.index.tbin").c_str());
    ThymeBinEncoder enc(&sink);
    const ThymeStructDesc regs[2] = {frame_desc, gap_desc};
    CHECK(enc.WriteHeader(0x5151, kT0, regs, 2));
    for (uint32_t i = 0; i < spec.frame_rows; ++i) {
      const uint32_t len = FrameLen(i);
      frame.resize(len);
      FillFrame(frame.data(), len, i);
      CHECK(std::fwrite(frame.data(), 1, len, raw) == len);
      triggered::FrameIndexRecord idx{};
      idx.offset = raw_offset;
      idx.len = len;
      idx.flags = i % 2;
      idx.payload_crc = Crc32(frame.data(), len);
      CHECK(enc.WriteData(i, frame_desc.struct_id, FrameMono(i), i, &idx,
                          sizeof(idx)));
      raw_offset += len;
      if (i == 10) {
        WriteGap(&enc, gap_desc.struct_id, FrameMono(i) + 500,
                 triggered::kGapReasonOverrun, 3);
      }
    }
    CHECK(enc.Finish());
    CHECK(sink.Sync());
    CHECK(std::fclose(raw) == 0);
  }

  if (spec.schema) {
    const ThymeStructDesc descs[4] = {meta_desc, small_desc, frame_desc,
                                      gap_desc};
    std::string serr;
    if (!schema::WriteSchemaFile(dir + "/schema.json", descs, 4, nullptr,
                                 false, &serr)) {
      if (err != nullptr) *err = serr;
      return false;
    }
  }
  return true;
}

// Truncates every .tbin at its REC-04 footer index, leaving a valid
// unindexed record stream (the legacy/stream-capture shape).
inline bool MakeUnindexed(const std::string& dir, std::string* err) {
  const char* files[3] = {"/meta.tbin", "/small.tbin",
                          "/frames/cam_a.index.tbin"};
  for (const char* rel : files) {
    const std::string path = dir + rel;
    std::FILE* f = std::fopen(path.c_str(), "r+b");
    if (f == nullptr) {
      if (err != nullptr) *err = "cannot open " + path;
      return false;
    }
    if (std::fseek(f, -static_cast<long>(sizeof(ThymeBinIndexFooter)),
                   SEEK_END) != 0) {
      std::fclose(f);
      if (err != nullptr) *err = "cannot seek " + path;
      return false;
    }
    ThymeBinIndexFooter footer{};
    if (std::fread(&footer, sizeof(footer), 1, f) != 1 ||
        std::memcmp(footer.magic, kThymeBinIndexMagic, sizeof(footer.magic)) !=
            0) {
      std::fclose(f);
      if (err != nullptr) *err = "no footer in " + path;
      return false;
    }
    std::fclose(f);
    if (truncate(path.c_str(), static_cast<off_t>(footer.entries_offset)) !=
        0) {
      if (err != nullptr) *err = "truncate failed for " + path;
      return false;
    }
  }
  return true;
}

}  // namespace replay_synth
