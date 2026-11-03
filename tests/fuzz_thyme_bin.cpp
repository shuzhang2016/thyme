// fuzz_thyme_bin.cpp — TST-03 structured fuzz driver for the .thyme.bin
// reader (ThymeBinReader: header, struct registry, record stream, footer
// index). Deterministic: splitmix64(seed, iteration) picks the seed and the
// mutation script, so a failure reproduces with the same argv.
//
// Usage: fuzz_thyme_bin [iterations] [seed]
//   env THYME_FUZZ_ITERS / THYME_FUZZ_SEED override argv.
//
// libFuzzer upgrade path: build with -DTHYME_FUZZ_LIBFUZZER and without a
// main(); RunOneInput is the LLVMFuzzerTestOneInput body.

#include "thyme/check.h"
#include "thyme/record/thyme_bin.h"

#include "fuzz_harness.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <unistd.h>

namespace {

using thyme::ThymeBinDataEntry;
using thyme::ThymeBinReader;
using thyme_fuzz::SplitMix64;

constexpr std::size_t kMaxInput = 1u << 20;  // 1 MiB: bounded time/memory
constexpr uint64_t kDefaultIters = 20000;
constexpr uint64_t kDefaultSeed = 0x54594d45u;  // "TYME"

// In-memory sink so the canonical seeds are produced by the real encoder
// (deterministic bytes, guaranteed-valid CRCs/index).
class MemSink : public thyme::ThymeBinSink {
 public:
  explicit MemSink(bool seekable) : seekable_(seekable) {}
  bool ok() const override { return true; }
  bool Write(const void* data, std::size_t n) override {
    const auto* p = static_cast<const uint8_t*>(data);
    bytes.insert(bytes.end(), p, p + n);
    return true;
  }
  bool Seekable() const override { return seekable_; }
  int64_t Tell() const override {
    return seekable_ ? static_cast<int64_t>(bytes.size()) : -1;
  }
  std::vector<uint8_t> bytes;

 private:
  bool seekable_;
};

struct Seed {
  std::vector<uint8_t> bytes;
  std::vector<std::size_t> record_offsets;   // first byte of each record
  std::vector<std::size_t> crc_offsets;      // crc32 field of each record
  std::vector<std::size_t> payload_size_offsets;  // kData records only
};

// Writes the canonical 5-record stream (data, trigger, gap, session, data)
// plus a second data run long enough to carry a footer index when seekable.
Seed MakeEncoderSeed(bool seekable) {
  Seed s;
  MemSink sink(seekable);
  thyme::ThymeBinEncoder enc(&sink);
  thyme::ThymeStructDesc descs[2]{};
  descs[0].struct_id = 0;
  descs[0].contract_version = 1;
  descs[0].payload_size = 16;
  std::snprintf(descs[0].name, sizeof(descs[0].name), "Demo");
  descs[1].struct_id = 1;
  descs[1].contract_version = 1;
  descs[1].payload_size = 4;
  std::snprintf(descs[1].name, sizeof(descs[1].name), "Aux");
  CHECK(enc.WriteHeader(0x1234, 1000000, descs, 2));

  uint8_t payload[16];
  for (int i = 0; i < 16; ++i) payload[i] = static_cast<uint8_t>(i + 1);

  auto mark = [&]() {
    if (seekable) s.record_offsets.push_back(sink.bytes.size());
  };
  auto mark_data = [&]() {
    if (seekable) {
      // type(1)+pad(3)+seq(8)+struct_id(2)+reserved(2)+mono(8)+src_seq(4)
      s.payload_size_offsets.push_back(sink.bytes.size() + 28);
    }
  };
  auto mark_end = [&]() {
    if (seekable) s.crc_offsets.push_back(sink.bytes.size() - 4);
  };

  mark();
  mark_data();
  CHECK(enc.WriteData(1, 0, 1000100, 9, payload, 16));
  mark_end();
  mark();
  CHECK(enc.WriteTrigger(7, 1000200, 1000, 2000));
  mark_end();
  mark();
  CHECK(enc.WriteGap(1, 1000300, 1, 3));
  mark_end();
  mark();
  CHECK(enc.WriteData(2, 1, 1000400, 1, payload, 4));
  mark_end();
  // Enough records for a non-trivial footer index (two block entries).
  for (uint64_t i = 0; i < 130; ++i) {
    mark();
    mark_data();
    CHECK(enc.WriteData(10 + i, 0, 1000500 + i * 100,
                        static_cast<uint32_t>(100 + i), payload, 16));
    mark_end();
  }
  CHECK(enc.Finish());
  s.bytes = std::move(sink.bytes);
  return s;
}

std::string TmpPath() {
  const char* dir = std::getenv("THYME_FUZZ_TMPDIR");
  if (dir == nullptr || dir[0] == '\0' ||
      std::strncmp(dir, "/dev/shm", 8) == 0) {
    dir = "/tmp";
  }
  char buf[160];
  std::snprintf(buf, sizeof(buf), "%s/thyme_fuzz_bin_%ld.bin", dir,
                static_cast<long>(getpid()));
  return buf;
}

// The fuzz target proper. Never crashes, never hangs: the input is bounded
// and the record loop is capped.
void RunOneInput(const uint8_t* data, std::size_t size) {
  if (size > kMaxInput) return;
  static const std::string path = TmpPath();
  {
    FILE* f = std::fopen(path.c_str(), "wb");
    if (f == nullptr) return;
    if (size > 0) {
      const std::size_t w = std::fwrite(data, 1, size, f);
      CHECK(w == size);
    }
    CHECK(std::fclose(f) == 0);
  }
  ThymeBinReader r(path.c_str());
  if (!r.ok()) return;  // rejected header / unreadable: a defined result
  if (r.HasIndex()) {
    // Seek paths must accept arbitrary targets without corrupting state.
    CHECK(r.SeekToMonoNs(0));
    CHECK(r.SeekToMonoNs(1000000));
    CHECK(r.SeekToMonoNs(~0ull));
    CHECK(r.SeekToMonoNs(r.first_mono_ns()));
    CHECK(r.SeekToMonoNs(r.last_mono_ns()));
    r.SeekToMonoNs(0);
  }
  ThymeBinDataEntry e{};
  for (int i = 0; i < 100000; ++i) {
    const ThymeBinReader::ReadResult res = r.ReadNext(e);
    CHECK(res == ThymeBinReader::ReadResult::kOk ||
          res == ThymeBinReader::ReadResult::kEof ||
          res == ThymeBinReader::ReadResult::kCorrupt);
    if (res == ThymeBinReader::ReadResult::kOk) {
      CHECK(e.payload.size() <= thyme::kThymeBinMaxPayloadSize);
      if (e.type == thyme::ThymeBinEntryType::kData) {
        CHECK(e.payload.size() == e.payload_size);
      }
      continue;
    }
    break;  // kEof / kCorrupt terminate the stream
  }
}

void Poke(std::vector<uint8_t>* b, std::size_t at, uint64_t value, int width) {
  if (b->size() < static_cast<std::size_t>(width) ||
      at > b->size() - static_cast<std::size_t>(width)) {
    return;
  }
  for (int i = 0; i < width; ++i) {
    (*b)[at + i] = static_cast<uint8_t>((value >> (8 * i)) & 0xFFu);
  }
}

std::vector<uint8_t> Mutate(std::vector<uint8_t> in, const Seed& structured,
                            bool have_layout, SplitMix64* rng) {
  const int ops = 1 + static_cast<int>(rng->Below(3));
  for (int op = 0; op < ops && in.size() <= kMaxInput; ++op) {
    switch (rng->Below(14)) {
      case 0:  // flip header magic
        thyme_fuzz::FlipRandomBytes(&in, rng, 1);
        if (in.size() >= 4 && rng->Coin(2)) {
          Poke(&in, rng->Below(4), rng->Byte(), 1);
        }
        break;
      case 1:  // file_abi_version: valid edges + garbage
        Poke(&in, offsetof(thyme::ThymeBinFileHeader, file_abi_version),
             rng->Pick(std::vector<uint64_t>{0, 1, 2, 3, 0xFFFF}), 2);
        break;
      case 2:  // struct_count: around the kThymeBinMaxStructCount bound
        Poke(&in, offsetof(thyme::ThymeBinFileHeader, struct_count),
             rng->Pick(std::vector<uint64_t>{0, 1, 63, 64, 65, 0xFFFFFFFFu}),
             4);
        break;
      case 3:  // truncate at a real record boundary when known
        if (have_layout && !structured.record_offsets.empty()) {
          in.resize(rng->Pick(structured.record_offsets));
        } else {
          thyme_fuzz::Truncate(&in, rng);
        }
        break;
      case 4:  // corrupt a record CRC (reader must report kCorrupt)
        if (have_layout && !structured.crc_offsets.empty()) {
          const std::size_t at = rng->Pick(structured.crc_offsets);
          if (at < in.size()) {
            in[at] = static_cast<uint8_t>(in[at] ^
                                          static_cast<uint8_t>(rng->Byte() | 1u));
          }
        } else {
          thyme_fuzz::FlipRandomBit(&in, rng);
        }
        break;
      case 5:  // payload_size lies: 0 / exact max / max+1 / huge
        if (have_layout && !structured.payload_size_offsets.empty()) {
          Poke(&in, rng->Pick(structured.payload_size_offsets),
               rng->Pick(std::vector<uint64_t>{
                   0, thyme::kThymeBinMaxPayloadSize,
                   thyme::kThymeBinMaxPayloadSize + 1, 0xFFFFFFFFu}),
               4);
        } else {
          thyme_fuzz::PokeLe(&in, rng, 4,
                             rng->Pick(std::vector<uint64_t>{
                                 0, 0xFFFFFFFFu, thyme::kThymeBinMaxPayloadSize,
                                 thyme::kThymeBinMaxPayloadSize + 1}));
        }
        break;
      case 6:
        thyme_fuzz::FillRandomSpan(&in, rng, rng->Coin(2));
        break;
      case 7:
        thyme_fuzz::DeleteSpan(&in, rng);
        break;
      case 8:
        thyme_fuzz::DuplicateSpan(&in, rng, kMaxInput);
        break;
      case 9:
        thyme_fuzz::SwapSpans(&in, rng);
        break;
      case 10:  // footer/index region: random poke in the last 96 bytes
        if (in.size() > 4) {
          const std::size_t tail =
              in.size() - std::min<std::size_t>(in.size(), 96);
          Poke(&in, tail + rng->Below(in.size() - tail), rng->Next(),
               static_cast<int>(1 + rng->Below(4)));
        }
        break;
      case 11:
        thyme_fuzz::ExtendRandom(&in, rng, kMaxInput);
        break;
      case 12:  // mono_ns / sequence fields: extremes
        thyme_fuzz::PokeLe(&in, rng, 8,
                           rng->Pick(std::vector<uint64_t>{0, 1, ~0ull}));
        break;
      default:
        thyme_fuzz::FlipRandomBit(&in, rng);
        break;
    }
  }
  return in;
}

}  // namespace

#if defined(THYME_FUZZ_LIBFUZZER)
extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  RunOneInput(data, size);
  return 0;
}
#else
int main(int argc, char** argv) {
  const thyme_fuzz::RunConfig cfg =
      thyme_fuzz::ParseRunConfig(argc, argv, kDefaultIters, kDefaultSeed);

  std::vector<std::vector<uint8_t>> corpus =
      thyme_fuzz::LoadCorpusDir(THYME_FUZZ_CORPUS_DIR);
  const Seed indexed = MakeEncoderSeed(true);
  const Seed plain = MakeEncoderSeed(false);
  corpus.push_back(indexed.bytes);
  corpus.push_back(plain.bytes);
  CHECK(!corpus.empty());

  // (a) corpus replay: every seed must be handled without a crash.
  for (const auto& s : corpus) RunOneInput(s.data(), s.size());

  // Canonical seeds must actually parse (guards the harness against a codec
  // change silently turning the whole corpus into rejects).
  {
    static const std::string path = TmpPath();
    FILE* f = std::fopen(path.c_str(), "wb");
    CHECK(f != nullptr);
    CHECK(std::fwrite(indexed.bytes.data(), 1, indexed.bytes.size(), f) ==
          indexed.bytes.size());
    CHECK(std::fclose(f) == 0);
    ThymeBinReader r(path.c_str());
    CHECK(r.ok());
    CHECK(r.HasIndex());
    ThymeBinDataEntry e{};
    uint64_t records = 0;
    while (r.ReadNext(e) == ThymeBinReader::ReadResult::kOk) ++records;
    CHECK(records == r.indexed_record_count());
    CHECK(records == 134);
  }

  // (b) deterministic structured mutations.
  uint64_t accepted = 0;
  for (uint64_t i = 0; i < cfg.iterations; ++i) {
    SplitMix64 rng(thyme_fuzz::IterSeed(cfg.seed, i));
    const bool use_indexed = rng.Coin(2);
    const Seed& base = use_indexed ? indexed : plain;
    const bool corpus_base = !corpus.empty() && rng.Coin(4);
    std::vector<uint8_t> in =
        corpus_base ? corpus[rng.Below(corpus.size())] : base.bytes;
    in = Mutate(std::move(in), base, !corpus_base, &rng);
    RunOneInput(in.data(), in.size());
    ++accepted;
  }
  const std::string tmp = TmpPath();
  std::remove(tmp.c_str());
  std::printf("fuzz_thyme_bin: %llu inputs, seed=%llu, corpus=%zu, OK\n",
              static_cast<unsigned long long>(accepted),
              static_cast<unsigned long long>(cfg.seed), corpus.size());
  return 0;
}
#endif
