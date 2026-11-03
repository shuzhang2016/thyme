// test_record_manifest.cpp — ABI-10 record-manifest identity.
//
// RecordStructId is the row index into GEN-12's sorted recorded-channel list,
// so adding/removing a recorded channel renumbers rows: a package or perf log
// written under one manifest must never be decoded as if it were another.
//
// Covered:
//   * .tbin v3 carries the CRC-protected manifest block; a writer without a
//     manifest keeps the legacy v2/v1 bytes (manifest absent);
//   * a tampered/truncated/unset block is rejected at open (kInvalid), never
//     decoded;
//   * perf logs carry a type-7 block; thyme_log_merge accepts one manifest,
//     refuses a cross-file conflict and refuses --expect-hash on a mismatch or
//     on legacy logs without a block;
//   * a real trigger capture with a manifest: meta.tbin block + field-level
//     schema.json agree, the schema sidecar is covered by the integrity
//     manifest, replay list/verify/info accept the package and refuse an
//     expected-hash mismatch (including play, before injecting anything);
//   * a tampered/deleted sidecar and a legacy (no sidecar, no block) package
//     are handled explicitly;
//   * an invalidated (.incomplete) package still carries a *partial* schema.
#include "thyme/check.h"
#include "thyme/contracts/frame.h"
#include "thyme/record/package_integrity.h"
#include "thyme/record/record_mirror_ring_client.h"
#include "thyme/record/record_schema.h"
#include "thyme/record/thyme_bin.h"
#include "thyme/record/triggered_recorder.h"
#include "thyme/replay/replay_reader.h"
#include "thyme/runtime/perf_dump.h"
#include "thyme/runtime/perf_log.h"
#include "thyme/runtime/record_mirror_ring.h"
#include "thyme/runtime/shm_spmc_ring.h"

#include "log_merge.h"
#include "replay_cli.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <ctime>
#include <sys/mman.h>
#include <unistd.h>
#include <vector>

using namespace thyme;

namespace {

constexpr uint64_t kManifestHash = 0x9914c81f74037d59ull;

struct Pod16 {
  uint64_t a;
  uint32_t b;
  uint32_t c;
};
static_assert(sizeof(Pod16) == 16, "fixture arithmetic");
static_assert(offsetof(Pod16, b) == 8, "fixture layout");

const schema::FieldLayout kPodFields[] = {
    {"a", "uint64", 0, 8, 1, 8},
    {"b", "uint32", 8, 4, 1, 4},
    {"c", "uint32", 12, 4, 1, 4},
};
const schema::ContractLayout kPod = {"Pod16", sizeof(Pod16), 3,
                                     alignof(Pod16), kPodFields};

const schema::ContractLayout* FindPod(const char* cpp_type) {
  return (cpp_type != nullptr && std::strcmp(cpp_type, "Pod16") == 0)
             ? &kPod
             : nullptr;
}

schema::RecordManifest PodManifest(uint64_t hash = kManifestHash) {
  schema::RecordManifest m;
  m.hash = hash;
  m.version = schema::kManifestVersion;
  m.contract = &FindPod;
  return m;
}

ThymeStructDesc MakeDesc(uint16_t id, uint32_t payload, const char* name) {
  ThymeStructDesc d{};
  d.struct_id = id;
  d.contract_version = 1;
  d.payload_size = payload;
  std::snprintf(d.name, sizeof(d.name), "%s", name);
  return d;
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

std::string ReadTmpFile(FILE* f) {
  std::rewind(f);
  std::string body;
  char buf[4096];
  std::size_t n = 0;
  while ((n = std::fread(buf, 1, sizeof(buf), f)) != 0) body.append(buf, n);
  return body;
}

std::string ReadAll(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  return std::string((std::istreambuf_iterator<char>(in)),
                     std::istreambuf_iterator<char>());
}

void WriteAll(const std::string& path, const std::string& body) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out.write(body.data(), static_cast<std::streamsize>(body.size()));
}

std::string ReplaceOnce(const std::string& body, const std::string& from,
                        const std::string& to) {
  const std::size_t pos = body.find(from);
  CHECK(pos != std::string::npos);
  std::string out = body;
  out.replace(pos, from.size(), to);
  return out;
}

bool Contains(const std::string& body, const char* needle) {
  return body.find(needle) != std::string::npos;
}

void CopyTree(const std::string& from, const std::string& to) {
  std::filesystem::remove_all(to);
  std::filesystem::copy(from, to,
                        std::filesystem::copy_options::recursive |
                            std::filesystem::copy_options::overwrite_existing);
}

std::string Root() {
  const std::filesystem::path p =
      std::filesystem::temp_directory_path() / "thyme_record_manifest_test";
  std::filesystem::remove_all(p);
  std::filesystem::create_directories(p);
  return p.string();
}

// ---- .tbin manifest block ------------------------------------------------

const ThymeStructDesc kDemoDesc = MakeDesc(0, sizeof(Pod16), "Pod16");

std::string WriteTbin(const std::string& path,
                      const ThymeBinManifestBlock* block) {
  ThymeBinWriter w(path.c_str());
  CHECK(w.ok());
  CHECK(w.WriteHeader(7, 1000, &kDemoDesc, 1, block));
  const Pod16 pod{1, 2, 3};
  CHECK(w.WriteData(1, 0, 2000, 9, &pod, sizeof(pod)));
  CHECK(w.Finish());
  return path;
}

void TestTbinManifestBlock() {
  const std::string root = Root();
  const ThymeBinManifestBlock block =
      MakeThymeBinManifestBlock(kManifestHash, schema::kManifestVersion);

  const std::string v3 = WriteTbin(root + "/v3.tbin", &block);
  {
    ThymeBinReader r(v3.c_str());
    CHECK(r.ok());
    CHECK(r.header().file_abi_version == kThymeBinFileAbiVersion);
    CHECK(r.manifest_state() == ThymeBinReader::ManifestState::kOk);
    CHECK(r.manifest_hash() == kManifestHash);
    CHECK(r.manifest_version() == schema::kManifestVersion);
    ThymeBinDataEntry e{};
    CHECK(r.ReadNext(e) == ThymeBinReader::ReadResult::kOk);
    CHECK(e.payload_size == sizeof(Pod16));
  }

  // No manifest -> the legacy indexed format, byte-identical to pre-ABI-10.
  const std::string v2 = WriteTbin(root + "/v2.tbin", nullptr);
  {
    ThymeBinReader r(v2.c_str());
    CHECK(r.ok());
    CHECK(r.header().file_abi_version == kThymeBinFileAbiVersionIndexed);
    CHECK(r.manifest_state() == ThymeBinReader::ManifestState::kAbsent);
    CHECK(r.manifest_hash() == 0);
    ThymeBinDataEntry e{};
    CHECK(r.ReadNext(e) == ThymeBinReader::ReadResult::kOk);
  }

  // Tampered block (a flipped hash byte breaks the block CRC): refuse.
  {
    const std::string tampered = root + "/tampered.tbin";
    CopyTree(v3, tampered);
    std::string bytes = ReadAll(tampered);
    const std::size_t block_off =
        sizeof(ThymeBinFileHeader) + sizeof(ThymeStructDesc);
    const std::size_t at =
        block_off + offsetof(ThymeBinManifestBlock, manifest_hash);
    bytes[at] = static_cast<char>(static_cast<unsigned char>(bytes[at]) ^ 0xFF);
    WriteAll(tampered, bytes);
    ThymeBinReader r(tampered.c_str());
    CHECK(!r.ok());
    CHECK(r.manifest_state() == ThymeBinReader::ManifestState::kInvalid);
    CHECK(Contains(r.manifest_error(), "CRC"));
  }

  // A v3 header without a block is corruption, not legacy.
  {
    const std::string path = root + "/noblock.tbin";
    std::FILE* f = std::fopen(path.c_str(), "wb");
    CHECK(f != nullptr);
    ThymeBinFileHeader h{};
    std::memcpy(h.magic, kThymeBinMagic, sizeof(h.magic));
    h.file_abi_version = kThymeBinFileAbiVersion;
    h.struct_count = 1;
    CHECK(std::fwrite(&h, sizeof(h), 1, f) == 1);
    CHECK(std::fwrite(&kDemoDesc, sizeof(kDemoDesc), 1, f) == 1);
    std::fclose(f);
    ThymeBinReader r(path.c_str());
    CHECK(!r.ok());
    CHECK(r.manifest_state() == ThymeBinReader::ManifestState::kInvalid);
    CHECK(Contains(r.manifest_error(), "truncated"));
  }

  // A block whose manifest_version is unset is invalid even with a good CRC.
  {
    const std::string path = root + "/vzero.tbin";
    std::FILE* f = std::fopen(path.c_str(), "wb");
    CHECK(f != nullptr);
    ThymeBinFileHeader h{};
    std::memcpy(h.magic, kThymeBinMagic, sizeof(h.magic));
    h.file_abi_version = kThymeBinFileAbiVersion;
    h.struct_count = 1;
    CHECK(std::fwrite(&h, sizeof(h), 1, f) == 1);
    CHECK(std::fwrite(&kDemoDesc, sizeof(kDemoDesc), 1, f) == 1);
    ThymeBinManifestBlock b = MakeThymeBinManifestBlock(kManifestHash, 0);
    b.crc32 = 0;
    b.crc32 = Crc32(reinterpret_cast<const uint8_t*>(&b),
                    offsetof(ThymeBinManifestBlock, crc32));
    CHECK(std::fwrite(&b, sizeof(b), 1, f) == 1);
    std::fclose(f);
    ThymeBinReader r(path.c_str());
    CHECK(!r.ok());
    CHECK(r.manifest_state() == ThymeBinReader::ManifestState::kInvalid);
    CHECK(Contains(r.manifest_error(), "unset manifest version"));
  }

  // The encoder refuses an unset manifest instead of writing an unreadable file.
  {
    ThymeBinWriter w((root + "/refused.tbin").c_str());
    CHECK(w.ok());
    ThymeBinManifestBlock zero = MakeThymeBinManifestBlock(kManifestHash, 0);
    CHECK(!w.WriteHeader(1, 0, &kDemoDesc, 1, &zero));
  }
}

// ---- perf log manifest block --------------------------------------------

std::string WritePerfLog(const std::string& path, bool with_manifest,
                         uint64_t hash = kManifestHash) {
  PerfLogDumper d;
  CHECK(d.Open(path.c_str(), 0));
  if (with_manifest) {
    d.SetRecordManifest(hash, schema::kManifestVersion);
  }
  PerfSampler sampler(64);
  sampler.Note(NowNs(), 1200, 3, 0);
  CHECK(d.DumpIfDue(NowNs(), sampler, nullptr, 3));
  d.Close();
  return path;
}

void TestPerfManifestBlock() {
  const std::string root = Root();
  const std::string a = WritePerfLog(root + "/a.bin", true);
  const std::string legacy = WritePerfLog(root + "/legacy.bin", false);
  const std::string b = WritePerfLog(root + "/b.bin", true, 0xDEADBEEFCAFEF00Dull);

  // Wire shape: T9P1 then type 7, count 1, version 1, hash.
  {
    const std::string bytes = ReadAll(a);
    CHECK(bytes.size() > 4 + 6 + kPerfManifestRecordBytes);
    CHECK(bytes.compare(0, 4, "T9P1") == 0);
    CHECK(static_cast<uint8_t>(bytes[4]) == kPerfManifestBlockType);
    CHECK(static_cast<uint8_t>(bytes[6]) == 1);  // u32 count, little-endian
    CHECK(static_cast<uint8_t>(bytes[10]) == schema::kManifestVersion);
    uint64_t hash = 0;
    for (int i = 0; i < 8; ++i) {
      hash |= static_cast<uint64_t>(static_cast<uint8_t>(bytes[14 + i]))
              << (8 * i);
    }
    CHECK(hash == kManifestHash);
  }
  {
    const std::string bytes = ReadAll(legacy);
    CHECK(static_cast<uint8_t>(bytes[4]) != kPerfManifestBlockType);
  }

  logmerge::Options opt;
  FILE* out = std::tmpfile();
  CHECK(out != nullptr);
  FILE* diag = std::tmpfile();
  CHECK(diag != nullptr);

  {
    logmerge::ManifestReport rep;
    CHECK(logmerge::Merge({a}, opt, out, diag, &rep) == 0);
    CHECK(rep.declared);
    CHECK(!rep.conflict);
    CHECK(rep.hash == kManifestHash);
    CHECK(rep.version == schema::kManifestVersion);
    CHECK(rep.files == 1);
  }
  {
    // A legacy log alone is still mergeable (no manifest to disagree with).
    logmerge::ManifestReport rep;
    CHECK(logmerge::Merge({legacy}, opt, out, diag, &rep) == 0);
    CHECK(!rep.declared);
  }
  {
    // Two builds -> refuse to join RecordStructIds.
    logmerge::ManifestReport rep;
    CHECK(logmerge::Merge({a, b}, opt, out, diag, &rep) == 4);
    CHECK(rep.declared);
    CHECK(rep.conflict);
    CHECK(Contains(ReadTmpFile(diag), "different record manifests"));
  }
  {
    opt.have_expect_hash = true;
    opt.expect_hash = kManifestHash;
    CHECK(logmerge::Merge({a}, opt, out, diag, nullptr) == 0);
    opt.expect_hash = 0x1234;
    CHECK(logmerge::Merge({a}, opt, out, diag, nullptr) == 4);
    // Legacy log: cannot verify the expectation -> fail closed.
    CHECK(logmerge::Merge({legacy}, opt, out, diag, nullptr) == 4);
  }
  std::fclose(out);
  std::fclose(diag);
}

// ---- real capture: package meta block + schema sidecar -------------------

std::atomic<uint32_t> g_case{0};
int g_seal_fault = 0;
int SealFault(void*) { return g_seal_fault; }

std::string CapturePackage(const std::string& out_dir,
                           const schema::RecordManifest* manifest,
                           int seal_fault = 0) {
  const uint32_t n = g_case.fetch_add(1);
  char page[96];
  char ring[96];
  std::snprintf(page, sizeof(page), "/thyme_abi10_p_%d_%u",
                static_cast<int>(getpid()), n);
  std::snprintf(ring, sizeof(ring), "/thyme_abi10_r_%d_%u",
                static_cast<int>(getpid()), n);
  shm_unlink(page);
  shm_unlink(ring);

  ThymeStructDesc desc = MakeDesc(0, sizeof(Pod16), "Pod16");
  RecordMirrorRingSpec spec{sizeof(Pod16), 64};
  record_mirror::RingClient mirror;
  CHECK(mirror.Create(page, &desc, &spec, 1, 7));
  std::string dir;
  {
    ShmSpmcRing<Pod16, 64> ring_client;
    CHECK(ring_client.Create(ring));
    ring_client.SetRecordTap(mirror.Page(), 0);
    for (uint64_t i = 0; i < 16; ++i) {
      const Pod16 p{i + 1, static_cast<uint32_t>((i + 1) * 2),
                    static_cast<uint32_t>((i + 1) * 3)};
      ring_client.Push(p);
      SleepNs(200000);
    }

    triggered::Config cfg;
    cfg.sources = {{triggered::SourceConfig::Kind::kMirrorRing, page, ""}};
    cfg.out_dir = out_dir;
    cfg.post_grace_ns = 5000000ULL;
    cfg.record_manifest = manifest;
    g_seal_fault = seal_fault;
    if (seal_fault != 0) {
      cfg.fault_ctx = nullptr;
      cfg.fault_inject_seal = &SealFault;
    }
    triggered::TriggeredRecorder rec;
    CHECK(rec.Init(cfg));
    CHECK(rec.Trigger(42, 50000000ULL, 10000000ULL));
    const uint64_t deadline = NowNs() + 15000000000ULL;
    while (rec.phase() != triggered::Phase::kArmed) {
      rec.Tick();
      CHECK(NowNs() < deadline);
      SleepNs(100000);
    }
    dir = rec.last_package_dir();
    rec.Shutdown();
    ring_client.Close();
  }
  mirror.Shutdown(true);
  shm_unlink(page);
  shm_unlink(ring);
  return dir;
}

void TestPackageManifest() {
  const std::string root = Root();
  const schema::RecordManifest manifest = PodManifest();
  const std::string dir = CapturePackage(root, &manifest);

  CHECK(std::filesystem::exists(dir + "/" + schema::kFileName));
  CHECK(triggered::TriggeredRecorder::PackageComplete(dir));

  // meta.tbin is v3 and declares the same manifest as the sidecar.
  {
    ThymeBinReader r((dir + "/meta.tbin").c_str());
    CHECK(r.ok());
    CHECK(r.header().file_abi_version == kThymeBinFileAbiVersion);
    CHECK(r.manifest_state() == ThymeBinReader::ManifestState::kOk);
    CHECK(r.manifest_hash() == kManifestHash);
  }
  // The sidecar is field-level and agrees with the block.
  {
    schema::PackageSchema sc;
    std::string err;
    CHECK(schema::ReadPackageSchema(dir, &sc, &err) == schema::ReadState::kOk);
    CHECK(!sc.partial);
    CHECK(sc.manifest_declared);
    CHECK(sc.manifest_hash == kManifestHash);
    bool pod_found = false;
    for (const schema::RecordDesc& rec : sc.records) {
      if (rec.name == "Pod16") {
        pod_found = true;
        CHECK(rec.struct_id == 0);
        CHECK(rec.layout == schema::LayoutState::kKnown);
        CHECK(rec.payload_offset == 0);  // this fixture records bare Pod16
        CHECK(rec.fields.size() == 3);
        CHECK(rec.fields[2].offset == offsetof(Pod16, c));
      } else {
        // Internal control records are bare and have no field layout.
        CHECK(rec.layout == schema::LayoutState::kUnavailable);
      }
    }
    CHECK(pod_found);
  }
  // The integrity manifest covers the sidecar (no unlisted/missing file).
  {
    const integrity::VerifyResult v =
        integrity::VerifyPackageManifest(dir, /*signing_key_path=*/"");
    CHECK(v.state == integrity::ManifestState::kOk);
    CHECK(v.ok());
    bool covered = false;
    for (const integrity::FileStatus& f : v.files) {
      if (f.path == schema::kFileName) {
        covered = true;
        CHECK(f.hash_ok);
      }
    }
    CHECK(covered);
  }

  // Replay reader: schema + manifest are reported, list/verify succeed.
  replaycli::PackageReport rep;
  {
    std::ostringstream out;
    CHECK(replaycli::RunList(dir, false, out, &rep) == replaycli::kExitOk);
    CHECK(rep.schema_state == "ok");
    CHECK(rep.manifest_declared);
    CHECK(rep.manifest_hash == kManifestHash);
    CHECK(rep.schema_records >= 2);
    CHECK(Contains(out.str(), "schema  : present"));
    CHECK(Contains(out.str(), "manifest: 0x9914c81f74037d59"));
  }
  {
    std::ostringstream out;
    CHECK(replaycli::RunVerify(dir, false, out, &rep) == replaycli::kExitOk);
  }
  {
    std::ostringstream out;
    CHECK(replaycli::RunSchema(dir, false, out, &rep) == replaycli::kExitOk);
    CHECK(Contains(out.str(), "layout=known"));
    CHECK(Contains(out.str(), "field  : a uint64"));
  }
  // Expectation: matching hash passes, a different build is refused by every
  // reading command.
  {
    std::ostringstream out;
    CHECK(replaycli::RunList(dir, false, out, &rep, true, kManifestHash) ==
          replaycli::kExitOk);
  }
  {
    std::ostringstream out;
    CHECK(replaycli::RunInfo(dir, false, out, &rep, true, 0x1234) ==
          replaycli::kExitFailure);
    CHECK(Contains(rep.error, "record-manifest mismatch"));
  }
  {
    std::ostringstream out;
    CHECK(replaycli::RunVerify(dir, false, out, &rep, true, 0x1234) ==
          replaycli::kExitFailure);
  }
  {
    replaycli::PlayOptions opt;
    opt.package_dir = dir;
    opt.have_expect_manifest_hash = true;
    opt.expect_manifest_hash = 0x1234;
    std::ostringstream out;
    replaycli::PlaySummary summary;
    CHECK(replaycli::RunPlay(opt, nullptr, out, &summary) ==
          replaycli::kExitFailure);
    CHECK(Contains(summary.error, "record-manifest mismatch"));
  }

  // Tamper with the sidecar's manifest: the independent meta.tbin block
  // catches it.
  {
    const std::string tampered = root + "/tampered_hash";
    CopyTree(dir, tampered);
    std::string body = ReadAll(tampered + "/" + schema::kFileName);
    body = ReplaceOnce(body, schema::FormatManifestHash(kManifestHash),
                       "0x0000000000000000");
    WriteAll(tampered + "/" + schema::kFileName, body);
    std::ostringstream out;
    CHECK(replaycli::RunList(tampered, false, out, &rep) ==
          replaycli::kExitFailure);
    CHECK(Contains(rep.error, "differs from the meta.tbin block"));
    CHECK(replaycli::RunVerify(tampered, false, out, &rep) ==
          replaycli::kExitFailure);
  }
  // Tamper with a field layout: strict JSON validation refuses it.
  {
    const std::string tampered = root + "/tampered_field";
    CopyTree(dir, tampered);
    std::string body = ReadAll(tampered + "/" + schema::kFileName);
    body = ReplaceOnce(body, "\"count\":1,\"stride\":8",
                       "\"count\":2,\"stride\":8");
    WriteAll(tampered + "/" + schema::kFileName, body);
    std::ostringstream out;
    CHECK(replaycli::RunList(tampered, false, out, &rep) ==
          replaycli::kExitFailure);
    CHECK(Contains(rep.error, "size != count * stride"));
  }
  // Deleting the sidecar from a package that declares a manifest is tamper,
  // not legacy.
  {
    const std::string missing = root + "/missing_schema";
    CopyTree(dir, missing);
    std::filesystem::remove(missing + "/" + schema::kFileName);
    std::ostringstream out;
    CHECK(replaycli::RunList(missing, false, out, &rep) ==
          replaycli::kExitFailure);
    CHECK(Contains(rep.error, "sidecar missing"));
  }
  // The schema command reports absence explicitly (exit non-zero).
  {
    const std::string missing = root + "/missing_schema";
    std::ostringstream out;
    CHECK(replaycli::RunSchema(missing, false, out, &rep) ==
          replaycli::kExitFailure);
    CHECK(Contains(rep.error, "no .tbin carries one") ||
          Contains(rep.error, "schema"));
  }
}

void TestInvalidatedPackagePartialSchema() {
  const std::string root = Root();
  const schema::RecordManifest manifest = PodManifest();
  const std::string dir = CapturePackage(root, &manifest, /*seal_fault=*/4);
  const std::string incomplete = dir + ".incomplete";
  CHECK(std::filesystem::exists(incomplete));
  CHECK(!std::filesystem::exists(dir));
  CHECK(std::filesystem::exists(incomplete + "/" + schema::kFileName));

  schema::PackageSchema sc;
  std::string err;
  CHECK(schema::ReadPackageSchema(incomplete, &sc, &err) ==
        schema::ReadState::kOk);
  CHECK(sc.partial);
  CHECK(sc.manifest_declared);
  CHECK(sc.manifest_hash == kManifestHash);
  bool pod_found = false;
  for (const schema::RecordDesc& rec : sc.records) {
    if (rec.name == "Pod16") {
      pod_found = true;
      CHECK(rec.layout == schema::LayoutState::kKnown);
    }
  }
  CHECK(pod_found);

  // The partial integrity manifest covers the partial sidecar.
  const integrity::VerifyResult v =
      integrity::VerifyPackageManifest(incomplete, "");
  CHECK(v.state == integrity::ManifestState::kOk);
  CHECK(v.partial);
  CHECK(v.intact());
}

void TestLegacyPackage() {
  const std::string root = Root();
  const std::string dir = root + "/legacy";
  std::filesystem::create_directories(dir);
  // A pre-ABI-09/10 package: v2 meta.tbin, no manifest block, no sidecar.
  {
    ThymeBinWriter w((dir + "/meta.tbin").c_str());
    CHECK(w.ok());
    CHECK(w.WriteHeader(99, 0, &kDemoDesc, 1));
    const Pod16 pod{1, 2, 3};
    CHECK(w.WriteData(1, 0, 1000, 1, &pod, sizeof(pod)));
    CHECK(w.Finish());
  }

  replaycli::PackageReport rep;
  {
    std::ostringstream out;
    CHECK(replaycli::RunList(dir, false, out, &rep) == replaycli::kExitOk);
    CHECK(rep.schema_state == "absent");
    CHECK(!rep.manifest_declared);
    CHECK(Contains(out.str(), "manifest: absent"));
  }
  {
    // Legacy packages stay readable and verifiable.
    std::ostringstream out;
    CHECK(replaycli::RunVerify(dir, false, out, &rep) == replaycli::kExitOk);
  }
  {
    // ... but they cannot be verified against a build's manifest: fail closed.
    std::ostringstream out;
    CHECK(replaycli::RunList(dir, false, out, &rep, true, kManifestHash) ==
          replaycli::kExitFailure);
    CHECK(Contains(rep.error, "legacy"));
  }
  {
    std::ostringstream out;
    CHECK(replaycli::RunSchema(dir, false, out, &rep) ==
          replaycli::kExitFailure);
    CHECK(Contains(rep.error, "no schema.json"));
  }
}

}  // namespace

int main() {
  TestTbinManifestBlock();
  TestPerfManifestBlock();
  TestPackageManifest();
  TestInvalidatedPackagePartialSchema();
  TestLegacyPackage();
  std::printf("test_record_manifest: OK\n");
  return 0;
}
