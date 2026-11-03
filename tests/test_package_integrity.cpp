// test_package_integrity.cpp — SEC-03 evidence manifests + REC-18 partial
// manifests for invalidated (.incomplete) packages.
//
// A real TriggeredRecorder capture (mirror-ring source, sealed package) is
// used for the happy path; the tamper/absent/signature cases copy or edit the
// resulting package so each scenario is independent of the recorder.
//
// Covered:
//   * clean seal -> manifest present, per-file sha256 + chain + trigger window
//     ids verify OK (offline, no daemon);
//   * one modified byte -> that file mismatches and the chain root differs;
//   * one deleted file -> missing + chain failure;
//   * an extra unlisted file -> unlisted + failure;
//   * an edited manifest -> invalid (self-consistency chain);
//   * absent manifest (pre-SEC-03 package) -> "absent", not a crash;
//   * HMAC-SHA256: correct key OK, wrong key fails, missing key fails closed;
//   * the thyme-replay `integrity` command mirrors these results and exits
//     non-zero on every failure;
//   * REC-18: a big-frame channel's frames/<name>.raw + .index.tbin bulk
//     payload is covered by the manifest; a flipped byte in the .raw is
//     detected;
//   * REC-18: a fault-injected seal failure invalidates the package AND
//     leaves a partial manifest over the survivors (signed, when a key is
//     configured) -> verify reports partial + intact, CLI prints PARTIAL and
//     exits non-zero; a tampered survivor flips it to FAILED;
//   * REC-18: the retention startup sweep manifests a crash-leftover
//     (unsealed wal) package, recovering the session id from the wal header.
#include "thyme/check.h"
#include "thyme/record/clock_anchor.h"
#include "thyme/record/package_integrity.h"
#include "thyme/record/record_mirror_ring_client.h"
#include "thyme/record/retention.h"

#include "session_lease_cleanup.h"
#include "thyme/record/triggered_recorder.h"
#include "thyme/runtime/pinned_slot_store.h"
#include "thyme/runtime/record_mirror_ring.h"
#include "thyme/runtime/shm_spmc_ring.h"

#include "replay_cli.h"
#include "../src/record/triggered_recorder_internal.h"

#include <atomic>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

using namespace thyme;

#if defined(__linux__)
namespace {
std::atomic<bool> g_count_reads{false};
std::atomic<uint64_t> g_read_bytes{0};
}  // namespace

extern "C" ssize_t __real_read(int fd, void* data, std::size_t n);
extern "C" ssize_t __wrap_read(int fd, void* data, std::size_t n) {
  const ssize_t result = __real_read(fd, data, n);
  if (g_count_reads.load() && result > 0) {
    g_read_bytes.fetch_add(static_cast<uint64_t>(result));
  }
  return result;
}
#endif

namespace {

struct Pod16 {
  uint64_t a;
  uint32_t b;
  uint32_t c;
};

std::atomic<uint32_t> g_case{0};

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

ThymeStructDesc MakeDesc(uint16_t sid, uint32_t payload, const char* name) {
  ThymeStructDesc d{};
  d.struct_id = sid;
  d.contract_version = 1;
  d.payload_size = payload;
  std::snprintf(d.name, sizeof(d.name), "%s", name);
  return d;
}

void RunCapture(triggered::TriggeredRecorder& rec) {
  const uint64_t deadline = NowNs() + 15000000000ULL;
  while (rec.phase() != triggered::Phase::kArmed) {
    rec.Tick();
    CHECK(NowNs() < deadline);
    SleepNs(100000);
  }
}

// Captures one package into out_dir and returns its path. key_path empty =
// unsigned manifest.
std::string CapturePackage(const std::string& out_dir,
                           const std::string& key_path) {
  const uint32_t n = g_case.fetch_add(1);
  char page[96];
  char ring[96];
  std::snprintf(page, sizeof(page), "/thyme_sec03_p_%d_%u",
                static_cast<int>(getpid()), n);
  std::snprintf(ring, sizeof(ring), "/thyme_sec03_r_%d_%u",
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
    for (uint64_t i = 0; i < 32; ++i) {
      const Pod16 p{i + 1, static_cast<uint32_t>((i + 1) * 2),
                    static_cast<uint32_t>((i + 1) * 3)};
      ring_client.Push(p);
      SleepNs(200000);
    }

    triggered::Config cfg;
    cfg.sources = {{triggered::SourceConfig::Kind::kMirrorRing, page, ""}};
    cfg.out_dir = out_dir;
    cfg.post_grace_ns = 5000000ULL;
    cfg.signing_key_path = key_path;
    triggered::TriggeredRecorder rec;
    CHECK(rec.Init(cfg));
    CHECK(rec.Trigger(1, 50000000ULL, 10000000ULL));
#if defined(__linux__)
    g_read_bytes.store(0);
    g_count_reads.store(key_path.empty());
#endif
    RunCapture(rec);
#if defined(__linux__)
    g_count_reads.store(false);
    if (key_path.empty()) CHECK(g_read_bytes.load() == 0);
#endif
    dir = rec.last_package_dir();
    CHECK(triggered::TriggeredRecorder::PackageComplete(dir));
    rec.Shutdown();
    ring_client.Close();
  }
  mirror.Shutdown(true);
  shm_unlink(page);
  shm_unlink(ring);
  return dir;
}

void WriteText(const std::string& path, const std::string& text) {
  std::ofstream f(path, std::ios::binary | std::ios::trunc);
  CHECK(f.good());
  f.write(text.data(), static_cast<std::streamsize>(text.size()));
  CHECK(f.good());
}

std::string ReadBinary(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  CHECK(f.good());
  return std::string(std::istreambuf_iterator<char>(f),
                     std::istreambuf_iterator<char>());
}

void ExpectSameAsFullHashManifest(const std::string& dir,
                                  const integrity::PackageInfo& info,
                                  const std::string& key_path) {
  const std::string manifest_path = dir + "/integrity.manifest";
  const std::string signature_path = dir + "/integrity.hmac";
  const std::string expected_manifest = ReadBinary(manifest_path);
  const bool signed_manifest = !key_path.empty();
  const std::string expected_signature =
      signed_manifest ? ReadBinary(signature_path) : std::string{};
  std::string err;
  CHECK(integrity::WritePackageManifest(dir, info, key_path, &err));
  const std::string actual_manifest = ReadBinary(manifest_path);
  CHECK(actual_manifest == expected_manifest);
  if (signed_manifest) CHECK(ReadBinary(signature_path) == expected_signature);
}

void FlipLastByte(const std::string& path) {
  std::fstream f(path, std::ios::in | std::ios::out | std::ios::binary);
  CHECK(f.good());
  f.seekg(0, std::ios::end);
  const std::streamoff size = f.tellg();
  CHECK(size > 0);
  f.seekg(size - 1, std::ios::beg);
  char c = 0;
  f.read(&c, 1);
  CHECK(f.good());
  c = static_cast<char>(c ^ 0x5A);
  f.seekp(size - 1, std::ios::beg);
  f.write(&c, 1);
  CHECK(f.good());
}

const integrity::FileStatus* FindFile(const integrity::VerifyResult& r,
                                      const std::string& path) {
  for (const integrity::FileStatus& f : r.files) {
    if (f.path == path) return &f;
  }
  return nullptr;
}

void ExpectManifestOk(const std::string& dir, const std::string& key) {
  const integrity::VerifyResult r =
      integrity::VerifyPackageManifest(dir, key);
  CHECK(r.state == integrity::ManifestState::kOk);
  CHECK(r.ok());
  CHECK(r.chain_ok);
  CHECK(r.files_missing == 0);
  CHECK(r.files_mismatch == 0);
  CHECK(r.files_unlisted == 0);
  CHECK(r.files.size() >= 3);  // meta.tbin + small.tbin + wal.log
  CHECK(r.files_ok == r.files.size());
  CHECK(r.info.kind == integrity::PackageKind::kTrigger);
  CHECK(r.info.manifest_mode == integrity::ManifestMode::kFull);
  CHECK(r.info.trigger_id == 1);
  CHECK(r.info.session_id == 1);
  CHECK(r.info.t0_ns != 0);
  CHECK(r.info.before_ns == 50000000ULL);
  CHECK(r.info.after_ns == 10000000ULL);
  CHECK(r.info.seal_time_ns != 0);
  // The manifest covers every file but itself.
  CHECK(FindFile(r, "integrity.manifest") == nullptr);
  CHECK(FindFile(r, "meta.tbin") != nullptr);
  CHECK(FindFile(r, "wal.log") != nullptr);
  std::filesystem::path manifest = dir;
  manifest /= "integrity.manifest";
  CHECK(std::filesystem::exists(manifest));
}

void TestSealVerifyAndTamper(const std::filesystem::path& root) {
  const std::string pkg =
      CapturePackage((root / "unsigned").string(), "");
  ExpectManifestOk(pkg, "");
  const integrity::VerifyResult sealed =
      integrity::VerifyPackageManifest(pkg, "");
#if defined(__linux__)
  g_read_bytes.store(0);
  g_count_reads.store(true);
#endif
  ExpectSameAsFullHashManifest(pkg, sealed.info, "");
#if defined(__linux__)
  g_count_reads.store(false);
  CHECK(g_read_bytes.load() != 0);  // the legacy comparison really re-read files
#endif

  // CLI (daemon-free) reports the same success.
  {
    std::ostringstream out;
    std::ostringstream err;
    CHECK(replaycli::RunCli({"integrity", pkg}, out, err, nullptr) ==
          replaycli::kExitOk);
    CHECK(out.str().find("integrity: OK") != std::string::npos);
    CHECK(out.str().find("chain    : OK") != std::string::npos);
  }
  // `verify --integrity` runs both the data checks and the manifest checks.
  {
    std::ostringstream out;
    std::ostringstream err;
    CHECK(replaycli::RunCli({"verify", pkg, "--integrity"}, out, err,
                            nullptr) == replaycli::kExitOk);
    CHECK(out.str().find("verify  : OK") != std::string::npos);
    CHECK(out.str().find("integrity: OK") != std::string::npos);
  }

  // ---- one modified byte: per-file mismatch + chain failure -------------
  {
    const std::filesystem::path copy = root / "tampered";
    std::filesystem::copy(pkg, copy,
                          std::filesystem::copy_options::recursive |
                              std::filesystem::copy_options::overwrite_existing);
    const std::string target = (copy / "meta.tbin").string();
    FlipLastByte(target);
    const integrity::VerifyResult r =
        integrity::VerifyPackageManifest(copy.string(), "");
    CHECK(r.state == integrity::ManifestState::kOk);
    CHECK(!r.ok());
    CHECK(!r.chain_ok);
    CHECK(r.files_mismatch == 1);
    const integrity::FileStatus* f = FindFile(r, "meta.tbin");
    CHECK(f != nullptr);
    CHECK(f->present);
    CHECK(!f->hash_ok || !f->size_ok);
    std::ostringstream out;
    std::ostringstream err;
    CHECK(replaycli::RunCli({"integrity", copy.string()}, out, err, nullptr) ==
          replaycli::kExitFailure);
    CHECK(out.str().find("MISMATCH") != std::string::npos);
    CHECK(out.str().find("integrity: FAILED") != std::string::npos);
  }

  // ---- one deleted file --------------------------------------------------
  {
    const std::filesystem::path copy = root / "missing";
    std::filesystem::copy(pkg, copy,
                          std::filesystem::copy_options::recursive |
                              std::filesystem::copy_options::overwrite_existing);
    CHECK(std::filesystem::remove(copy / "wal.log"));
    const integrity::VerifyResult r =
        integrity::VerifyPackageManifest(copy.string(), "");
    CHECK(r.state == integrity::ManifestState::kOk);
    CHECK(!r.ok());
    CHECK(!r.chain_ok);
    CHECK(r.files_missing == 1);
  }

  // ---- extra unlisted file ----------------------------------------------
  {
    const std::filesystem::path copy = root / "extra";
    std::filesystem::copy(pkg, copy,
                          std::filesystem::copy_options::recursive |
                              std::filesystem::copy_options::overwrite_existing);
    WriteText((copy / "injected.raw").string(), "fabricated evidence");
    const integrity::VerifyResult r =
        integrity::VerifyPackageManifest(copy.string(), "");
    CHECK(!r.ok());
    CHECK(r.files_unlisted == 1);
    CHECK(r.unlisted.size() == 1);
    CHECK(r.unlisted[0] == "injected.raw");
    // The listed files themselves are untouched: the chain still matches.
    CHECK(r.chain_ok);
  }

  // ---- edited manifest -> invalid (chain root no longer matches) --------
  {
    const std::filesystem::path copy = root / "edited_manifest";
    std::filesystem::copy(pkg, copy,
                          std::filesystem::copy_options::recursive |
                              std::filesystem::copy_options::overwrite_existing);
    FlipLastByte((copy / "integrity.manifest").string());
    const integrity::VerifyResult r =
        integrity::VerifyPackageManifest(copy.string(), "");
    CHECK(r.state == integrity::ManifestState::kInvalid);
    CHECK(!r.ok());
    CHECK(r.error.find("chain") != std::string::npos);
  }

  // ---- absent manifest: aborted/legacy package --------------------------
  {
    const std::filesystem::path copy = root / "absent";
    std::filesystem::copy(pkg, copy,
                          std::filesystem::copy_options::recursive |
                              std::filesystem::copy_options::overwrite_existing);
    CHECK(std::filesystem::remove(copy / "integrity.manifest"));
    const integrity::VerifyResult r =
        integrity::VerifyPackageManifest(copy.string(), "");
    CHECK(r.state == integrity::ManifestState::kAbsent);
    CHECK(!r.ok());
    CHECK(r.error.find("absent") != std::string::npos ||
          r.error.find("no integrity manifest") != std::string::npos);
    std::ostringstream out;
    std::ostringstream err;
    CHECK(replaycli::RunCli({"integrity", copy.string()}, out, err, nullptr) ==
          replaycli::kExitFailure);
    CHECK(out.str().find("absent") != std::string::npos);
  }
}

void TestIncrementalWriteContract(const std::filesystem::path& root) {
  const std::filesystem::path dir = root / "incremental_contract";
  std::filesystem::create_directories(dir);
  const std::string file = (dir / "contract.bin").string();
  integrity::PackageHashState hashes;
  hashes.Track("contract.bin");

  std::FILE* raw = std::fopen(file.c_str(), "wb");
  CHECK(raw != nullptr);
  CHECK(triggered::detail::WritePackageBytes(raw, "abc", 3, &hashes,
                                             "contract.bin") == 3);
  CHECK(std::fseek(raw, 0, SEEK_SET) == 0);
  CHECK(triggered::detail::WritePackageBytes(raw, "Z", 1, &hashes,
                                             "contract.bin") == 0);
  CHECK(std::fclose(raw) == 0);

  integrity::PackageInfo info;
  std::string err;
  CHECK(integrity::WritePackageManifestFromHashes(
      dir.string(), info, "", hashes, &err));

  {
    std::ofstream append(file, std::ios::binary | std::ios::app);
    append << 'd';
  }
  err.clear();
  CHECK(!integrity::WritePackageManifestFromHashes(
      dir.string(), info, "", hashes, &err));
  CHECK(err.find("size") != std::string::npos);

  // Same-size external tampering is outside the incremental trust boundary:
  // generation cannot prove those bytes, so the independent verifier must fail.
  WriteText(file, "ZZZ");
  hashes.Replace("contract.bin", "abc", 3);
  CHECK(integrity::WritePackageManifestFromHashes(
      dir.string(), info, "", hashes, &err));
  const integrity::VerifyResult verify =
      integrity::VerifyPackageManifest(dir.string(), "");
  CHECK(!verify.ok());
  CHECK(verify.files_mismatch == 1);
}

void TestHashCheckpoint(const std::filesystem::path& root) {
  const std::filesystem::path dir = root / "checkpoint_parent" / "pkg";
  const std::string data(1024 * 1024, 'x');
  auto MakeRecovery = [&](uint64_t session) {
    std::filesystem::create_directories(dir);
    WriteText((dir / "data.bin").string(), data);
    triggered::WalHeader wh{};
    std::memcpy(wh.magic, triggered::kWalMagic, sizeof(triggered::kWalMagic));
    wh.trigger_id = session;
    const triggered::WalRow row{triggered::kWalOpDescriptor,
                                triggered::kWalOk,
                                1,
                                0,
                                0,
                                7};
    std::string wal(reinterpret_cast<const char*>(&wh), sizeof(wh));
    wal.append(reinterpret_cast<const char*>(&row), sizeof(row));
    WriteText((dir / "wal.log").string(), wal);
    integrity::PackageHashState state;
    state.Track("data.bin");
    state.Append("data.bin", data.data(), data.size());
    state.Track("wal.log");
    state.Append("wal.log", &wh, sizeof(wh));
    state.Append("wal.log", &row, sizeof(row));
    CHECK(state.SaveCheckpoint(dir.string(), session));
  };

  MakeRecovery(42);
  integrity::PackageHashState restored;
  CHECK(restored.LoadCheckpoint(dir.string(), 42));
  CHECK(!restored.LoadCheckpoint(dir.string(), 43));
  const auto first = restored.DigestFiles();
  const auto second = restored.DigestFiles();
  CHECK(first.size() == second.size());
  for (std::size_t i = 0; i < first.size(); ++i) {
    CHECK(first[i].path == second[i].path);
    CHECK(first[i].size == second[i].size);
    CHECK(std::memcmp(first[i].sha256, second[i].sha256, 32) == 0);
  }

  const std::string checkpoint = (dir / "integrity.checkpoint").string();
  const auto checkpoint_size = std::filesystem::file_size(checkpoint);
  std::filesystem::resize_file(checkpoint, checkpoint_size - 1);
  CHECK(!restored.LoadCheckpoint(dir.string(), 42));
  MakeRecovery(42);
  FlipLastByte(checkpoint);
  CHECK(!restored.LoadCheckpoint(dir.string(), 42));

  MakeRecovery(42);
  retention::Manager manager;
  retention::Config cfg;
  cfg.out_dir = dir.parent_path().string();
  CHECK(manager.Init(cfg));
  CHECK(!triggered::TriggeredRecorder::PackageComplete(dir.string()));
  WriteText((dir / ".integrity.checkpoint.tmp.crash").string(), "partial");
  g_read_bytes.store(0);
  g_count_reads.store(true);
  CHECK(manager.MarkIncomplete() == 1);
  g_count_reads.store(false);
  CHECK(g_read_bytes.load() < 4096);  // Small WAL only, not the 1 MiB payload.
  const std::string incomplete = dir.string() + ".incomplete";
  const integrity::VerifyResult recovered =
      integrity::VerifyPackageManifest(incomplete, "");
  CHECK(recovered.partial && recovered.intact());
  CHECK(FindFile(recovered, "integrity.checkpoint") == nullptr);
  CHECK(FindFile(recovered, ".integrity.checkpoint.tmp.crash") == nullptr);

  // Corrupt checkpoints never take the fast path: recovery must re-read all
  // payload bytes rather than accept a stale manifest.
  std::filesystem::remove_all(incomplete);
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);
  MakeRecovery(42);
  FlipLastByte(checkpoint);
  g_read_bytes.store(0);
  g_count_reads.store(true);
  retention::Manager fallback;
  CHECK(fallback.Init(cfg));
  CHECK(fallback.MarkIncomplete() == 1);
  g_count_reads.store(false);
  CHECK(g_read_bytes.load() >= data.size());

  MakeRecovery(42);
  {
    std::ofstream append((dir / "data.bin").string(),
                         std::ios::binary | std::ios::app);
    append << 'd';
  }
  CHECK(!restored.LoadCheckpoint(dir.string(), 42));

  // WAL seal/abort can overwrite bytes without changing its size. Recovery
  // rehashes this small mutable journal while retaining the payload fast path.
  std::filesystem::remove_all(incomplete);
  std::filesystem::remove_all(dir);
  MakeRecovery(42);
  FlipLastByte((dir / "wal.log").string());
  g_read_bytes.store(0);
  g_count_reads.store(true);
  retention::Manager mutable_wal;
  CHECK(mutable_wal.Init(cfg));
  CHECK(mutable_wal.MarkIncomplete() == 1);
  g_count_reads.store(false);
  CHECK(g_read_bytes.load() < 4096);
  const integrity::VerifyResult recovered_wal =
      integrity::VerifyPackageManifest(incomplete, "");
  CHECK(recovered_wal.partial && recovered_wal.intact());
}

void TestCheckpointAfterSigkill(const std::filesystem::path& root) {
  const std::string out = (root / "checkpoint_sigkill").string();
  const std::string page =
      "/thyme_checkpoint_" + std::to_string(static_cast<int>(getpid()));
  std::filesystem::create_directories(out);
  ShmRegion::UnlinkName(page.c_str());
  const pid_t child = fork();
  CHECK(child >= 0);
  if (child == 0) {
    const ThymeStructDesc desc = MakeDesc(0, sizeof(Pod16), "Pod16");
    const RecordMirrorRingSpec spec{sizeof(Pod16), 64};
    record_mirror::RingClient mirror;
    CHECK(mirror.Create(page.c_str(), &desc, &spec, 1, 7));
    triggered::Config cfg;
    cfg.sources = {{triggered::SourceConfig::Kind::kMirrorRing, page, ""}};
    cfg.out_dir = out;
    triggered::TriggeredRecorder recorder;
    CHECK(recorder.Init(cfg));
    triggered::ContinuousConfig continuous;
    continuous.slice_bytes = 1024 * 1024;
    CHECK(recorder.ArmContinuous(continuous));
    const Pod16 pod{1, 2, 3};
    CHECK(mirror.Page()->TryWriteRing(0, &pod, sizeof(pod), NowNs(), 1));
    CHECK(recorder.Trigger(73, 1000000, 60000000000ULL, 0));
    for (int i = 0; i < 20; ++i) {
      recorder.Tick();
      SleepNs(100000);
    }
    SleepNs(1100000000ULL);
    recorder.Tick();
    raise(SIGKILL);
    _exit(1);
  }
  int status = 0;
  CHECK(waitpid(child, &status, 0) == child);
  CHECK(WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL);
  ShmRegion::UnlinkName(page.c_str());

  std::size_t checkpoints = 0;
  for (const auto& entry : std::filesystem::directory_iterator(out)) {
    if (entry.is_directory() &&
        std::filesystem::exists(
            entry.path() / integrity::PackageHashState::kCheckpointName)) {
      ++checkpoints;
    }
  }
  CHECK(checkpoints == 2);

  retention::Manager manager;
  retention::Config cfg;
  cfg.out_dir = out;
  CHECK(manager.Init(cfg));
  CHECK(manager.MarkIncomplete() == 2);
  bool saw_trigger = false;
  bool saw_continuous = false;
  for (const auto& entry : std::filesystem::directory_iterator(out)) {
    if (!entry.is_directory() ||
        entry.path().extension() != ".incomplete") {
      continue;
    }
    const integrity::VerifyResult result =
        integrity::VerifyPackageManifest(entry.path().string(), "");
    CHECK(result.partial && result.intact());
    saw_trigger = saw_trigger ||
                  result.info.kind == integrity::PackageKind::kTrigger;
    saw_continuous = saw_continuous ||
                     result.info.kind == integrity::PackageKind::kContinuous;
  }
  CHECK(saw_trigger);
  CHECK(saw_continuous);
}

void TestClockAnchors(const std::filesystem::path& root) {
  // ARCH-15 S3: latest-anchor-at-or-before, no interpolation, overflow refuse.
  // Between (100,1000) and (200,5000) the linear interpolate of 150 would be
  // 3000; the contract is 1050 (carry the first anchor's delta).
  const std::vector<triggered::ClockAnchor> anchors{{100, 1000}, {200, 5000}};
  uint64_t realtime = 0;
  CHECK(triggered::MonoToRealtimeNs(anchors, 50, &realtime));
  CHECK(realtime == 950);
  CHECK(triggered::MonoToRealtimeNs(anchors, 150, &realtime));
  CHECK(realtime == 1050);
  CHECK(realtime != 3000);
  CHECK(triggered::MonoToRealtimeNs(anchors, 250, &realtime));
  CHECK(realtime == 5050);
  CHECK(!triggered::MonoToRealtimeNs(std::vector<triggered::ClockAnchor>{}, 1,
                                     &realtime));
  CHECK(!triggered::MonoToRealtimeNs(
      triggered::ClockAnchor{0, UINT64_MAX}, 1, &realtime));
  CHECK(!triggered::MonoToRealtimeNs(triggered::ClockAnchor{1, 0}, 0,
                                     &realtime));

  const std::string package =
      CapturePackage((root / "clock_anchor").string(), "");
  std::vector<triggered::ClockAnchor> persisted;
  CHECK(triggered::ReadPackageClockAnchors(package, &persisted));
  CHECK(persisted.size() >= 2);
  for (const auto& anchor : persisted) {
    CHECK(anchor.mono_ns != 0);
    CHECK(anchor.realtime_ns != 0);
    CHECK(triggered::MonoToRealtimeNs(anchor, anchor.mono_ns, &realtime));
    CHECK(realtime == anchor.realtime_ns);
  }

  const std::filesystem::path legacy = root / "clock_legacy";
  std::filesystem::create_directories(legacy);
  const triggered::WalHeader header{{'T', 'R', 'I', 'G', 'W', 'A', 'L', '1'}, 1};
  const triggered::WalRow seal{triggered::kWalOpSeal, 0, 0, 0, 0, 1};
  std::string wal(reinterpret_cast<const char*>(&header), sizeof(header));
  wal.append(reinterpret_cast<const char*>(&seal), sizeof(seal));
  WriteText((legacy / "wal.log").string(), wal);
  CHECK(triggered::ReadPackageClockAnchors(legacy.string(), &persisted));
  CHECK(persisted.empty());

  const std::filesystem::path bad = root / "clock_bad";
  std::filesystem::create_directories(bad);
  WriteText((bad / "wal.log").string(), "truncated");
  CHECK(!triggered::ReadPackageClockAnchors(bad.string(), &persisted));
}

void TestSigning(const std::filesystem::path& root) {
  const std::string key_path = (root / "signing.key").string();
  const std::string wrong_path = (root / "wrong.key").string();
  WriteText(key_path, "correct-horse-battery-staple\n");
  WriteText(wrong_path, "not-the-key\n");

  const std::string pkg = CapturePackage((root / "signed").string(), key_path);
  const integrity::VerifyResult good =
      integrity::VerifyPackageManifest(pkg, key_path);
  CHECK(good.state == integrity::ManifestState::kOk);
  CHECK(good.signed_manifest);
  CHECK(good.signature_present);
  CHECK(good.signature_checked);
  CHECK(good.signature_ok);
  CHECK(good.ok());
  CHECK(std::filesystem::exists(std::filesystem::path(pkg) /
                                "integrity.hmac"));
  ExpectSameAsFullHashManifest(pkg, good.info, key_path);

  const integrity::VerifyResult wrong =
      integrity::VerifyPackageManifest(pkg, wrong_path);
  CHECK(wrong.signed_manifest);
  CHECK(wrong.signature_checked);
  CHECK(!wrong.signature_ok);
  CHECK(!wrong.ok());
  CHECK(wrong.chain_ok);  // content is intact; only authenticity fails

  const integrity::VerifyResult no_key =
      integrity::VerifyPackageManifest(pkg, "");
  CHECK(no_key.signed_manifest);
  CHECK(!no_key.signature_checked);
  CHECK(!no_key.ok());
  CHECK(no_key.error.find("--key") != std::string::npos);

  // CLI: correct key passes, wrong key fails.
  {
    std::ostringstream out;
    std::ostringstream err;
    CHECK(replaycli::RunCli({"integrity", pkg, "--key", key_path}, out, err,
                            nullptr) == replaycli::kExitOk);
    CHECK(out.str().find("signature: OK") != std::string::npos);
  }
  {
    std::ostringstream out;
    std::ostringstream err;
    CHECK(replaycli::RunCli({"integrity", pkg, "--key", wrong_path}, out, err,
                            nullptr) == replaycli::kExitFailure);
    CHECK(out.str().find("signature: FAILED") != std::string::npos);
  }

  // A signed manifest whose signature file is removed fails closed even with
  // the right key.
  {
    const std::filesystem::path copy = root / "signed_no_sig";
    std::filesystem::copy(pkg, copy,
                          std::filesystem::copy_options::recursive |
                              std::filesystem::copy_options::overwrite_existing);
    CHECK(std::filesystem::remove(copy / "integrity.hmac"));
    const integrity::VerifyResult r =
        integrity::VerifyPackageManifest(copy.string(), key_path);
    CHECK(r.signed_manifest);
    CHECK(!r.signature_present);
    CHECK(!r.ok());
  }
}

// ---- REC-18: bulk payload coverage (big-frame channel) ---------------------

void TestPinnedSlotCoverage(const std::filesystem::path& root) {
  const uint32_t n = g_case.fetch_add(1);
  char chan[96];
  std::snprintf(chan, sizeof(chan), "/thyme_rec18_bf_%d_%u",
                static_cast<int>(getpid()), n);
  shm_unlink(chan);
  PinnedSlotWriter w;
  CHECK(w.Create(chan, 1000, 16, 0));
  for (uint64_t f = 0; f < 6; ++f) {
    PinnedSlotWriter::BeginView bv = w.Begin(1000);
    std::memset(bv.payload, static_cast<int>(0x20 + f), 1000);
    w.Commit(NowNs());
    SleepNs(500000);
  }
  const std::string out = (root / "pinned_slot").string();
  std::string pkg;
  {
    triggered::Config cfg;
    cfg.sources = {{triggered::SourceConfig::Kind::kPinnedSlot, chan, "cloud_1"}};
    cfg.out_dir = out;
    cfg.post_grace_ns = 5000000ULL;
    triggered::TriggeredRecorder rec;
    CHECK(rec.Init(cfg));
    CHECK(rec.Trigger(9, 60000000000ULL, 0));
    RunCapture(rec);
    pkg = rec.last_package_dir();
    CHECK(triggered::TriggeredRecorder::PackageComplete(pkg));
    rec.Shutdown();
  }
  w.Close();
  shm_unlink(chan);

  // The bulk payload and its index are both manifest-covered.
  const integrity::VerifyResult r = integrity::VerifyPackageManifest(pkg, "");
  CHECK(r.state == integrity::ManifestState::kOk);
  CHECK(r.ok());
  CHECK(!r.partial);
  const integrity::FileStatus* raw = FindFile(r, "frames/cloud_1.raw");
  const integrity::FileStatus* idx = FindFile(r, "frames/cloud_1.index.tbin");
  CHECK(raw != nullptr && idx != nullptr);
  CHECK(raw->manifest_size == 6128);  // REC-26/27 128 B header + 6×1000 B
  CHECK(raw->size_ok && raw->hash_ok);
  CHECK(idx->size_ok && idx->hash_ok);
  ExpectSameAsFullHashManifest(pkg, r.info, "");

  // A flipped byte in the bulk payload is detected by the package manifest.
  const std::filesystem::path copy = root / "pinned_slot_tampered";
  std::filesystem::copy(pkg, copy,
                        std::filesystem::copy_options::recursive |
                            std::filesystem::copy_options::overwrite_existing);
  FlipLastByte((copy / "frames/cloud_1.raw").string());
  const integrity::VerifyResult t =
      integrity::VerifyPackageManifest(copy.string(), "");
  CHECK(t.state == integrity::ManifestState::kOk);
  CHECK(!t.ok());
  CHECK(!t.intact());
  CHECK(t.files_mismatch == 1);
  const integrity::FileStatus* fr = FindFile(t, "frames/cloud_1.raw");
  CHECK(fr != nullptr && fr->present && !fr->hash_ok);
  std::ostringstream out_s;
  std::ostringstream err_s;
  CHECK(replaycli::RunCli({"integrity", copy.string()}, out_s, err_s,
                          nullptr) == replaycli::kExitFailure);
  CHECK(out_s.str().find("MISMATCH") != std::string::npos);
  CHECK(out_s.str().find("integrity: FAILED") != std::string::npos);
}

std::string CapturePinnedSlotMode(const std::filesystem::path& root, const std::string& name,
                                integrity::ManifestMode mode,
                                triggered::SourceConfig::ManifestPolicy policy =
                                    triggered::SourceConfig::ManifestPolicy::kDefault,
                                const std::string& key_path = {}) {
  const uint32_t n = g_case.fetch_add(1);
  char chan[96];
  std::snprintf(chan, sizeof(chan), "/thyme_rec23_bf_%d_%u", static_cast<int>(getpid()), n);
  shm_unlink(chan);
  PinnedSlotWriter writer;
  CHECK(writer.Create(chan, 1000, 16, 0));
  for (uint64_t frame = 0; frame < 4; ++frame) {
    PinnedSlotWriter::BeginView view = writer.Begin(1000);
    std::memset(view.payload, static_cast<int>(0x30 + frame), 1000);
    writer.Commit(NowNs());
    SleepNs(500000);
  }
  std::string package;
  {
    triggered::SourceConfig source{triggered::SourceConfig::Kind::kPinnedSlot, chan, "cloud_1"};
    source.manifest = policy;
    triggered::Config cfg;
    cfg.sources = {source};
    cfg.out_dir = (root / name).string();
    cfg.post_grace_ns = 5000000ULL;
    cfg.manifest_mode = mode;
    cfg.signing_key_path = key_path;
    triggered::TriggeredRecorder recorder;
    CHECK(recorder.Init(cfg));
    CHECK(recorder.integrity_mode() == (policy == triggered::SourceConfig::ManifestPolicy::kDefault
                                            ? mode
                                            : integrity::ManifestMode::kMixed));
    CHECK(recorder.Trigger(23, 60000000000ULL, 0));
    RunCapture(recorder);
    package = recorder.last_package_dir();
    CHECK(triggered::TriggeredRecorder::PackageComplete(package));
    recorder.Shutdown();
  }
  writer.Close();
  shm_unlink(chan);
  return package;
}

void ExpectModeCli(const std::string& package, const std::string& mode,
                   const std::string& key = {}) {
  std::ostringstream out;
  std::ostringstream err;
  std::vector<std::string> args{"integrity", package};
  if (!key.empty()) {
    args.emplace_back("--key");
    args.emplace_back(key);
  }
  CHECK(replaycli::RunCli(args, out, err, nullptr) == replaycli::kExitOk);
  CHECK(out.str().find("mode     : " + mode) != std::string::npos);
  CHECK(out.str().find("integrity: OK(" + mode + "); UNVERIFIED(payload)") != std::string::npos);
  CHECK(out.str().find("UNVERIFIED(payload)") != std::string::npos);
  CHECK(out.str().find("integrity: OK\n") == std::string::npos);

  args[0] = "verify";
  args.insert(args.begin() + 2, "--integrity");
  out.str("");
  out.clear();
  err.str("");
  err.clear();
  CHECK(replaycli::RunCli(args, out, err, nullptr) == replaycli::kExitOk);
  CHECK(out.str().find("verify  : OK") != std::string::npos);
  CHECK(out.str().find("integrity: OK(" + mode +
                       "); UNVERIFIED(payload)") != std::string::npos);
}

void TestMetadataAndOffModes(const std::filesystem::path& root) {
  const std::string metadata =
      CapturePinnedSlotMode(root, "mode_metadata", integrity::ManifestMode::kMetadata);
  integrity::VerifyResult r = integrity::VerifyPackageManifest(metadata, "");
  CHECK(r.ok());
  CHECK(r.info.manifest_mode == integrity::ManifestMode::kMetadata);
  CHECK(r.files_unverified == 1);
  const integrity::FileStatus* raw = FindFile(r, "frames/cloud_1.raw");
  const integrity::FileStatus* index = FindFile(r, "frames/cloud_1.index.tbin");
  CHECK(raw != nullptr && !raw->hash_checked && !raw->hash_ok);
  CHECK(index != nullptr && index->hash_checked && index->hash_ok);
  ExpectModeCli(metadata, "metadata");

  const std::filesystem::path metadata_flip = root / "mode_metadata_flip";
  std::filesystem::copy(metadata, metadata_flip, std::filesystem::copy_options::recursive);
  FlipLastByte((metadata_flip / "frames/cloud_1.raw").string());
  r = integrity::VerifyPackageManifest(metadata_flip.string(), "");
  CHECK(r.ok() && r.chain_ok && r.files_mismatch == 0);
  FlipLastByte((metadata_flip / "frames/cloud_1.index.tbin").string());
  r = integrity::VerifyPackageManifest(metadata_flip.string(), "");
  CHECK(!r.ok() && r.files_mismatch == 1);

  const std::filesystem::path metadata_resize = root / "mode_metadata_resize";
  std::filesystem::copy(metadata, metadata_resize, std::filesystem::copy_options::recursive);
  {
    std::ofstream raw_out(metadata_resize / "frames/cloud_1.raw", std::ios::binary | std::ios::app);
    raw_out.put('X');
  }
  r = integrity::VerifyPackageManifest(metadata_resize.string(), "");
  CHECK(!r.ok() && r.files_mismatch == 1);

  const std::string key_path = (root / "off.key").string();
  WriteText(key_path, "rec23-off-key\n");
  const std::string off =
      CapturePinnedSlotMode(root, "mode_off", integrity::ManifestMode::kOff,
                          triggered::SourceConfig::ManifestPolicy::kDefault, key_path);
  r = integrity::VerifyPackageManifest(off, key_path);
  CHECK(r.ok());
  CHECK(r.info.manifest_mode == integrity::ManifestMode::kOff);
  CHECK(r.files_unverified == r.files.size());
  CHECK(r.signed_manifest && r.signature_ok);
  ExpectModeCli(off, "off", key_path);

  const std::filesystem::path off_flip = root / "mode_off_flip";
  std::filesystem::copy(off, off_flip, std::filesystem::copy_options::recursive);
  FlipLastByte((off_flip / "frames/cloud_1.raw").string());
  FlipLastByte((off_flip / "meta.tbin").string());
  r = integrity::VerifyPackageManifest(off_flip.string(), key_path);
  CHECK(r.ok() && r.chain_ok && r.files_mismatch == 0);
  WriteText((off_flip / "injected.raw").string(), "extra");
  r = integrity::VerifyPackageManifest(off_flip.string(), key_path);
  CHECK(!r.ok() && r.files_unlisted == 1);
  const integrity::VerifyResult wrong_key =
      integrity::VerifyPackageManifest(off, root / "signing.key");
  CHECK(!wrong_key.ok() && !wrong_key.signature_ok);
}

void TestMixedMode(const std::filesystem::path& root) {
  const uint32_t n = g_case.fetch_add(1);
  char first[96];
  char second[96];
  std::snprintf(first, sizeof(first), "/thyme_rec23_mix_a_%d_%u", static_cast<int>(getpid()), n);
  std::snprintf(second, sizeof(second), "/thyme_rec23_mix_b_%d_%u", static_cast<int>(getpid()), n);
  PinnedSlotWriter writer_a;
  PinnedSlotWriter writer_b;
  CHECK(writer_a.Create(first, 256, 8, 0));
  CHECK(writer_b.Create(second, 256, 8, 0));
  for (uint64_t frame = 0; frame < 3; ++frame) {
    PinnedSlotWriter::BeginView a = writer_a.Begin(256);
    PinnedSlotWriter::BeginView b = writer_b.Begin(256);
    std::memset(a.payload, 0x41, 256);
    std::memset(b.payload, 0x42, 256);
    writer_a.Commit(NowNs());
    writer_b.Commit(NowNs());
    SleepNs(500000);
  }
  std::string package;
  {
    triggered::SourceConfig a{triggered::SourceConfig::Kind::kPinnedSlot, first, "full_raw"};
    triggered::SourceConfig b{triggered::SourceConfig::Kind::kPinnedSlot, second, "metadata_raw"};
    b.manifest = triggered::SourceConfig::ManifestPolicy::kMetadata;
    triggered::Config cfg;
    cfg.sources = {a, b};
    cfg.out_dir = (root / "mode_mixed").string();
    triggered::TriggeredRecorder recorder;
    CHECK(recorder.Init(cfg));
    CHECK(recorder.integrity_mode() == integrity::ManifestMode::kMixed);
    CHECK(recorder.Trigger(24, 60000000000ULL, 0));
    RunCapture(recorder);
    package = recorder.last_package_dir();
    recorder.Shutdown();
  }
  writer_a.Close();
  writer_b.Close();
  shm_unlink(first);
  shm_unlink(second);

  integrity::VerifyResult r = integrity::VerifyPackageManifest(package, "");
  CHECK(r.ok() && r.info.manifest_mode == integrity::ManifestMode::kMixed);
  CHECK(FindFile(r, "frames/full_raw.raw")->hash_checked);
  CHECK(!FindFile(r, "frames/metadata_raw.raw")->hash_checked);
  ExpectModeCli(package, "mixed");
  FlipLastByte(package + "/frames/metadata_raw.raw");
  r = integrity::VerifyPackageManifest(package, "");
  CHECK(r.ok());
  FlipLastByte(package + "/frames/full_raw.raw");
  r = integrity::VerifyPackageManifest(package, "");
  CHECK(!r.ok() && r.files_mismatch == 1);
}

// ---- REC-18: partial manifests on invalidated packages ---------------------

int g_seal_fault = 0;
int SealFaultInject(void*) { return g_seal_fault; }

// A seal-path failure (fault-injected wal fsync failure) invalidates the
// package; the .incomplete dir must carry a signed PARTIAL manifest over the
// surviving files.
void TestPartialSealFault(const std::filesystem::path& root) {
  const std::string key_path = (root / "partial.key").string();
  WriteText(key_path, "partial-manifest-key\n");

  const uint32_t n = g_case.fetch_add(1);
  char page[96];
  char ring[96];
  std::snprintf(page, sizeof(page), "/thyme_rec18_p_%d_%u",
                static_cast<int>(getpid()), n);
  std::snprintf(ring, sizeof(ring), "/thyme_rec18_r_%d_%u",
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
    for (uint64_t i = 0; i < 8; ++i) {
      const Pod16 p{i + 1, static_cast<uint32_t>((i + 1) * 2),
                    static_cast<uint32_t>((i + 1) * 3)};
      ring_client.Push(p);
      SleepNs(200000);
    }
    triggered::Config cfg;
    cfg.sources = {{triggered::SourceConfig::Kind::kMirrorRing, page, ""}};
    cfg.out_dir = (root / "partial").string();
    cfg.post_grace_ns = 5000000ULL;
    cfg.signing_key_path = key_path;
    cfg.fault_inject_seal = SealFaultInject;
    triggered::TriggeredRecorder rec;
    CHECK(rec.Init(cfg));
    g_seal_fault = 2;  // wal fsync fails after the seal row reached the cache
    CHECK(rec.Trigger(77, 50000000ULL, 10000000ULL));
    RunCapture(rec);
    g_seal_fault = 0;
    dir = rec.last_package_dir();
    CHECK(!triggered::TriggeredRecorder::PackageComplete(dir));
    CHECK(rec.disk_full());
    rec.Shutdown();
    ring_client.Close();
  }
  mirror.Shutdown(true);
  shm_unlink(page);
  shm_unlink(ring);

  const std::string inc = dir + ".incomplete";
  CHECK(std::filesystem::exists(inc));
  CHECK(std::filesystem::exists(inc + "/integrity.manifest"));
  CHECK(std::filesystem::exists(inc + "/integrity.hmac"));  // signed too

  const integrity::VerifyResult r =
      integrity::VerifyPackageManifest(inc, key_path);
  CHECK(r.state == integrity::ManifestState::kOk);
  CHECK(r.partial);
  CHECK(!r.ok());    // not a sealed package
  CHECK(r.intact());  // but every surviving file verifies
  CHECK(r.chain_ok);
  CHECK(r.signed_manifest && r.signature_checked && r.signature_ok);
  CHECK(r.files_missing == 0 && r.files_mismatch == 0 &&
        r.files_unlisted == 0);
  CHECK(r.info.kind == integrity::PackageKind::kTrigger);
  CHECK(r.info.trigger_id == 77);
  CHECK(r.info.t0_ns != 0);
  CHECK(FindFile(r, "wal.log") != nullptr);  // abort row covered
  integrity::PackageInfo partial_info = r.info;
  partial_info.partial = r.partial;
  ExpectSameAsFullHashManifest(inc, partial_info, key_path);

  // CLI: PARTIAL is reported distinctly from OK and FAILED, exit non-zero.
  {
    std::ostringstream out_s;
    std::ostringstream err_s;
    CHECK(replaycli::RunCli({"integrity", inc, "--key", key_path}, out_s,
                            err_s, nullptr) == replaycli::kExitFailure);
    CHECK(out_s.str().find("partial  : yes") != std::string::npos);
    CHECK(out_s.str().find("integrity: PARTIAL") != std::string::npos);
  }
  {
    std::ostringstream out_s;
    std::ostringstream err_s;
    CHECK(replaycli::RunCli({"integrity", inc, "--key", key_path, "--json"},
                            out_s, err_s, nullptr) == replaycli::kExitFailure);
    CHECK(out_s.str().find("\"partial\":true") != std::string::npos);
    CHECK(out_s.str().find("\"intact\":true") != std::string::npos);
  }

  // A tampered survivor flips the partial package to FAILED.
  FlipLastByte(inc + "/small.tbin");
  const integrity::VerifyResult t =
      integrity::VerifyPackageManifest(inc, key_path);
  CHECK(t.partial);
  CHECK(!t.intact());
  CHECK(t.files_mismatch == 1);
  {
    std::ostringstream out_s;
    std::ostringstream err_s;
    CHECK(replaycli::RunCli({"integrity", inc, "--key", key_path}, out_s,
                            err_s, nullptr) == replaycli::kExitFailure);
    CHECK(out_s.str().find("integrity: FAILED") != std::string::npos);
  }
}

void TestContinuousIncrementalManifest(const std::filesystem::path& root) {
  const uint32_t n = g_case.fetch_add(1);
  char page[96];
  char ring[96];
  std::snprintf(page, sizeof(page), "/thyme_rec22_cp_%d_%u",
                static_cast<int>(getpid()), n);
  std::snprintf(ring, sizeof(ring), "/thyme_rec22_cr_%d_%u",
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
    triggered::Config cfg;
    cfg.sources = {{triggered::SourceConfig::Kind::kMirrorRing, page, ""}};
    cfg.out_dir = (root / "continuous_incremental").string();
    triggered::TriggeredRecorder rec;
    CHECK(rec.Init(cfg));
    triggered::ContinuousConfig cont;
    cont.slice_bytes = 1024 * 1024;
#if defined(__linux__)
    g_read_bytes.store(0);
    g_count_reads.store(true);
#endif
    CHECK(rec.ArmContinuous(cont));
    for (uint64_t i = 0; i < 16; ++i) {
      const Pod16 p{i + 1, static_cast<uint32_t>(i * 2),
                    static_cast<uint32_t>(i * 3)};
      ring_client.Push(p);
      rec.Tick();
      SleepNs(100000);
    }
    for (uint32_t i = 0; i < 4; ++i) rec.Tick();
    SleepNs(1100000000ULL);
    rec.Tick();  // Periodic anchor between slice creation and seal.
    rec.DisarmContinuous();
#if defined(__linux__)
    g_count_reads.store(false);
    CHECK(g_read_bytes.load() == 0);
#endif
    dir = rec.current_slice_dir();
    CHECK(triggered::TriggeredRecorder::PackageComplete(dir));
    CHECK(rec.continuous_stats().slices_done == 1);
    rec.Shutdown();
    ring_client.Close();
  }
  mirror.Shutdown(true);
  shm_unlink(page);
  shm_unlink(ring);

  const integrity::VerifyResult r = integrity::VerifyPackageManifest(dir, "");
  CHECK(r.state == integrity::ManifestState::kOk);
  CHECK(r.ok());
  CHECK(r.info.kind == integrity::PackageKind::kContinuous);
  CHECK(FindFile(r, "small.tbin") != nullptr);
  std::vector<triggered::ClockAnchor> anchors;
  CHECK(triggered::ReadPackageClockAnchors(dir, &anchors));
  CHECK(anchors.size() >= 3);  // Initial + periodic + seal anchors.
  ExpectSameAsFullHashManifest(dir, r.info, "");
}

// ---- REC-18: startup recovery sweep manifests crash leftovers --------------

void TestRecoverySweepManifest(const std::filesystem::path& root) {
  const std::string out = (root / "sweep").string();
  std::filesystem::create_directories(out);
  // A crash-leftover package: wal header (trigger id 42) + a payload file,
  // no seal row — exactly what a recorder crash mid-capture leaves behind.
  const std::string pkg = out + "/42_123456";
  std::filesystem::create_directories(pkg);
  WriteText(pkg + "/data.bin", "surviving payload");
  {
    FILE* f = std::fopen((pkg + "/wal.log").c_str(), "wb");
    CHECK(f != nullptr);
    triggered::WalHeader wh{};
    std::memcpy(wh.magic, triggered::kWalMagic, sizeof(triggered::kWalMagic));
    wh.trigger_id = 42;
    CHECK(std::fwrite(&wh, sizeof(wh), 1, f) == 1);
    CHECK(std::fclose(f) == 0);
  }

  retention::Manager rm;
  retention::Config rcfg;
  rcfg.out_dir = out;
  CHECK(rm.Init(rcfg));
  CHECK(rm.MarkIncomplete() == 1);
  const std::string inc = pkg + ".incomplete";
  CHECK(std::filesystem::exists(inc));
  CHECK(std::filesystem::exists(inc + "/integrity.manifest"));
  CHECK(rm.MarkIncomplete() == 0);  // idempotent: nothing new to mark

  const integrity::VerifyResult r = integrity::VerifyPackageManifest(inc, "");
  CHECK(r.state == integrity::ManifestState::kOk);
  CHECK(r.partial);
  CHECK(r.intact());
  CHECK(!r.ok());
  CHECK(r.info.kind == integrity::PackageKind::kTrigger);
  CHECK(r.info.trigger_id == 42);   // recovered from the wal header
  CHECK(r.info.session_id == 42);
  CHECK(FindFile(r, "data.bin") != nullptr);
  CHECK(FindFile(r, "wal.log") != nullptr);
  {
    std::ostringstream out_s;
    std::ostringstream err_s;
    CHECK(replaycli::RunCli({"integrity", inc}, out_s, err_s, nullptr) ==
          replaycli::kExitFailure);
    CHECK(out_s.str().find("integrity: PARTIAL") != std::string::npos);
  }
}

void TestRecoveryMetadataMode(const std::filesystem::path& root) {
  const std::string out = (root / "sweep_metadata").string();
  const std::string package = out + "/43_123456";
  std::filesystem::create_directories(package + "/frames");
  WriteText(package + "/frames/cloud.raw", "surviving raw payload");
  WriteText(package + "/wal.log", "unsealed wal");

  retention::Manager manager;
  retention::Config cfg;
  cfg.out_dir = out;
  cfg.manifest_policy.manifest_mode = integrity::ManifestMode::kMetadata;
  cfg.manifest_policy.bulk_modes = {{"cloud", integrity::ManifestMode::kMetadata}};
  CHECK(manager.Init(cfg));
  CHECK(manager.MarkIncomplete() == 1);
  const std::string incomplete = package + ".incomplete";
  integrity::VerifyResult r = integrity::VerifyPackageManifest(incomplete, "");
  CHECK(r.partial && r.intact() && !r.ok());
  CHECK(r.info.manifest_mode == integrity::ManifestMode::kMetadata);
  CHECK(!FindFile(r, "frames/cloud.raw")->hash_checked);
  CHECK(FindFile(r, "wal.log")->hash_checked);
  FlipLastByte(incomplete + "/frames/cloud.raw");
  r = integrity::VerifyPackageManifest(incomplete, "");
  CHECK(r.partial && r.intact() && !r.ok());
  std::ostringstream out_s;
  std::ostringstream err_s;
  CHECK(replaycli::RunCli({"integrity", incomplete}, out_s, err_s, nullptr) ==
        replaycli::kExitFailure);
  CHECK(out_s.str().find("integrity: PARTIAL") != std::string::npos);
  CHECK(out_s.str().find("UNVERIFIED(payload)") != std::string::npos);
}

}  // namespace

int main() {
  const std::filesystem::path root =
      std::filesystem::path("/tmp") /
      ("thyme_sec03_" + std::to_string(static_cast<int>(getpid())));
  std::filesystem::remove_all(root);
  std::filesystem::create_directories(root);

  TestSealVerifyAndTamper(root);
  TestIncrementalWriteContract(root);
  TestHashCheckpoint(root);
  TestCheckpointAfterSigkill(root);
  TestClockAnchors(root);
  TestSigning(root);
  TestPinnedSlotCoverage(root);
  TestMetadataAndOffModes(root);
  TestMixedMode(root);
  TestPartialSealFault(root);
  TestContinuousIncrementalManifest(root);
  TestRecoverySweepManifest(root);
  TestRecoveryMetadataMode(root);

  // ARCH-14: the recorder leaves its session-lease page behind on purpose.
  thyme_test::UnlinkSessionLeasesUnder(root);
  std::filesystem::remove_all(root);
  std::printf("test_package_integrity: OK\n");
  return 0;
}
