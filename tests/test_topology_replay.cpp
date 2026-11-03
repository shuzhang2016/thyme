// test_topology_replay.cpp — TST-12 / REP-11: replay across a topology change.
//
// Package A is written through ThymeBinEncoder with ABI-11 keyed descriptors
// (file version 4) and a record-manifest block + sidecar:
//
//   small.tbin registry: alpha(id 0, 52 B framed), beta(id 1, 32 B bare),
//                        removed(id 2, 8 B bare)
//
// Target topology B is a RecordMirrorRingPage whose structs are reordered and
// renumbered (beta=0, alpha=1) and that adds gamma=2 but omits `removed`.
//
// Asserted:
//   * `--check-compat` maps alpha 0->1 and beta 1->0 by key, and fails on the
//     missing `removed` unless `--allow-missing`;
//   * `--allow-missing` skips the removed channel and injects the rest;
//   * a target whose key matches but layout_hash differs is an explicit
//     error (never a silent remap), in check and in real playback;
//   * replay still applies the REP-12 embedded FrameHeader rewrite on the
//     mapped framed channel (mono_ns/session/sequence + valid CRC);
//   * two replays of the same package into fresh targets produce the same
//     mono_ns/sequence/payload bytes for every matched channel.
//
// The ring pages are created in /dev/shm with a pid-unique name and unlinked
// on exit; the test is deterministic and runs in well under a second.

#include "thyme/check.h"
#include "thyme/contracts/frame_header.h"
#include "thyme/record/record_mirror_ring_client.h"
#include "thyme/record/record_schema.h"
#include "thyme/record/thyme_bin.h"
#include "replay_cli.h"
#include "thyme/runtime/record_mirror_ring.h"
#include "thyme/runtime/shm_region.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <sstream>
#include <string>
#include <unistd.h>
#include <vector>

using namespace thyme;

namespace {

constexpr uint64_t kManifestHash = 0x51A7B00C1234ABCDULL;
constexpr uint64_t kRecordedSession = 0x5151;
constexpr uint32_t kFrameMagic = 0x54484D31;  // "THM1"
constexpr uint16_t kFrameVersion = 1;
constexpr uint64_t kT0 = 1000;

struct PodA {
  uint64_t a;
  uint32_t b;
  uint32_t c;
};
struct PodB {
  uint64_t a;
  uint64_t b;
  uint8_t fill[16];
};
static_assert(sizeof(PodA) == 16, "fixture");
static_assert(sizeof(PodB) == 32, "fixture");

const schema::FieldLayout kAlphaFields[] = {
    {"a", "uint64", 0, 8, 1, 8},
    {"b", "uint32", 8, 4, 1, 4},
    {"c", "uint32", 12, 4, 1, 4},
};
const schema::ContractLayout kAlphaLayout = {"PodA", sizeof(PodA), 3,
                                             alignof(PodA), kAlphaFields};

const schema::FieldLayout kBetaFields[] = {
    {"a", "uint64", 0, 8, 1, 8},
    {"b", "uint64", 8, 8, 1, 8},
    {"fill", "uint8", 16, 16, 16, 1},
};
const schema::ContractLayout kBetaLayout = {"PodB", sizeof(PodB), 3,
                                            alignof(PodB), kBetaFields};

const schema::FieldLayout kRemovedFields[] = {
    {"value", "uint64", 0, 8, 1, 8},
};
const schema::ContractLayout kRemovedLayout = {"Removed", 8, 1, 8,
                                               kRemovedFields};

constexpr uint32_t kAlphaPayload = sizeof(FrameHeader) + sizeof(PodA);  // 52
constexpr uint32_t kBetaPayload = sizeof(PodB);                         // 32
constexpr uint32_t kRemovedPayload = 8;

ThymeStructDesc Desc(uint16_t id, const char* cpp_type, const char* channel,
                     const char* key, uint32_t payload, uint64_t layout_hash) {
  ThymeStructDesc d{};
  d.struct_id = id;
  d.contract_version = 1;
  d.payload_size = payload;
  d.layout_hash = layout_hash;
  std::snprintf(d.name, sizeof(d.name), "%s", cpp_type);
  SetThymeStructChannel(&d, channel);
  SetThymeStructRecordKey(&d, key);
  return d;
}

std::string ShmName(const char* tag) {
  static int counter = 0;
  char buf[96];
  std::snprintf(buf, sizeof(buf), "/thyme_tst12_%d_%s_%d",
                static_cast<int>(getpid()), tag, counter++);
  return std::string(buf);
}

const ThymeBinManifestBlock kManifest =
    MakeThymeBinManifestBlock(kManifestHash, schema::kManifestVersion);

std::vector<uint8_t> AlphaPayload(uint32_t seq, uint64_t mono) {
  PodA pod{};
  pod.a = 0x1000 + seq;
  pod.b = seq * 2;
  pod.c = seq * 3;
  FrameHeader h{};
  h.sequence = seq;
  h.mono_ns = mono;
  h.session_id = kRecordedSession;
  h.size = static_cast<uint16_t>(kAlphaPayload);
  h.magic = kFrameMagic;
  h.version = kFrameVersion;
  uint8_t body[sizeof(PodA)];
  std::memcpy(body, &pod, sizeof(body));
  FrameHeaderSeal(h, body, sizeof(body), kFrameMagic, kFrameVersion);
  std::vector<uint8_t> out(kAlphaPayload);
  std::memcpy(out.data(), &h, sizeof(h));
  std::memcpy(out.data() + sizeof(h), body, sizeof(body));
  return out;
}

std::vector<uint8_t> BetaPayload(uint32_t seq) {
  PodB pod{};
  pod.a = 0x2000 + seq;
  pod.b = 0x3000 + seq;
  for (uint32_t i = 0; i < sizeof(pod.fill); ++i) {
    pod.fill[i] = static_cast<uint8_t>(seq * 7 + i);
  }
  std::vector<uint8_t> out(sizeof(pod));
  std::memcpy(out.data(), &pod, sizeof(pod));
  return out;
}

const ThymeStructDesc kAlpha = Desc(0, "PodA", "alpha", "alpha", kAlphaPayload,
                                    schema::ComputeLayoutHash(kAlphaLayout));
const ThymeStructDesc kBeta = Desc(1, "PodB", "beta", "beta", kBetaPayload,
                                   schema::ComputeLayoutHash(kBetaLayout));
const ThymeStructDesc kRemoved =
    Desc(2, "Removed", "removed", "removed", kRemovedPayload,
         schema::ComputeLayoutHash(kRemovedLayout));
const ThymeStructDesc kMeta = Desc(63, "TrigMeta", "", "", 8, 0);

void WritePackage(const std::string& dir) {
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);
  {
    ThymeBinFileSink sink((dir + "/meta.tbin").c_str());
    ThymeBinEncoder enc(&sink);
    CHECK(enc.WriteHeader(kRecordedSession, kT0, &kMeta, 1, &kManifest));
    CHECK(enc.Finish());
    CHECK(sink.Sync());
  }
  {
    const ThymeStructDesc regs[3] = {kAlpha, kBeta, kRemoved};
    ThymeBinFileSink sink((dir + "/small.tbin").c_str());
    ThymeBinEncoder enc(&sink);
    CHECK(enc.WriteHeader(kRecordedSession, kT0, regs, 3, &kManifest));
    // Sorted by mono_ns, as the recorder writes a source.
    const std::vector<uint8_t> a0 = AlphaPayload(0, kT0);
    const std::vector<uint8_t> a1 = AlphaPayload(1, kT0 + 1000);
    const std::vector<uint8_t> b0 = BetaPayload(0);
    const std::vector<uint8_t> b1 = BetaPayload(1);
    const std::vector<uint8_t> b2 = BetaPayload(2);
    const uint64_t removed = 0xDEADBEEFULL;
    CHECK(enc.WriteData(0, 0, kT0, 0, a0.data(),
                        static_cast<uint32_t>(a0.size())));
    CHECK(enc.WriteData(1, 2, kT0 + 200, 0, &removed, sizeof(removed)));
    CHECK(enc.WriteData(2, 1, kT0 + 500, 0, b0.data(),
                        static_cast<uint32_t>(b0.size())));
    CHECK(enc.WriteData(3, 0, kT0 + 1000, 1, a1.data(),
                        static_cast<uint32_t>(a1.size())));
    CHECK(enc.WriteData(4, 1, kT0 + 1500, 1, b1.data(),
                        static_cast<uint32_t>(b1.size())));
    CHECK(enc.WriteData(5, 1, kT0 + 2500, 2, b2.data(),
                        static_cast<uint32_t>(b2.size())));
    CHECK(enc.Finish());
    CHECK(sink.Sync());
  }
  // Sidecar: records sorted by struct_id; channels[] is the ABI-11 key table.
  const ThymeStructDesc sidecar_descs[4] = {kAlpha, kBeta, kRemoved, kMeta};
  schema::RecordManifest manifest;
  manifest.hash = kManifestHash;
  manifest.version = schema::kManifestVersion;
  std::string err;
  CHECK(schema::WritePackageSchema(dir, sidecar_descs, 4, &manifest,
                                   /*partial=*/false, &err));
  CHECK(err.empty());
}

struct Ring {
  std::unique_ptr<record_mirror::RingClient> client;
  std::string name;
};

Ring MakeRing(const char* tag, const ThymeStructDesc* descs,
              const RecordMirrorRingSpec* specs, uint32_t count) {
  Ring ring;
  ring.name = ShmName(tag);
  ring.client = std::make_unique<record_mirror::RingClient>();
  CHECK(ring.client->Create(ring.name.c_str(), descs, specs, count, 1));
  return ring;
}

struct CliResult {
  int rc = 0;
  std::string out;
  std::string err;
};

CliResult RunCli(const std::vector<std::string>& args) {
  CliResult r;
  std::ostringstream out;
  std::ostringstream err;
  r.rc = replaycli::RunCli(args, out, err, nullptr);
  r.out = out.str();
  r.err = err.str();
  return r;
}

bool Contains(const std::string& text, const std::string& needle) {
  return text.find(needle) != std::string::npos;
}

void CheckAlphaSample(const RecordMirrorRingPage* page, uint16_t sid,
                      uint64_t index, uint64_t expected_mono,
                      uint32_t expected_sequence) {
  uint8_t buf[256];
  uint32_t size = 0;
  uint64_t mono = 0;
  uint32_t seq = 0;
  CHECK(page->ReadRingSample(sid, index, buf, sizeof(buf), size, mono, seq));
  CHECK(size == kAlphaPayload);
  CHECK(mono == expected_mono);
  CHECK(seq == expected_sequence);
  FrameHeader h{};
  std::memcpy(&h, buf, sizeof(h));
  CHECK(h.magic == kFrameMagic);
  CHECK(h.version == kFrameVersion);
  CHECK(h.valid == 1);
  CHECK(h.size == kAlphaPayload);
  CHECK(h.mono_ns == expected_mono);
  CHECK(h.sequence == expected_sequence);
  CHECK(h.session_id != kRecordedSession);  // REP-12 rewrite
  CHECK(FrameHeaderOk(h, buf + sizeof(FrameHeader),
                      size - sizeof(FrameHeader), kFrameMagic, kFrameVersion));
}

void TestTopologyChangeReplay() {
  const std::string root =
      (std::filesystem::temp_directory_path() / "thyme_tst12").string();
  const std::string pkg = root + "/pkg_a";
  WritePackage(pkg);

  const uint64_t h_alpha = schema::ComputeLayoutHash(kAlphaLayout);
  const uint64_t h_beta = schema::ComputeLayoutHash(kBetaLayout);
  // Topology B: beta=0, alpha=1 (reordered/renumbered), gamma=2 (added);
  // `removed` is absent.
  const ThymeStructDesc b_descs[3] = {
      Desc(0, "PodB", "beta", "beta", kBetaPayload, h_beta),
      Desc(1, "PodA", "alpha", "alpha", kAlphaPayload, h_alpha),
      Desc(2, "Gamma", "gamma", "gamma", 16, 0x1234),
  };
  const RecordMirrorRingSpec b_specs[3] = {
      {kBetaPayload, 32}, {kAlphaPayload, 32}, {16, 32}};
  Ring b = MakeRing("b", b_descs, b_specs, 3);

  // 1. Missing channel is an explicit failure without --allow-missing.
  {
    CliResult r = RunCli({"play", pkg, "--small", "small=" + b.name,
                          "--check-compat"});
    CHECK(r.rc == replaycli::kExitTargets);
    CHECK(Contains(r.out, "removed"));
    CHECK(Contains(r.out, "allow-missing"));
  }
  // 2. --allow-missing turns it into a reported skip and succeeds.
  {
    CliResult r = RunCli({"play", pkg, "--small", "small=" + b.name,
                          "--check-compat", "--allow-missing"});
    CHECK(r.rc == replaycli::kExitOk);
    CHECK(Contains(r.out, "topology:"));
    CHECK(Contains(r.out, "mapped"));
    CHECK(Contains(r.out, "missing"));
    // alpha 0 -> 1, beta 1 -> 0, removed -> no target
    CHECK(Contains(r.out, "alpha"));
    CHECK(Contains(r.out, "beta"));
  }
  // 3. Real playback: inject only the matched channels.
  {
    CliResult r = RunCli({"play", pkg, "--small", "small=" + b.name,
                          "--as-fast", "--allow-missing", "--no-gate"});
    CHECK(r.rc == replaycli::kExitOk);
  }
  const RecordMirrorRingPage* page = b.client->Page();
  CHECK(page != nullptr && page->Validate());
  CHECK(page->RingHead(0) == 3);  // beta -> target 0
  CHECK(page->RingHead(1) == 2);  // alpha -> target 1
  CHECK(page->RingHead(2) == 0);  // gamma: no package channel
  for (uint64_t i = 0; i < 2; ++i) {
    CheckAlphaSample(page, 1, i, kT0 + i * 1000, static_cast<uint32_t>(i));
  }
  for (uint64_t i = 0; i < 3; ++i) {
    uint8_t buf[64];
    uint32_t size = 0;
    uint64_t mono = 0;
    uint32_t seq = 0;
    CHECK(page->ReadRingSample(0, i, buf, sizeof(buf), size, mono, seq));
    CHECK(size == kBetaPayload);
    const std::vector<uint8_t> expected = BetaPayload(static_cast<uint32_t>(i));
    CHECK(std::memcmp(buf, expected.data(), size) == 0);
    CHECK(seq == i);
  }

  // 4. Layout mismatch: same key, different layout hash -> explicit failure.
  {
    ThymeStructDesc bad[3];
    for (int i = 0; i < 3; ++i) bad[i] = b_descs[i];
    bad[1].layout_hash = h_alpha ^ 0x1ULL;
    Ring mismatch = MakeRing("bad", bad, b_specs, 3);
    CliResult check = RunCli({"play", pkg, "--small", "small=" + mismatch.name,
                              "--check-compat", "--allow-missing"});
    CHECK(check.rc == replaycli::kExitTargets);
    CHECK(Contains(check.out, "layout"));
    CliResult play = RunCli({"play", pkg, "--small", "small=" + mismatch.name,
                             "--as-fast", "--allow-missing", "--no-gate"});
    CHECK(play.rc == replaycli::kExitTargets);
    // Nothing was injected under the mismatched mapping.
    CHECK(mismatch.client->Page()->RingHead(0) == 0);
    CHECK(mismatch.client->Page()->RingHead(1) == 0);
    mismatch.client->Shutdown(true);
  }

  // 5. Determinism: replay again into a fresh topology; matched channels must
  //    produce the same mono_ns/sequence/payload bytes.
  {
    Ring again = MakeRing("again", b_descs, b_specs, 3);
    CliResult r = RunCli({"play", pkg, "--small", "small=" + again.name,
                          "--as-fast", "--allow-missing", "--no-gate"});
    CHECK(r.rc == replaycli::kExitOk);
    const RecordMirrorRingPage* p2 = again.client->Page();
    CHECK(p2->RingHead(0) == page->RingHead(0));
    CHECK(p2->RingHead(1) == page->RingHead(1));
    for (uint64_t i = 0; i < 3; ++i) {
      uint8_t a[64];
      uint8_t c[64];
      uint32_t as = 0;
      uint32_t cs = 0;
      uint64_t am = 0;
      uint64_t cm = 0;
      uint32_t aseq = 0;
      uint32_t cseq = 0;
      CHECK(page->ReadRingSample(0, i, a, sizeof(a), as, am, aseq));
      CHECK(p2->ReadRingSample(0, i, c, sizeof(c), cs, cm, cseq));
      CHECK(as == cs && am == cm && aseq == cseq);
      CHECK(std::memcmp(a, c, as) == 0);
    }
    for (uint64_t i = 0; i < 2; ++i) {
      CheckAlphaSample(p2, 1, i, kT0 + i * 1000, static_cast<uint32_t>(i));
    }
    again.client->Shutdown(true);
  }

  b.client->Shutdown(true);
  std::filesystem::remove_all(root);
}

}  // namespace

int main() {
  TestTopologyChangeReplay();
  std::printf("test_topology_replay: OK\n");
  return 0;
}
