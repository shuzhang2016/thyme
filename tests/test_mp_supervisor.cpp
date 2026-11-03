// MpSupervisorSession integration: real fork/exec of role children against
// SHM objects created by the table-driven supervisor skeleton. Covers the
// happy path, the record-mirror gate contract, child failure propagation, the
// SHM-create failure cleanup (mirror unlink), the too-many-processes abort,
// and the WaitChildTimeout SIGKILL fallback. The playbook (duration loop,
// gate composition, report, exit code) is the app's job; this test drives the
// session primitives directly.
//
// The session execs this same binary as a role child:
//   test_mp_supervisor --role <role> --shm-prefix <prefix> [--record-shm <n>]
#include "thyme/check.h"
#include "thyme/app/mp_control.h"
#include "thyme/app/mp_runtime.h"
#include "thyme/app/mp_supervisor.h"
#include "thyme/app/record_mirror.h"
#include "thyme/app/time.h"
#include "thyme/contracts/struct_desc.h"
#include "thyme/record/record_table.h"
#include "thyme/runtime/shm_region.h"
#include "thyme/runtime/shared_health.h"
#include "test_wait.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <sys/mman.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

using thyme::app::CrossGatesOk;
using thyme::app::HealthScanOk;
using thyme::app::LoopsOk;
using thyme::app::MpCrossGate;
using thyme::app::MpHealthScanSummary;
using thyme::app::MpNode;
using thyme::app::MpProcess;
using thyme::app::MpRecordGate;
using thyme::app::MpShmChannel;
using thyme::app::MpSupervisorConfig;
using thyme::app::MpSupervisorSession;
using thyme::app::ScanHealthPage;

constexpr MpProcess kProcs[] = {{"perception", "perception"}, {"rest", "rest"}};
constexpr MpNode kNodes[] = {{"perception", "perception", "perception", 10},
                             {"rest", "rest", "rest", 10}};
constexpr MpRecordGate kGates[] = {{"perception", 0}, {"rest", 1}};

uint32_t TestPayloadSize(uint16_t) { return 16; }
thyme::ThymeRecordPriority TestPriority(uint16_t) {
  return thyme::ThymeRecordPriority::kP1;
}
void TestFillDescs(thyme::ThymeStructDesc* out, std::size_t cap) {
  CHECK(cap >= 2);
  for (std::size_t i = 0; i < 2; ++i) {
    out[i].struct_id = static_cast<uint16_t>(i);
    out[i].contract_version = 1;
    out[i].payload_size = 16;
    std::snprintf(out[i].name, sizeof(out[i].name), "s%zu", i);
  }
}
const thyme::ThymeRecordTable kTestRecordTable{2, &TestPayloadSize,
                                               &TestPriority, &TestFillDescs};

MpSupervisorConfig MakeConfig() {
  MpSupervisorConfig cfg;
  cfg.processes = kProcs;
  cfg.nodes = kNodes;
  cfg.record_gates = kGates;
  cfg.health_n = 2;
  cfg.record_table = &kTestRecordTable;
  return cfg;
}

// TST-07: the child publishes its poll-loop count (release store) on every
// iteration, so "the roles have started" is observable. Waiting for that
// handshake replaces the fixed 300 ms settle sleep; a stopped child fails the
// CHECK at the deadline instead of silently passing a short test.
bool WaitChildrenLooping(MpSupervisorSession& session,
                         uint64_t loops = 5,
                         uint64_t timeout_ns = 5000000000ULL) {
  const bool ok = thyme_test::WaitFor(
      [&] {
        for (std::size_t i = 0; i < std::size(kProcs); ++i) {
          if (session.ctrl()->proc_loops[i].load(std::memory_order_acquire) <
              loops) {
            return false;
          }
        }
        return true;
      },
      timeout_ns);
  if (!ok) {
    for (std::size_t i = 0; i < std::size(kProcs); ++i) {
      std::fprintf(stderr, "role %zu (%s) proc_loops=%llu (wanted %llu)\n", i,
                   kProcs[i].role,
                   static_cast<unsigned long long>(
                       session.ctrl()->proc_loops[i].load(
                           std::memory_order_acquire)),
                   static_cast<unsigned long long>(loops));
    }
  }
  return ok;
}

std::string MirrorName() {
  char buf[128];
  std::snprintf(buf, sizeof(buf), "/thyme_test_mp_%d", static_cast<int>(getpid()));
  return buf;
}

bool ShmNameExists(const char* name) {
  const int fd = shm_open(name, O_RDONLY, 0);
  if (fd >= 0) {
    close(fd);
    return true;
  }
  return errno != ENOENT;  // ENOENT -> cleanly unlinked
}

// ---- loops gate (pure, no fork) -----------------------------------------

void TestLoopsGate() {
  // Each process meets its own bound (no privileged role, no aggregate).
  {
    MpSupervisorConfig cfg = MakeConfig();  // perception 10Hz, rest 10Hz
    // Bound @2s = 10*2/4 = 5 for each process.
    const uint64_t at_bound[] = {5, 5};
    CHECK(LoopsOk(cfg, 2, at_bound));
    const uint64_t high[] = {6, 25};
    CHECK(LoopsOk(cfg, 2, high));
    const uint64_t rest_below[] = {5, 4};
    CHECK(!LoopsOk(cfg, 2, rest_below));
    const uint64_t perc_below[] = {4, 5};
    CHECK(!LoopsOk(cfg, 2, perc_below));
    const uint64_t none[] = {0, 0};
    CHECK(!LoopsOk(cfg, 2, none));
  }
  // Processes with different bounds are each judged independently: a fast
  // process can no longer mask a slow one (or be masked by it).
  {
    const MpProcess procs[] = {{"perception", "perception"},
                               {"rest", "rest"},
                               {"ar2", "ar2"}};
    const MpNode nodes[] = {{"perception", "perception", "perception", 10},
                            {"rest", "rest", "rest", 10},
                            {"ar2", "ar2", "ar2", 50}};
    MpSupervisorConfig cfg = MakeConfig();
    cfg.processes = procs;
    cfg.nodes = nodes;
    cfg.health_n = 3;
    // Bounds @2s: perception=5, rest=5, ar2=25.
    const uint64_t ok[] = {5, 5, 25};
    CHECK(LoopsOk(cfg, 2, ok));
    const uint64_t fast_short[] = {5, 5, 24};  // ar2 below its own 25
    CHECK(!LoopsOk(cfg, 2, fast_short));
  }
  // No perception process: every remaining process still meets its own bound.
  {
    const MpProcess procs[] = {{"rest", "rest"}};
    const MpNode nodes[] = {{"rest", "rest", "rest", 10}};
    MpSupervisorConfig cfg = MakeConfig();
    cfg.processes = procs;
    cfg.nodes = nodes;
    cfg.health_n = 1;
    const uint64_t ok[] = {5};
    CHECK(LoopsOk(cfg, 2, ok));
    const uint64_t below[] = {4};
    CHECK(!LoopsOk(cfg, 2, below));
  }
}

void TestHealthScanAggregate() {
  thyme::SharedHealthPage page{};
  CHECK(page.Init(1, 2));
  page.slots[0].drop_cnt.store(3, std::memory_order_relaxed);
  page.slots[0].deadline_miss_cnt.store(1, std::memory_order_relaxed);
  page.slots[1].error_code.store(7, std::memory_order_relaxed);
  MpSupervisorConfig cfg = MakeConfig();
  const MpHealthScanSummary s = ScanHealthPage(cfg, page);
  CHECK(s.workers_scanned == 2);
  CHECK(s.total_drops == 3);
  CHECK(s.total_deadline_miss == 1);
  CHECK(s.total_errors == 1);
  CHECK(!HealthScanOk(s, true, true, false));
  CHECK(!HealthScanOk(s, true, false, false));
  CHECK(!HealthScanOk(s, false, true, false));
  CHECK(HealthScanOk(s, false, false, false));
  CHECK(!HealthScanOk(s, false, false, true));
}

void TestHealthScanSkipsReporter() {
  const MpNode nodes[] = {{"hr", "health_reporter", "rest", 1},
                          {"tick", "tick", "rest", 10}};
  MpSupervisorConfig cfg = MakeConfig();
  cfg.nodes = nodes;
  cfg.health_n = 2;
  thyme::SharedHealthPage page{};
  CHECK(page.Init(1, 2));
  page.slots[0].drop_cnt.store(99, std::memory_order_relaxed);
  page.slots[1].drop_cnt.store(1, std::memory_order_relaxed);
  const MpHealthScanSummary s = ScanHealthPage(cfg, page);
  CHECK(s.workers_scanned == 1);
  CHECK(s.total_drops == 1);
}

// ---- cross-process frame gates (pure, no fork) ---------------------------

void TestCrossGates() {
  // kind 1 crosses processes (perception -> rest); kind 2 is same-process.
  constexpr MpShmChannel kChannels[] = {{"_ob", 1, "perception", "rest"},
                                        {"_ln", 2, "rest", "rest"}};
  uint64_t frames[thyme::app::MpControl::kMaxCrossFrameSlots]{};
  const thyme::span<const uint64_t> frame_span(
      frames, thyme::app::MpControl::kMaxCrossFrameSlots);
  // No gates: passes even with zero frames (check disabled).
  {
    MpSupervisorConfig cfg = MakeConfig();
    cfg.shm_channels = kChannels;
    CHECK(CrossGatesOk(cfg, frame_span));
  }
  // Gate met: gated kind crosses processes and its slot saw a frame.
  {
    constexpr MpCrossGate kGate[] = {{1, 0}};
    MpSupervisorConfig cfg = MakeConfig();
    cfg.shm_channels = kChannels;
    cfg.cross_gates = kGate;
    frames[0] = 1;
    CHECK(CrossGatesOk(cfg, frame_span));
    // Gate unmet: counter still zero.
    frames[0] = 0;
    CHECK(!CrossGatesOk(cfg, frame_span));
  }
  // Gate on a non-cross kind: misconfiguration fails even with frames.
  {
    constexpr MpCrossGate kGate[] = {{2, 0}};
    MpSupervisorConfig cfg = MakeConfig();
    cfg.shm_channels = kChannels;
    cfg.cross_gates = kGate;
    frames[0] = 5;
    CHECK(!CrossGatesOk(cfg, frame_span));
    frames[0] = 0;
  }
  // Gate slot out of range: misconfiguration fails.
  {
    constexpr MpCrossGate kGate[] = {{1, 99}};
    MpSupervisorConfig cfg = MakeConfig();
    cfg.shm_channels = kChannels;
    cfg.cross_gates = kGate;
    CHECK(!CrossGatesOk(cfg, frame_span));
  }
}

// ---- role child ----------------------------------------------------------

int RunChild(int argc, char** argv) {
  const char* role = nullptr;
  const char* prefix = nullptr;
  const char* record_shm = nullptr;
  int proc_idx = -1;
  bool fail_exit = std::getenv("T9_MP_SUPERVISOR_CHILD_FAIL") != nullptr;
  const bool inject_drops =
      std::getenv("T9_MP_SUPERVISOR_CHILD_DROPS") != nullptr;
  const bool record_ring = std::getenv("T9_MP_SUPERVISOR_RING") != nullptr;
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--role") == 0 && i + 1 < argc) role = argv[++i];
    else if (std::strcmp(argv[i], "--shm-prefix") == 0 && i + 1 < argc) prefix = argv[++i];
    else if (std::strcmp(argv[i], "--record-shm") == 0 && i + 1 < argc) record_shm = argv[++i];
    else if (std::strcmp(argv[i], "--proc-idx") == 0 && i + 1 < argc) proc_idx = std::atoi(argv[++i]);
  }
  if (role == nullptr || prefix == nullptr) return 2;
  if (proc_idx < 0 ||
      static_cast<std::size_t>(proc_idx) >= thyme::app::MpControl::kMaxProcSlots)
    return 2;

  thyme::app::MpRoleSession sess;
  const bool opened =
      record_ring
          ? sess.Open(prefix, record_shm, 2, thyme::app::kMpOpenTimeoutNs, 0,
                      true)
          : sess.Open(prefix, record_shm, 2, thyme::app::kMpOpenTimeoutNs);
  if (!opened) {
    return 2;
  }

  // Write my record gate slot (one gate per producer process).
  if (record_shm != nullptr && record_shm[0] != '\0') {
    uint8_t payload[16] = {1, 2, 3, 4};
    for (const auto& g : kGates) {
      if (std::strcmp(g.process, role) == 0) {
        if (record_ring) {
          CHECK(thyme::app::record_mirror::RingPage()->TryWriteRing(
              g.slot, payload, sizeof(payload), thyme::app::NowMonoNs(), 1));
        } else {
          CHECK(thyme::app::record_mirror::Page()->WriteSlot(
              g.slot, payload, sizeof(payload), thyme::app::NowMonoNs(), 1));
        }
        break;
      }
    }
  }

  if (inject_drops) {
    std::size_t slot = 0;
    for (std::size_t i = 0; i < std::size(kNodes); ++i) {
      if (std::strcmp(kNodes[i].process, role) == 0) {
        slot = i;
        break;
      }
    }
    sess.slots()[slot].AddDrops(1);
  }

  // Loop until the supervisor signals stop (or a safety deadline). The loop
  // counter is published every iteration (not only at exit) so the test can
  // wait for the child's readiness handshake instead of sleeping for 300 ms.
  uint64_t loops = 0;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (!sess.stop_requested() &&
         std::chrono::steady_clock::now() < deadline) {
    sess.ctrl()->proc_loops[proc_idx].store(loops,
                                            std::memory_order_release);
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    ++loops;
  }

  sess.ctrl()->proc_loops[proc_idx].store(loops, std::memory_order_release);

  if (record_shm != nullptr && record_shm[0] != '\0') {
    if (record_ring) {
      thyme::app::record_mirror::ShutdownRing(false);
    } else {
      thyme::app::record_mirror::Shutdown(false);
    }
  }
  if (fail_exit) return 1;
  return loops >= 1 ? 0 : 1;
}

// ---- tests ---------------------------------------------------------------

void TestWaitChildTimeout() {
  pid_t pid = fork();
  CHECK(pid >= 0);
  if (pid == 0) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1000));
    _exit(0);
  }
  bool timed = false;
  const int st = thyme::app::WaitChildTimeout(pid, 5ULL * 1000 * 1000, &timed);
  CHECK(timed);
  CHECK(WIFSIGNALED(st));
  CHECK(WTERMSIG(st) == SIGKILL);
  // Already reaped by WaitChildTimeout.
  int status = 0;
  CHECK(waitpid(pid, &status, 0) == -1);
}

const char* ResolveRoleExecutable(void*, const char*, const char* self_exe) {
  return self_exe;
}

const char* ResolveFalseExecutable(void*, const char*, const char*) {
  return "/bin/false";
}

void TestRoleExecutableResolver() {
  auto cfg = MakeConfig();
  cfg.role_executable = ResolveFalseExecutable;
  thyme::app::MpSupervisorSession session(cfg);
  CHECK(session.Prepare(nullptr));
  CHECK(session.ForkRoles("/proc/self/exe"));
  session.SignalStop();
  CHECK(!session.ReapChildren());
  session.Cleanup();
}

void TestHappyPath() {
  auto cfg = MakeConfig();
  cfg.role_executable = ResolveRoleExecutable;
  thyme::app::MpSupervisorSession session(cfg);
  CHECK(session.Prepare(nullptr));
  CHECK(session.ForkRoles("/proc/self/exe"));
  CHECK(WaitChildrenLooping(session));
  session.SignalStop();
  const bool all_ok = session.ReapChildren();
  uint64_t proc_loops[thyme::app::MpControl::kMaxProcSlots]{};
  uint64_t cross_frames[thyme::app::MpControl::kMaxCrossFrameSlots]{};
  session.Collect(proc_loops, cross_frames);
  CHECK(all_ok);
  CHECK(LoopsOk(MakeConfig(), 2,
                thyme::span<const uint64_t>(proc_loops, 2)));
  session.Cleanup();
}

void TestRecordMode() {
  const std::string name = MirrorName();
  thyme::app::MpSupervisorSession session(MakeConfig());
  CHECK(session.Prepare(name.c_str()));
  CHECK(session.ForkRoles("/proc/self/exe"));
  CHECK(WaitChildrenLooping(session));
  session.SignalStop();
  CHECK(session.ReapChildren());
  session.Cleanup();
  // The supervisor unlinks the mirror it created (success path).
  CHECK(!ShmNameExists(name.c_str()));
}

void TestRecordRingMode() {
  const std::string name = MirrorName();
  constexpr thyme::RecordMirrorRingSpec specs[] = {{16, 1}, {16, 1}};
  MpSupervisorConfig cfg = MakeConfig();
  cfg.record_ring_specs = specs;
  CHECK(setenv("T9_MP_SUPERVISOR_RING", "1", 1) == 0);
  thyme::app::MpSupervisorSession session(cfg);
  CHECK(session.Prepare(name.c_str()));
  const thyme::RecordMirrorRingPage* page = thyme::app::record_mirror::RingPage();
  CHECK(page != nullptr && page->Validate());
  CHECK(page->struct_count() == 2);
  CHECK(page->arena_bytes() == 256);
  CHECK(session.ForkRoles("/proc/self/exe"));
  CHECK(WaitChildrenLooping(session));
  session.SignalStop();
  CHECK(session.ReapChildren());
  CHECK(page->RingHead(0) == 1);
  CHECK(page->RingHead(1) == 1);
  session.Cleanup();
  CHECK(unsetenv("T9_MP_SUPERVISOR_RING") == 0);
  CHECK(!ShmNameExists(name.c_str()));
}

void TestChildFailure() {
  CHECK(setenv("T9_MP_SUPERVISOR_CHILD_FAIL", "1", 1) == 0);
  thyme::app::MpSupervisorSession session(MakeConfig());
  CHECK(session.Prepare(nullptr));
  CHECK(session.ForkRoles("/proc/self/exe"));
  CHECK(WaitChildrenLooping(session));
  session.SignalStop();
  const bool all_ok = session.ReapChildren();
  CHECK(unsetenv("T9_MP_SUPERVISOR_CHILD_FAIL") == 0);
  CHECK(!all_ok);  // a child exiting non-zero fails the run
  session.Cleanup();
}

// ARCH-11: role attach validates the full super-header on both pages. A
// tampered health or control page must fail Open() before any counter is read.
void TestRoleOpenRejectsTamperedPages() {
  char prefix[64];
  std::snprintf(prefix, sizeof(prefix), "/thyme_test_mprole_%d",
                static_cast<int>(getpid()));
  char health_name[80];
  char ctrl_name[80];
  std::snprintf(health_name, sizeof(health_name), "%s_h", prefix);
  std::snprintf(ctrl_name, sizeof(ctrl_name), "%s_c", prefix);
  thyme::ShmRegion::UnlinkName(health_name);
  thyme::ShmRegion::UnlinkName(ctrl_name);

  thyme::ShmRegion health;
  CHECK(health.CreateDescribed(health_name, thyme::SharedHealthPage::Spec()));
  auto* health_page = static_cast<thyme::SharedHealthPage*>(health.data());
  CHECK(health_page->Init(static_cast<uint32_t>(getpid()), 2));
  thyme::ShmRegion ctrl;
  CHECK(ctrl.CreateDescribed(ctrl_name, thyme::app::MpControl::Spec()));
  auto* ctrl_page = static_cast<thyme::app::MpControl*>(ctrl.data());
  ctrl_page->Init();

  {
    thyme::app::MpRoleSession session;
    CHECK(session.Open(prefix, nullptr, 2, 0));
    CHECK(session.health_page()->worker_count() == 2);
    CHECK(session.ctrl()->Validate());
    session.Close();
  }
  {
    thyme::app::MpRoleSession rejected;
    health_page->super.magic.store(0xDEADBEEFULL,
                                   std::memory_order_release);
    CHECK(!rejected.Open(prefix, nullptr, 2, 0));
    health_page->super.magic.store(thyme::SharedHealthPage::kMagic,
                                   std::memory_order_release);
    health_page->header.worker_count.store(3, std::memory_order_relaxed);
    CHECK(!rejected.Open(prefix, nullptr, 2, 0));
    health_page->header.worker_count.store(2, std::memory_order_relaxed);
    health_page->header.reserved[0] = 1;
    CHECK(!rejected.Open(prefix, nullptr, 2, 0));
    health_page->header.reserved[0] = 0;
    ctrl_page->super.magic.store(0xDEADBEEFULL, std::memory_order_release);
    CHECK(!rejected.Open(prefix, nullptr, 2, 0));
    ctrl_page->super.magic.store(thyme::app::MpControl::kMagic,
                                 std::memory_order_release);
    ctrl_page->super.layout_hash.store(0xDEADBEEFULL,
                                       std::memory_order_relaxed);
    CHECK(!rejected.Open(prefix, nullptr, 2, 0));
    ctrl_page->super.layout_hash.store(thyme::app::MpControlLayoutHash(),
                                       std::memory_order_relaxed);
    ctrl_page->reserved[0] = 1;
    CHECK(!rejected.Open(prefix, nullptr, 2, 0));
    ctrl_page->reserved[0] = 0;
    CHECK(rejected.Open(prefix, nullptr, 2, 0));
    rejected.Close();
  }

  health.Unmap();
  ctrl.Unmap();
  thyme::ShmRegion::UnlinkName(health_name);
  thyme::ShmRegion::UnlinkName(ctrl_name);
}

void TestCreateFailureCleanup() {
  const std::string name = MirrorName();
  CHECK(setenv("T9_MP_SUPERVISOR_FAIL_CREATE", "1", 1) == 0);
  thyme::app::MpSupervisorSession session(MakeConfig());
  const bool prepared = session.Prepare(name.c_str());
  CHECK(unsetenv("T9_MP_SUPERVISOR_FAIL_CREATE") == 0);
  CHECK(!prepared);
  // Failure path must unlink the mirror (B1 mirror-leak fix).
  CHECK(!ShmNameExists(name.c_str()));
}

void TestHealthStrictFails() {
  CHECK(setenv("T9_MP_SUPERVISOR_CHILD_DROPS", "1", 1) == 0);
  MpSupervisorConfig cfg = MakeConfig();
  cfg.health_scan_strict = true;
  thyme::app::MpSupervisorSession session(cfg);
  CHECK(session.Prepare(nullptr));
  CHECK(session.ForkRoles("/proc/self/exe"));
  CHECK(WaitChildrenLooping(session));
  session.SignalStop();
  const bool all_ok = session.ReapChildren();
  CHECK(unsetenv("T9_MP_SUPERVISOR_CHILD_DROPS") == 0);
  CHECK(all_ok);  // children exit 0, but drops were injected
  const thyme::SharedHealthPage* page = session.health_page();
  CHECK(page != nullptr && page->Validate());
  const MpHealthScanSummary s = ScanHealthPage(cfg, *page);
  CHECK(s.total_drops > 0);
  CHECK(!HealthScanOk(s, /*strict_drops=*/true,
                      /*strict_errors=*/true,
                      /*strict_deadline_miss=*/false));
  session.Cleanup();
}

void TestTooManyProcesses() {
  std::vector<MpProcess> procs;
  std::vector<MpNode> nodes;
  std::vector<std::string> ids, roles;
  ids.reserve(17);
  roles.reserve(17);
  for (int i = 0; i < 17; ++i) {
    ids.push_back("p" + std::to_string(i));
    roles.push_back("r" + std::to_string(i));
    procs.push_back(MpProcess{ids.back().c_str(), roles.back().c_str()});
    nodes.push_back(MpNode{ids.back().c_str(), "tick", roles.back().c_str(), 10});
  }
  MpSupervisorConfig cfg = MakeConfig();
  cfg.processes = thyme::span<const MpProcess>(procs.data(), procs.size());
  cfg.nodes = thyme::span<const MpNode>(nodes.data(), nodes.size());
  cfg.record_gates = {};
  cfg.health_n = 17;
  thyme::app::MpSupervisorSession session(cfg);
  CHECK(session.Prepare(nullptr));
  // 16 children are forked before the 17th aborts; the abort must stop and
  // reap them (bounded by the wait timeout), never leak orphans.
  CHECK(!session.ForkRoles("/proc/self/exe"));
}

}  // namespace

int main(int argc, char** argv) {
  if (argc >= 2 && std::strcmp(argv[1], "--role") == 0) {
    return RunChild(argc, argv);
  }
  TestWaitChildTimeout();
  TestLoopsGate();
  TestHealthScanAggregate();
  TestHealthScanSkipsReporter();
  TestCrossGates();
  TestRoleExecutableResolver();
  TestHappyPath();
  TestRecordMode();
  TestRecordRingMode();
  TestChildFailure();
  TestHealthStrictFails();
  TestRoleOpenRejectsTamperedPages();
  TestCreateFailureCleanup();
  TestTooManyProcesses();
  return 0;
}
