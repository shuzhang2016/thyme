// test_retention.cpp — RetentionManager (design 6.4/6.5) plus the engine-side
// ENOSPC path (fault-injected write failure -> unsealed package -> disk_full
// -> retention marks it incomplete -> ClearDiskFull -> retry succeeds), and the Center
// disk_full -> kQuotaExceeded linkage for non-crash triggers.
#include "thyme/check.h"
#include "thyme/record/record_mirror_ring_client.h"
#include "thyme/record/retention.h"
#include "thyme/record/thyme_bin.h"
#include "thyme/record/trigger_center.h"
#include "thyme/record/triggered_recorder.h"
#include "thyme/runtime/record_mirror_ring.h"
#include "thyme/runtime/shm_region.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <string>
#include <sys/stat.h>
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

bool PathExists(const std::string& p) {
  struct stat st {};
  return stat(p.c_str(), &st) == 0;
}

void Mkdir(const std::string& p) { CHECK(mkdir(p.c_str(), 0700) == 0); }

void MakeWal(const std::string& dir, const std::vector<uint8_t>& ops,
             bool bad_magic = false, bool short_header = false,
             std::size_t trailing_bytes = 0) {
  Mkdir(dir);
  FILE* f = std::fopen((dir + "/wal.log").c_str(), "wb");
  CHECK(f != nullptr);
  triggered::WalHeader wh{{'T', 'R', 'I', 'G', 'W', 'A', 'L', '1'}, 1};
  if (bad_magic) wh.magic[0] = 'X';
  const std::size_t header_bytes = short_header ? 3 : sizeof(wh);
  CHECK(std::fwrite(&wh, 1, header_bytes, f) == header_bytes);
  for (const uint8_t op : ops) {
    const triggered::WalRow row{op, 0, 0, 0, 0, 0};
    CHECK(std::fwrite(&row, sizeof(row), 1, f) == 1);
  }
  for (std::size_t i = 0; i < trailing_bytes; ++i) {
    CHECK(std::fputc(static_cast<int>(i & 0xFF), f) != EOF);
  }
  CHECK(std::fclose(f) == 0);
}

// Fake package: dir + data.bin(payload_bytes) + wal.log (sealed or not, using
// the package-ABI wal structs from triggered_recorder.h). Directory mtime is
// pinned AFTER file creation (file creation bumps the dir mtime).
void MakePkg(const std::string& dir, uint64_t payload_bytes, int64_t mtime_ns,
             bool sealed) {
  Mkdir(dir);
  FILE* f = std::fopen((dir + "/data.bin").c_str(), "wb");
  CHECK(f != nullptr);
  std::vector<uint8_t> buf(payload_bytes, 0xAB);
  if (payload_bytes > 0) CHECK(std::fwrite(buf.data(), 1, payload_bytes, f) ==
                               payload_bytes);
  CHECK(std::fclose(f) == 0);
  f = std::fopen((dir + "/wal.log").c_str(), "wb");
  CHECK(f != nullptr);
  const triggered::WalHeader wh{{'T', 'R', 'I', 'G', 'W', 'A', 'L', '1'}, 1};
  CHECK(std::fwrite(&wh, sizeof(wh), 1, f) == 1);
  if (sealed) {
    const triggered::WalRow seal{triggered::kWalOpSeal, 0, 0, 0, 0, 0};
    CHECK(std::fwrite(&seal, sizeof(seal), 1, f) == 1);
  }
  CHECK(std::fclose(f) == 0);
  struct timespec ts[2];
  ts[0].tv_sec = static_cast<time_t>(mtime_ns / 1000000000LL);
  ts[0].tv_nsec = static_cast<long>(mtime_ns % 1000000000LL);
  ts[1] = ts[0];
  CHECK(utimensat(AT_FDCWD, dir.c_str(), ts, 0) == 0);
}

// ---- Center disk_full linkage (fake recorder hooks) ----
bool g_disk_full = false;
bool g_active = false;
bool HookReady(void*) { return true; }
bool HookActive(void*) { return g_active; }
bool HookStart(void*, uint64_t, uint64_t, uint64_t, uint64_t, bool, bool,
               const std::vector<std::string>&) {
  g_active = true;
  return true;
}
bool HookPreempt(void*) {
  g_active = false;
  return true;
}
bool HookDiskFull(void*) { return g_disk_full; }

// ---- engine ENOSPC fault injection ----
bool g_fault = false;
bool FaultInject(void*) { return g_fault; }

// Seal-path fault injection: 0=ok, 1=seal fwrite fails, 2=wal fsync fails
// after the seal row reached the page cache, 3=data-file fsync fails.
int g_seal_fault = 0;
int SealFaultInject(void*) { return g_seal_fault; }

bool WalHasRow(const std::string& dir, uint8_t op) {
  FILE* f = std::fopen((dir + "/wal.log").c_str(), "rb");
  if (f == nullptr) return false;
  triggered::WalHeader wh{};
  if (std::fread(&wh, sizeof(wh), 1, f) != 1) {
    std::fclose(f);
    return false;
  }
  bool found = false;
  triggered::WalRow row{};
  while (std::fread(&row, sizeof(row), 1, f) == 1) {
    if (row.op == op) found = true;
  }
  std::fclose(f);
  return found;
}
bool WalHasAbortRow(const std::string& dir) {
  return WalHasRow(dir, triggered::kWalOpAbort);
}

void RunUntilArmed(triggered::TriggeredRecorder& rec) {
  const uint64_t deadline = NowNs() + 10000000000ULL;
  while (rec.phase() != triggered::Phase::kArmed) {
    rec.Tick();
    CHECK(NowNs() < deadline);
    SleepNs(100000);
  }
}

}  // namespace

int main() {
  const int me = static_cast<int>(getpid());
  char root[96];
  std::snprintf(root, sizeof(root), "/tmp/c9ret_%d", me);
  const std::string out = root;

  // ---- startup recovery: unsealed packages are marked .incomplete ----
  Mkdir(out);
  MakePkg(out + "/1_100", 1000, 100000000000LL, true);   // sealed, keep
  MakePkg(out + "/2_200", 1000, 200000000000LL, false);  // crashed mid-drain
  Mkdir(out + "/high");
  MakePkg(out + "/high/3_300", 1000, 300000000000LL, false);  // in high zone
  {
    retention::Manager rm;
    retention::Config rcfg;
    rcfg.out_dir = out;
    CHECK(rm.Init(rcfg));
    CHECK(rm.MarkIncomplete() == 2);
    CHECK(PathExists(out + "/1_100"));
    CHECK(!PathExists(out + "/2_200"));
    CHECK(PathExists(out + "/2_200.incomplete"));
    CHECK(PathExists(out + "/high/3_300.incomplete"));
    CHECK(PathExists(out + "/high"));  // the zone dir itself is not a package
    // Idempotent: a second scan finds nothing new.
    CHECK(rm.MarkIncomplete() == 0);
    // Sealed package passes the P2 completeness check.
    CHECK(triggered::TriggeredRecorder::PackageComplete(out + "/1_100"));
    CHECK(!triggered::TriggeredRecorder::PackageComplete(out +
                                                         "/2_200.incomplete"));
  }

  // ---- quota: oldest low-priority packages deleted; high/ never touched ----
  // Layout now: low = 1_100 (sealed, ~1040B), 2_200.incomplete (~1239B —
  // partial manifest included); high = 3_300.incomplete (~1239B).
  // Total ~3518B.
  {
    retention::Manager rm;
    retention::Config rcfg;
    rcfg.out_dir = out;
    // Total ~3518B; quota 2500B -> delete oldest low (1_100), then total
    // ~2478 <= 2500: stop. high/ untouched although it is the oldest data.
    rcfg.disk_quota_bytes = 2500;
    CHECK(rm.Init(rcfg));
    const uint64_t freed = rm.Enforce({});
    CHECK(freed >= 1000);
    CHECK(!PathExists(out + "/1_100"));              // oldest low deleted
    CHECK(PathExists(out + "/2_200.incomplete"));    // next-oldest survives
    CHECK(PathExists(out + "/high/3_300.incomplete"));  // high/ must not be touched
    const uint64_t freed2 = rm.Enforce({});  // already under quota
    CHECK(freed2 == 0);
  }

  // ---- high/ self-cleanup: over reserved -> oldest crash package deleted ----
  // .incomplete files are handled exclusively by the policy (kept by default), so quota sweeps no longer delete them;
  // this section verifies high/ reserved-zone sweeping with sealed packages, and the policy matrix is in test_retention_symlink.
  // MarkIncomplete already wrote a partial manifest for 3_300.incomplete (~223B),
  // so the high-zone total is ~1239 + 1040 + 1040 ≈ 3319B.
  MakePkg(out + "/high/4_400", 1000, 400000000000LL, true);
  MakePkg(out + "/high/4_401", 1000, 400000000001LL, true);
  {
    retention::Manager rm;
    retention::Config rcfg;
    rcfg.out_dir = out;
    // high = 3_300.incomplete (~1239B, manifest included) + 4_400 + 4_401
    // (~3319B) > 2400 -> delete the oldest SEALED crash package (4_400);
    // 2279 <= 2400 stops the sweep.
    rcfg.high_reserved_bytes = 2400;
    CHECK(rm.Init(rcfg));
    CHECK(rm.Enforce({}) >= 1000);
    CHECK(PathExists(out + "/high/3_300.incomplete"));  // policy keeps the incomplete package
    CHECK(!PathExists(out + "/high/4_400"));  // oldest sealed crash gone
    CHECK(PathExists(out + "/high/4_401"));
    CHECK(PathExists(out + "/2_200.incomplete"));  // low zone untouched
  }

  // ---- exclusion: quota pressure must skip the in-flight package ----
  // Layout from the earlier cases: 2_200.incomplete (low), high/ has
  // 3_300.incomplete + 4_401. Clear the incomplete packages via the delete_immediately
  // policy, then add three sealed low packages; quota forces deleting two of
  // the three, but the second-oldest is "active" -> the sweep must skip it
  // and delete the oldest + the newest instead.
  MakePkg(out + "/5_500", 1000, 500000000000LL, true);   // oldest
  MakePkg(out + "/6_600", 1000, 600000000000LL, true);   // active (excluded)
  MakePkg(out + "/7_700", 1000, 700000000000LL, true);   // newest
  {
    retention::Manager rm;
    retention::Config rcfg;
    rcfg.out_dir = out;
    rcfg.incomplete_policy = retention::IncompletePolicy::kDeleteImmediately;
    CHECK(rm.Init(rcfg));
    CHECK(rm.Enforce({}) >= 2000);  // both .incomplete files cleared by policy
    CHECK(!PathExists(out + "/2_200.incomplete"));
    CHECK(!PathExists(out + "/high/3_300.incomplete"));
    CHECK(PathExists(out + "/high/4_401"));

    uint64_t total = 0;
    for (const auto& p : rm.Scan()) total += p.bytes;
    // Keep roughly two packages' worth under quota: forces ~2 deletions, and
    // the second-oldest (6_600) is excluded -> oldest + newest go.
    rcfg.incomplete_policy = retention::IncompletePolicy::kKeep;
    rcfg.disk_quota_bytes = total - 2000;
    CHECK(rm.Init(rcfg));
    const uint64_t freed = rm.Enforce({out + "/6_600"});
    CHECK(freed >= 2000);
    CHECK(!PathExists(out + "/5_500"));           // oldest low deleted
    CHECK(PathExists(out + "/6_600"));            // active package skipped
    CHECK(!PathExists(out + "/7_700"));           // newest deleted instead
    CHECK(PathExists(out + "/high/4_401"));       // high/ untouched
  }

  // ---- Center disk_full linkage: non-crash rejected as quota, crash passes
  {
    trigger_cfg::CenterConfig ccfg;
    char sock[96];
    std::snprintf(sock, sizeof(sock), "/tmp/c9ret_%d.sock", me);
    ccfg.socket_path = sock;
    trigger_cfg::SourceRule crash;
    crash.name = "crash";
    crash.priority = 100;
    crash.quota_class = "safety";
    crash.before_ns = crash.after_ns = 1000000000ULL;
    trigger_cfg::SourceRule manual;
    manual.name = "manual";
    manual.priority = 10;
    manual.quota_class = "manual";
    manual.before_ns = manual.after_ns = 1000000000ULL;
    ccfg.sources = {crash, manual};
    ccfg.quota_limits = {{"safety", 0}, {"manual", 0}};  // required by T4 startup validation
    trigger_center::Hooks hooks;
    hooks.recorder_ready = HookReady;
    hooks.capture_active = HookActive;
    hooks.start_capture = HookStart;
    hooks.preempt_capture = HookPreempt;
    hooks.disk_full = HookDiskFull;
    trigger_center::Center center;
    CHECK(center.Init(ccfg, hooks));
    g_disk_full = true;
    using trigger_proto::TriggerCode;
    auto r = center.Submit("manual", -1, 0, false, false);
    CHECK(r.code == TriggerCode::kQuotaExceeded);  // 6.5: reject subsequent lower-priority triggers
    r = center.Submit("crash", -1, 0, false, false);
    CHECK(r.code == TriggerCode::kAccepted);  // crash still writes the high/ reserved zone
    center.Tick();  // T2: acknowledged trigger starts in Tick
    CHECK(g_active);
    center.Shutdown();
  }

  // ---- engine ENOSPC path (fault-injected write failure) ----
  {
    char page_name[96];
    std::snprintf(page_name, sizeof(page_name), "/c9ret_page_%d", me);
    ShmRegion::UnlinkName(page_name);
    ThymeStructDesc desc{};
    desc.struct_id = 0;
    desc.contract_version = 1;
    desc.payload_size = 8;
    std::snprintf(desc.name, sizeof(desc.name), "Pod8");
    RecordMirrorRingSpec spec{8, 64};
    record_mirror::RingClient mirror;
    CHECK(mirror.Create(page_name, &desc, &spec, 1, 1));
    for (uint64_t i = 0; i < 10; ++i) {
      const uint64_t mono = NowNs();  // real clock: inside the trigger window
      CHECK(mirror.Page()->TryWriteRing(0, &i, sizeof(i), mono,
                                        static_cast<uint32_t>(i)));
    }

    const std::string eng_out = out + "/eng";
    Mkdir(eng_out);
    triggered::Config ecfg;
    ecfg.sources = {{triggered::SourceConfig::Kind::kMirrorRing, page_name,
                     ""}};
    ecfg.out_dir = eng_out;
    ecfg.fault_inject_write = FaultInject;
    triggered::TriggeredRecorder rec;
    CHECK(rec.Init(ecfg));

    g_fault = true;
    CHECK(rec.Trigger(100, 60000000000ULL, 0));
    RunUntilArmed(rec);  // drain write fails -> capture actively invalidated
    CHECK(rec.disk_full());
    CHECK(rec.stats().disk_errors == 1);
    CHECK(!triggered::TriggeredRecorder::PackageComplete(
        rec.last_package_dir()));
    // The engine already renamed the package: retention finds nothing new.
    CHECK(PathExists(rec.last_package_dir() + ".incomplete"));
    // ...and the invalidated package carries an abort row on disk.
    CHECK(WalHasAbortRow(rec.last_package_dir() + ".incomplete"));
    // New captures are refused while disk_full.
    CHECK(!rec.Trigger(101, 60000000000ULL, 0));
    CHECK(rec.stats().triggers_rejected == 1);

    // 6.5: retention scan is idempotent; then the retry succeeds.
    retention::Manager rm;
    retention::Config rcfg2;
    rcfg2.out_dir = eng_out;
    CHECK(rm.Init(rcfg2));
    CHECK(rm.MarkIncomplete() == 0);
    g_fault = false;
    rec.ClearDiskFull();
    CHECK(rec.Trigger(102, 60000000000ULL, 0));
    RunUntilArmed(rec);
    CHECK(triggered::TriggeredRecorder::PackageComplete(
        rec.last_package_dir()));
    CHECK(rec.stats().captures_done == 1);
    uint64_t n = 0;
    for (const auto& e : [&] {
           ThymeBinReader rd(
               (rec.last_package_dir() + "/small.tbin").c_str());
           CHECK(rd.ok());
           std::vector<ThymeBinDataEntry> v;
           ThymeBinDataEntry en;
           while (rd.ReadNext(en) == thyme::ThymeBinReader::ReadResult::kOk) v.push_back(en);
           return v;
         }()) {
      CHECK(e.type == ThymeBinEntryType::kData);
      ++n;
    }
    CHECK(n == 10);
    mirror.Shutdown(true);
  }

  // ---- seal-path invalidation (trigger package): wal fsync fail (fault 2) and
  // seal fwrite fail (fault 1) must both end with an abort row on disk and a
  // renamed .incomplete package — never a self-sealed incomplete package. ----
  {
    char page_name[96];
    std::snprintf(page_name, sizeof(page_name), "/c9ret_page2_%d", me);
    ShmRegion::UnlinkName(page_name);
    ThymeStructDesc desc{};
    desc.struct_id = 0;
    desc.contract_version = 1;
    desc.payload_size = 8;
    std::snprintf(desc.name, sizeof(desc.name), "Pod8");
    RecordMirrorRingSpec spec{8, 64};
    record_mirror::RingClient mirror;
    CHECK(mirror.Create(page_name, &desc, &spec, 1, 1));
    for (uint64_t i = 0; i < 5; ++i) {
      CHECK(mirror.Page()->TryWriteRing(0, &i, sizeof(i), NowNs(),
                                        static_cast<uint32_t>(i)));
    }
    const std::string eng_out = out + "/eng_seal";
    Mkdir(eng_out);
    triggered::Config ecfg;
    ecfg.sources = {{triggered::SourceConfig::Kind::kMirrorRing, page_name,
                     ""}};
    ecfg.out_dir = eng_out;
    ecfg.fault_inject_seal = SealFaultInject;
    triggered::TriggeredRecorder rec;
    CHECK(rec.Init(ecfg));

    for (const int fault : {2, 1}) {
      g_seal_fault = fault;
      CHECK(rec.Trigger(200 + fault, 60000000000ULL, 0));
      RunUntilArmed(rec);
      CHECK(rec.disk_full());
      const std::string pkg = rec.last_package_dir();
      CHECK(!PathExists(pkg));                     // renamed away
      CHECK(PathExists(pkg + ".incomplete"));
      // The abort row replaced (fault 2) or followed (fault 1) the seal row;
      // in both cases no intact seal row survives on disk.
      CHECK(WalHasAbortRow(pkg + ".incomplete"));
      CHECK(!WalHasRow(pkg + ".incomplete", triggered::kWalOpSeal));
      CHECK(!triggered::TriggeredRecorder::PackageComplete(pkg));
      CHECK(!triggered::TriggeredRecorder::PackageComplete(
          pkg + ".incomplete"));  // .incomplete fast-path
      CHECK(rec.stats().captures_done == 0);
      CHECK(rec.stats().disk_errors >= 1);
      g_seal_fault = 0;
      rec.ClearDiskFull();
    }
    // Happy-path regression: seals completely.
    CHECK(rec.Trigger(300, 60000000000ULL, 0));
    RunUntilArmed(rec);
    CHECK(triggered::TriggeredRecorder::PackageComplete(
        rec.last_package_dir()));
    CHECK(rec.stats().captures_done == 1);
    mirror.Shutdown(true);
  }

  // ---- seal-path invalidation (continuous-slice CloseSlice) ----
  {
    char page_name[96];
    std::snprintf(page_name, sizeof(page_name), "/c9ret_page3_%d", me);
    ShmRegion::UnlinkName(page_name);
    ThymeStructDesc desc{};
    desc.struct_id = 0;
    desc.contract_version = 1;
    desc.payload_size = 8;
    std::snprintf(desc.name, sizeof(desc.name), "Pod8");
    RecordMirrorRingSpec spec{8, 64};
    record_mirror::RingClient mirror;
    CHECK(mirror.Create(page_name, &desc, &spec, 1, 1));
    const std::string eng_out = out + "/eng_slice";
    Mkdir(eng_out);
    triggered::Config ecfg;
    ecfg.sources = {{triggered::SourceConfig::Kind::kMirrorRing, page_name,
                     ""}};
    ecfg.out_dir = eng_out;
    ecfg.fault_inject_seal = SealFaultInject;
    triggered::TriggeredRecorder rec;
    CHECK(rec.Init(ecfg));
    triggered::ContinuousConfig ccfg;
    ccfg.slice_bytes = 1000000;
    CHECK(rec.ArmContinuous(ccfg));
    for (uint64_t i = 0; i < 5; ++i) {
      CHECK(mirror.Page()->TryWriteRing(0, &i, sizeof(i), NowNs(),
                                        static_cast<uint32_t>(i)));
      rec.Tick();
    }
    const std::string slice_dir = rec.current_slice_dir();
    g_seal_fault = 2;  // wal fsync fails at slice close
    rec.DisarmContinuous();
    CHECK(rec.continuous_stats().slices_done == 0);
    CHECK(rec.disk_full());
    CHECK(!PathExists(slice_dir));
    CHECK(PathExists(slice_dir + ".incomplete"));
    CHECK(WalHasAbortRow(slice_dir + ".incomplete"));
    CHECK(!triggered::TriggeredRecorder::PackageComplete(slice_dir));
    mirror.Shutdown(true);
  }

  // ---- PackageComplete WAL boundary matrix. The last complete row
  // must be seal, any abort wins, and malformed/truncated tails fail closed.
  {
    const std::string wal = out + "/wal_matrix";
    Mkdir(wal);
    const auto complete = [](const std::string& dir) {
      return triggered::TriggeredRecorder::PackageComplete(dir);
    };
    CHECK(!complete(wal + "/missing"));
    MakeWal(wal + "/bad_magic", {triggered::kWalOpSeal}, true);
    CHECK(!complete(wal + "/bad_magic"));
    MakeWal(wal + "/short_header", {}, false, true);
    CHECK(!complete(wal + "/short_header"));
    MakeWal(wal + "/header_only", {});
    CHECK(!complete(wal + "/header_only"));
    MakeWal(wal + "/descriptor", {triggered::kWalOpDescriptor});
    CHECK(!complete(wal + "/descriptor"));
    MakeWal(wal + "/sealed", {triggered::kWalOpSeal});
    CHECK(complete(wal + "/sealed"));
    MakeWal(wal + "/descriptor_seal",
            {triggered::kWalOpDescriptor, triggered::kWalOpSeal});
    CHECK(complete(wal + "/descriptor_seal"));
    MakeWal(wal + "/aborted", {triggered::kWalOpAbort});
    CHECK(!complete(wal + "/aborted"));
    MakeWal(wal + "/seal_abort",
            {triggered::kWalOpSeal, triggered::kWalOpAbort});
    CHECK(!complete(wal + "/seal_abort"));
    MakeWal(wal + "/abort_seal",
            {triggered::kWalOpAbort, triggered::kWalOpSeal});
    CHECK(!complete(wal + "/abort_seal"));
    MakeWal(wal + "/seal_descriptor",
            {triggered::kWalOpSeal, triggered::kWalOpDescriptor});
    CHECK(!complete(wal + "/seal_descriptor"));
    MakeWal(wal + "/truncated_tail", {triggered::kWalOpSeal}, false, false, 3);
    CHECK(!complete(wal + "/truncated_tail"));
    MakeWal(wal + "/sealed.incomplete", {triggered::kWalOpSeal});
    CHECK(!complete(wal + "/sealed.incomplete"));
  }

  // Cleanup tree.
  retention::Manager::RemoveTree(out);
  return 0;
}
