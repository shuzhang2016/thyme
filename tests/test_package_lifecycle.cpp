#include "thyme/check.h"
#include "thyme/record/package_integrity.h"
#include "thyme/record/record_mirror_ring_client.h"
#include "thyme/record/retention.h"

#include "session_lease_cleanup.h"
#include "thyme/record/triggered_recorder.h"
#include "thyme/runtime/record_mirror_ring.h"
#include "thyme/runtime/shm_region.h"

#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

using namespace thyme;

namespace {

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

std::string PageName(const char* tag) {
  return std::string("/thyme_arch16_") + tag + "_" +
         std::to_string(static_cast<int>(getpid()));
}

void CreatePage(const std::string& name, record_mirror::RingClient* mirror) {
  ShmRegion::UnlinkName(name.c_str());
  ThymeStructDesc desc{};
  desc.struct_id = 0;
  desc.contract_version = 1;
  desc.payload_size = sizeof(uint64_t);
  std::snprintf(desc.name, sizeof(desc.name), "Pod8");
  const RecordMirrorRingSpec spec{sizeof(uint64_t), 64};
  CHECK(mirror->Create(name.c_str(), &desc, &spec, 1, 7));
}

void Publish(record_mirror::RingClient* mirror, uint64_t count) {
  for (uint64_t i = 0; i < count; ++i) {
    CHECK(mirror->Page()->TryWriteRing(0, &i, sizeof(i), NowNs(),
                                      static_cast<uint32_t>(i)));
  }
}

void RunCapture(triggered::TriggeredRecorder* recorder) {
  const uint64_t deadline = NowNs() + 10000000000ULL;
  while (recorder->phase() != triggered::Phase::kArmed) {
    recorder->Tick();
    CHECK(NowNs() < deadline);
    SleepNs(100000);
  }
}

triggered::PackageLifecycleState ExpectState(
    const std::string& package, triggered::PackageLifecycle expected,
    triggered::PackageAbortReason reason = triggered::PackageAbortReason::kNone) {
  triggered::PackageLifecycleState state;
  CHECK(triggered::ReadPackageLifecycle(package, &state));
  CHECK(state.state == expected);
  CHECK(state.abort_reason == reason);
  CHECK(state.session_id != 0);
  CHECK(state.mono_ns != 0);
  CHECK(state.realtime_ns != 0);
  return state;
}

std::vector<triggered::PackageLifecycle> LifecycleRows(
    const std::string& package) {
  std::vector<triggered::PackageLifecycle> states;
  std::FILE* file = std::fopen((package + "/wal.log").c_str(), "rb");
  CHECK(file != nullptr);
  triggered::WalHeader header{};
  CHECK(std::fread(&header, sizeof(header), 1, file) == 1);
  triggered::WalRow row{};
  while (std::fread(&row, sizeof(row), 1, file) == 1) {
    if (row.op == triggered::kWalOpLifecycle) {
      CHECK(row.len == 0);
      states.push_back(static_cast<triggered::PackageLifecycle>(row.struct_id));
    }
  }
  CHECK(std::fclose(file) == 0);
  return states;
}

void TestNormalTriggerAndContinuous(const std::filesystem::path& root) {
  const std::string page = PageName("normal");
  record_mirror::RingClient mirror;
  CreatePage(page, &mirror);
  Publish(&mirror, 12);
  triggered::Config cfg;
  cfg.sources = {{triggered::SourceConfig::Kind::kMirrorRing, page, ""}};
  cfg.out_dir = (root / "normal").string();
  cfg.post_grace_ns = 1000000;
  triggered::TriggeredRecorder recorder;
  CHECK(recorder.Init(cfg));
  CHECK(recorder.Trigger(11, 60000000000ULL, 0));
  const std::string trigger_package = recorder.last_package_dir();
  ExpectState(trigger_package, triggered::PackageLifecycle::kCapturing);
  RunCapture(&recorder);
  ExpectState(trigger_package, triggered::PackageLifecycle::kSealed);
  CHECK(triggered::TriggeredRecorder::PackageComplete(trigger_package));
  CHECK(integrity::VerifyPackageManifest(trigger_package, "").ok());
  const std::vector<triggered::PackageLifecycle> expected{
      triggered::PackageLifecycle::kArmed,
      triggered::PackageLifecycle::kCapturing,
      triggered::PackageLifecycle::kSealing,
      triggered::PackageLifecycle::kSealed};
  CHECK(LifecycleRows(trigger_package) == expected);

  triggered::ContinuousConfig continuous;
  continuous.slice_bytes = 1024 * 1024;
  CHECK(recorder.ArmContinuous(continuous));
  Publish(&mirror, 4);
  recorder.Tick();
  const std::string slice = recorder.current_slice_dir();
  ExpectState(slice, triggered::PackageLifecycle::kCapturing);
  recorder.DisarmContinuous();
  ExpectState(slice, triggered::PackageLifecycle::kSealed);
  CHECK(triggered::TriggeredRecorder::PackageComplete(slice));
  CHECK(integrity::VerifyPackageManifest(slice, "").ok());
  CHECK(LifecycleRows(slice) == expected);
  mirror.Shutdown(true);
}

bool g_write_fault = false;
bool WriteFault(void*) { return g_write_fault; }
int g_seal_fault = 0;
int SealFault(void*) { return g_seal_fault; }

void TestFaultReasons(const std::filesystem::path& root) {
  const std::string page = PageName("fault");
  record_mirror::RingClient mirror;
  CreatePage(page, &mirror);
  Publish(&mirror, 8);
  triggered::Config cfg;
  cfg.sources = {{triggered::SourceConfig::Kind::kMirrorRing, page, ""}};
  cfg.out_dir = (root / "fault").string();
  cfg.post_grace_ns = 1000000;
  cfg.fault_inject_write = WriteFault;
  cfg.fault_inject_seal = SealFault;
  triggered::TriggeredRecorder recorder;
  CHECK(recorder.Init(cfg));

  g_write_fault = true;
  CHECK(recorder.Trigger(21, 60000000000ULL, 0));
  RunCapture(&recorder);
  g_write_fault = false;
  std::string package = recorder.last_package_dir() + ".incomplete";
  ExpectState(package, triggered::PackageLifecycle::kAborted,
              triggered::PackageAbortReason::kWriteFailure);
  CHECK(!triggered::TriggeredRecorder::PackageComplete(package));
  const integrity::VerifyResult write_manifest =
      integrity::VerifyPackageManifest(package, "");
  CHECK(write_manifest.partial && write_manifest.intact());

  recorder.ClearDiskFull();
  g_seal_fault = 2;
  CHECK(recorder.Trigger(22, 60000000000ULL, 0));
  RunCapture(&recorder);
  g_seal_fault = 0;
  package = recorder.last_package_dir() + ".incomplete";
  ExpectState(package, triggered::PackageLifecycle::kAborted,
              triggered::PackageAbortReason::kSealFailure);
  CHECK(!triggered::TriggeredRecorder::PackageComplete(package));
  const integrity::VerifyResult seal_manifest =
      integrity::VerifyPackageManifest(package, "");
  CHECK(seal_manifest.partial && seal_manifest.intact());
  mirror.Shutdown(true);
}

std::vector<std::string> WaitForStates(
    const std::filesystem::path& out, std::size_t count,
    triggered::PackageLifecycle expected) {
  std::vector<std::string> packages;
  const uint64_t deadline = NowNs() + 10000000000ULL;
  while (NowNs() < deadline) {
    packages.clear();
    if (std::filesystem::exists(out)) {
      for (const auto& entry : std::filesystem::directory_iterator(out)) {
        if (!entry.is_directory()) continue;
        triggered::PackageLifecycleState state;
        if (triggered::ReadPackageLifecycle(entry.path().string(), &state) &&
            state.state == expected) {
          packages.push_back(entry.path().string());
        }
      }
    }
    if (packages.size() == count) return packages;
    SleepNs(1000000);
  }
  CHECK(false);
  return packages;
}

void KillChild(pid_t child) {
  CHECK(kill(child, SIGKILL) == 0);
  int status = 0;
  CHECK(waitpid(child, &status, 0) == child);
  CHECK(WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL);
}

void CheckCrashRecovery(const std::filesystem::path& out,
                        const std::vector<std::string>& packages) {
  retention::Manager manager;
  retention::Config cfg;
  cfg.out_dir = out.string();
  CHECK(manager.Init(cfg));
  CHECK(manager.MarkIncomplete() == packages.size());
  for (const std::string& package : packages) {
    const std::string incomplete = package + ".incomplete";
    const auto before = ExpectState(incomplete, triggered::PackageLifecycle::kAborted,
                                    triggered::PackageAbortReason::kCrashRecovery);
    const auto after = ExpectState(incomplete, triggered::PackageLifecycle::kAborted,
                                   triggered::PackageAbortReason::kCrashRecovery);
    CHECK(before.mono_ns == after.mono_ns);
    CHECK(before.realtime_ns == after.realtime_ns);
    const integrity::VerifyResult manifest =
        integrity::VerifyPackageManifest(incomplete, "");
    CHECK(manifest.partial && manifest.intact());
  }
  CHECK(manager.MarkIncomplete() == 0);
}

void TestSigkillCapturing(const std::filesystem::path& root) {
  const std::filesystem::path out = root / "crash_capturing";
  const std::string page = PageName("crash_capturing");
  std::filesystem::create_directories(out);
  const pid_t child = fork();
  CHECK(child >= 0);
  if (child == 0) {
    record_mirror::RingClient mirror;
    CreatePage(page, &mirror);
    Publish(&mirror, 8);
    triggered::Config cfg;
    cfg.sources = {{triggered::SourceConfig::Kind::kMirrorRing, page, ""}};
    cfg.out_dir = out.string();
    triggered::TriggeredRecorder recorder;
    CHECK(recorder.Init(cfg));
    triggered::ContinuousConfig continuous;
    continuous.slice_bytes = 1024 * 1024;
    CHECK(recorder.ArmContinuous(continuous));
    CHECK(recorder.Trigger(31, 1000000, 60000000000ULL));
    for (;;) {
      recorder.Tick();
      SleepNs(1000000);
    }
  }
  const auto packages =
      WaitForStates(out, 2, triggered::PackageLifecycle::kCapturing);
  KillChild(child);
  ShmRegion::UnlinkName(page.c_str());
  CheckCrashRecovery(out, packages);
}

void SealingHook(void*) { SleepNs(10000000000ULL); }

void TestSigkillSealing(const std::filesystem::path& root) {
  const std::filesystem::path out = root / "crash_sealing";
  const std::string page = PageName("crash_sealing");
  std::filesystem::create_directories(out);
  const pid_t child = fork();
  CHECK(child >= 0);
  if (child == 0) {
    record_mirror::RingClient mirror;
    CreatePage(page, &mirror);
    Publish(&mirror, 8);
    triggered::Config cfg;
    cfg.sources = {{triggered::SourceConfig::Kind::kMirrorRing, page, ""}};
    cfg.out_dir = out.string();
    cfg.post_grace_ns = 1000000;
    cfg.test_hook_after_sealing = SealingHook;
    triggered::TriggeredRecorder recorder;
    CHECK(recorder.Init(cfg));
    CHECK(recorder.Trigger(32, 60000000000ULL, 0));
    for (;;) {
      recorder.Tick();
      SleepNs(100000);
    }
  }
  const auto packages =
      WaitForStates(out, 1, triggered::PackageLifecycle::kSealing);
  KillChild(child);
  ShmRegion::UnlinkName(page.c_str());
  CheckCrashRecovery(out, packages);
}

void WriteLegacyWal(const std::filesystem::path& package, bool sealed) {
  std::filesystem::create_directories(package);
  std::FILE* file = std::fopen((package / "wal.log").c_str(), "wb");
  CHECK(file != nullptr);
  const triggered::WalHeader header{{'T', 'R', 'I', 'G', 'W', 'A', 'L', '1'}, 41};
  CHECK(std::fwrite(&header, sizeof(header), 1, file) == 1);
  if (sealed) {
    const triggered::WalRow seal{triggered::kWalOpSeal, 0, 0, 0, 0, 0};
    CHECK(std::fwrite(&seal, sizeof(seal), 1, file) == 1);
  }
  CHECK(std::fclose(file) == 0);
}

void TestLegacyCompatibility(const std::filesystem::path& root) {
  const std::filesystem::path out = root / "legacy";
  const std::filesystem::path sealed = out / "41_1";
  const std::filesystem::path unsealed = out / "41_2";
  const std::filesystem::path torn_seal = out / "41_3";
  WriteLegacyWal(sealed, true);
  WriteLegacyWal(unsealed, false);
  WriteLegacyWal(torn_seal, false);
  {
    std::FILE* file = std::fopen((torn_seal / "wal.log").c_str(), "ab");
    CHECK(file != nullptr);
    const triggered::PackageLifecycle states[] = {
        triggered::PackageLifecycle::kArmed,
        triggered::PackageLifecycle::kCapturing,
        triggered::PackageLifecycle::kSealing,
        triggered::PackageLifecycle::kSealed};
    for (const auto state : states) {
      const triggered::WalRow row{triggered::kWalOpLifecycle, 0,
                                  static_cast<uint16_t>(state), 0, 1, 1};
      CHECK(std::fwrite(&row, sizeof(row), 1, file) == 1);
    }
    CHECK(std::fclose(file) == 0);
  }
  triggered::PackageLifecycleState state;
  CHECK(triggered::ReadPackageLifecycle(sealed.string(), &state));
  CHECK(state.legacy && state.state == triggered::PackageLifecycle::kSealed);
  CHECK(triggered::TriggeredRecorder::PackageComplete(sealed.string()));
  CHECK(triggered::ReadPackageLifecycle(unsealed.string(), &state));
  CHECK(state.legacy && state.state == triggered::PackageLifecycle::kCapturing);
  CHECK(state.mono_ns == 0 && state.realtime_ns == 0);
  ExpectState(torn_seal.string(), triggered::PackageLifecycle::kSealing);
  CHECK(!triggered::TriggeredRecorder::PackageComplete(torn_seal.string()));
  retention::Manager manager;
  retention::Config cfg;
  cfg.out_dir = out.string();
  CHECK(manager.Init(cfg));
  CHECK(manager.MarkIncomplete() == 2);
  ExpectState(unsealed.string() + ".incomplete",
              triggered::PackageLifecycle::kAborted,
              triggered::PackageAbortReason::kCrashRecovery);
  ExpectState(torn_seal.string() + ".incomplete",
              triggered::PackageLifecycle::kAborted,
              triggered::PackageAbortReason::kCrashRecovery);
  CHECK(triggered::TriggeredRecorder::PackageComplete(sealed.string()));
}

}  // namespace

int main() {
  const std::filesystem::path root =
      std::filesystem::path("/tmp") /
      ("thyme_arch16_" + std::to_string(static_cast<int>(getpid())));
  std::filesystem::remove_all(root);
  std::filesystem::create_directories(root);
  TestNormalTriggerAndContinuous(root);
  TestFaultReasons(root);
  TestSigkillCapturing(root);
  TestSigkillSealing(root);
  TestLegacyCompatibility(root);
  // ARCH-14: the recorder leaves its session-lease page behind on purpose.
  thyme_test::UnlinkSessionLeasesUnder(root);
  std::filesystem::remove_all(root);
  std::printf("test_package_lifecycle: OK\n");
  return 0;
}
