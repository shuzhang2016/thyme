// OPS-01 supervisor restart: real fork/exec children driven through
// MpSupervisorSession::PollRestarts. Covers immediate-exit respawn up to the
// budget then kGaveUp, observable exponential backoff spacing, a healthy role
// never being restarted, a stalled role (frozen heartbeat) being terminated
// and respawned, restart-disabled behaviour, and the counter/observability
// contract.
//
// The session execs this same binary as a role child:
//   test_mp_supervisor_restart --role <role> --shm-prefix <prefix>
//                              --proc-idx <n>
#include "thyme/check.h"
#include "thyme/app/mp_control.h"
#include "thyme/app/mp_runtime.h"
#include "thyme/app/mp_supervisor.h"
#include "thyme/app/time.h"
#include "thyme/runtime/shared_health.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <thread>
#include <unistd.h>

namespace {

using thyme::app::MpNode;
using thyme::app::MpProcess;
using thyme::app::MpRestartStats;
using thyme::app::MpRoleState;
using thyme::app::MpSupervisorConfig;
using thyme::app::MpSupervisorSession;
using thyme::app::NowMonoNs;

constexpr MpProcess kProcs[] = {{"worker", "worker"}};
constexpr MpNode kNodes[] = {{"worker", "tick", "worker", 10}};

constexpr char kExitNowEnv[] = "T9_MP_RESTART_CHILD_EXIT";
constexpr char kStallEnv[] = "T9_MP_RESTART_CHILD_STALL";

MpSupervisorConfig MakeConfig() {
  MpSupervisorConfig cfg;
  cfg.processes = kProcs;
  cfg.nodes = kNodes;
  cfg.health_n = 1;
  cfg.restart_enabled = true;
  cfg.max_restarts = 3;
  cfg.restart_window_ns = 5ULL * 1000 * 1000 * 1000;
  cfg.restart_backoff_initial_ns = 20ULL * 1000 * 1000;
  cfg.restart_backoff_max_ns = 200ULL * 1000 * 1000;
  cfg.restart_backoff_multiplier = 2;
  cfg.restart_kill_grace_ns = 50ULL * 1000 * 1000;
  cfg.restart_stall_timeout_ns = 0;
  return cfg;
}

template <typename Pred>
bool PollUntil(MpSupervisorSession& session, Pred pred, uint64_t timeout_ms) {
  const auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::milliseconds(timeout_ms);
  for (;;) {
    session.PollRestarts();
    if (pred()) return true;
    if (std::chrono::steady_clock::now() >= deadline) return false;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
}

// ---- role child ----------------------------------------------------------

int RunChild(int argc, char** argv) {
  const char* role = nullptr;
  const char* prefix = nullptr;
  int proc_idx = -1;
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--role") == 0 && i + 1 < argc) {
      role = argv[++i];
    } else if (std::strcmp(argv[i], "--shm-prefix") == 0 && i + 1 < argc) {
      prefix = argv[++i];
    } else if (std::strcmp(argv[i], "--proc-idx") == 0 && i + 1 < argc) {
      proc_idx = std::atoi(argv[++i]);
    }
  }
  if (role == nullptr || prefix == nullptr || proc_idx < 0 ||
      static_cast<std::size_t>(proc_idx) >=
          thyme::app::MpControl::kMaxProcSlots) {
    return 2;
  }
  // Immediate-death role: exit before attaching, exercising the "child dies
  // instantly" restart path.
  if (std::getenv(kExitNowEnv) != nullptr) return 1;

  thyme::app::MpRoleSession sess;
  if (!sess.Open(prefix, nullptr, 1, thyme::app::kMpOpenTimeoutNs)) return 2;

  const bool stall = std::getenv(kStallEnv) != nullptr;
  // Stall role: heartbeat a few times, then freeze both heartbeat and
  // proc_loops so the supervisor's stall detector fires.
  int beats_left = stall ? 3 : -1;
  uint64_t loops = 0;
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (!sess.stop_requested() &&
         std::chrono::steady_clock::now() < deadline) {
    if (beats_left != 0) {
      const uint64_t now = NowMonoNs();
      for (std::size_t i = 0; i < std::size(kNodes); ++i) {
        if (std::strcmp(kNodes[i].process, role) == 0) {
          sess.slots()[i].MarkAlive(now);
        }
      }
      sess.ctrl()->proc_loops[proc_idx].store(loops,
                                              std::memory_order_release);
      if (beats_left > 0) --beats_left;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    ++loops;
  }
  sess.ctrl()->proc_loops[proc_idx].store(loops, std::memory_order_release);
  return 0;
}

// ---- tests ---------------------------------------------------------------

// A role that exits immediately is respawned exactly max_restarts times, then
// the supervisor gives up and reports it.
void TestImmediateExitGivesUp() {
  CHECK(setenv(kExitNowEnv, "1", 1) == 0);
  MpSupervisorConfig cfg = MakeConfig();
  cfg.max_restarts = 2;
  cfg.restart_backoff_initial_ns = 10ULL * 1000 * 1000;
  cfg.restart_backoff_max_ns = 40ULL * 1000 * 1000;

  MpSupervisorSession session(cfg);
  CHECK(session.Prepare(nullptr));
  CHECK(session.ForkRoles("/proc/self/exe"));

  const bool gave_up = PollUntil(
      session,
      [&]() {
        return session.restart_stats().role[0].state == MpRoleState::kGaveUp;
      },
      2000);
  const MpRestartStats stats = session.restart_stats();
  CHECK(unsetenv(kExitNowEnv) == 0);

  CHECK(gave_up);
  CHECK(stats.role[0].state == MpRoleState::kGaveUp);
  CHECK(stats.role[0].pid == 0);
  CHECK(stats.role[0].restarts_attempted == 2);
  CHECK(stats.role[0].restarts_succeeded == 0);
  CHECK(stats.role[0].last_exit_code == 1);
  CHECK(stats.role[0].last_term_signal == 0);
  CHECK(stats.restarts_attempted == 2);
  CHECK(stats.restarts_succeeded == 0);
  CHECK(stats.restart_giveups == 1);

  session.SignalStop();
  // The role is gone for good, so the run's success gate must fail.
  CHECK(!session.ReapChildren());
  session.Cleanup();
}

// Two consecutive respawns are at least the configured backoff apart, and the
// backoff grows exponentially.
void TestBackoffSpacing() {
  CHECK(setenv(kExitNowEnv, "1", 1) == 0);
  MpSupervisorConfig cfg = MakeConfig();
  cfg.max_restarts = 3;
  cfg.restart_backoff_initial_ns = 20ULL * 1000 * 1000;
  cfg.restart_backoff_max_ns = 100ULL * 1000 * 1000;
  cfg.restart_backoff_multiplier = 2;

  MpSupervisorSession session(cfg);
  CHECK(session.Prepare(nullptr));
  CHECK(session.ForkRoles("/proc/self/exe"));

  uint64_t restart_times[4] = {};
  std::size_t observed = 0;
  uint32_t last_attempts = 0;
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(2);
  for (;;) {
    session.PollRestarts();
    const MpRestartStats stats = session.restart_stats();
    if (stats.role[0].restarts_attempted > last_attempts) {
      CHECK(observed < std::size(restart_times));
      restart_times[observed++] = stats.role[0].last_restart_ns;
      last_attempts = stats.role[0].restarts_attempted;
    }
    if (stats.role[0].state == MpRoleState::kGaveUp) break;
    if (std::chrono::steady_clock::now() >= deadline) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  const MpRestartStats stats = session.restart_stats();
  CHECK(unsetenv(kExitNowEnv) == 0);

  CHECK(stats.role[0].state == MpRoleState::kGaveUp);
  CHECK(observed == 3);
  CHECK(stats.role[0].restarts_attempted == 3);
  CHECK(stats.restarts_attempted == 3);
  // The child dies immediately, so each gap is bounded below by the backoff
  // used for that incarnation: 20 ms, then 40 ms (doubled), then 80 ms.
  CHECK(restart_times[1] - restart_times[0] >= 20ULL * 1000 * 1000);
  CHECK(restart_times[2] - restart_times[1] >= 40ULL * 1000 * 1000);
  CHECK(restart_times[1] - restart_times[0] <= 500ULL * 1000 * 1000);
  CHECK(restart_times[2] - restart_times[1] <= 500ULL * 1000 * 1000);

  session.SignalStop();
  (void)session.ReapChildren();
  session.Cleanup();
}

// A role that keeps heartbeating is never touched.
void TestHealthyRoleNotRestarted() {
  MpSupervisorConfig cfg = MakeConfig();
  cfg.restart_stall_timeout_ns = 100ULL * 1000 * 1000;
  cfg.max_restarts = 3;

  MpSupervisorSession session(cfg);
  CHECK(session.Prepare(nullptr));
  CHECK(session.ForkRoles("/proc/self/exe"));

  const auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::milliseconds(300);
  while (std::chrono::steady_clock::now() < deadline) {
    session.PollRestarts();
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  const MpRestartStats stats = session.restart_stats();
  CHECK(stats.role[0].state == MpRoleState::kRunning);
  CHECK(stats.role[0].pid > 0);
  CHECK(stats.role[0].restarts_attempted == 0);
  CHECK(stats.role[0].restarts_succeeded == 0);
  CHECK(stats.restarts_attempted == 0);
  CHECK(stats.restart_giveups == 0);

  // The heartbeat really was written (otherwise "healthy" would be vacuous).
  const thyme::SharedHealthPage* page = session.health_page();
  CHECK(page != nullptr && page->Validate());
  CHECK(page->slots[0].alive_counter.load(std::memory_order_acquire) > 0);

  session.SignalStop();
  CHECK(session.ReapChildren());
  session.Cleanup();
}

// A stalled role (frozen heartbeat and loop counter) is SIGTERMed, reaped and
// respawned; the respawn inherits the healthy environment once the test
// clears the stall trigger.
void TestStallRestart() {
  CHECK(setenv(kStallEnv, "1", 1) == 0);
  MpSupervisorConfig cfg = MakeConfig();
  cfg.max_restarts = 2;
  cfg.restart_backoff_initial_ns = 10ULL * 1000 * 1000;
  cfg.restart_backoff_max_ns = 40ULL * 1000 * 1000;
  cfg.restart_kill_grace_ns = 30ULL * 1000 * 1000;
  cfg.restart_stall_timeout_ns = 40ULL * 1000 * 1000;

  MpSupervisorSession session(cfg);
  CHECK(session.Prepare(nullptr));
  CHECK(session.ForkRoles("/proc/self/exe"));

  bool stall_trigger_cleared = false;
  uint64_t max_stall_age = 0;
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(2);
  for (;;) {
    session.PollRestarts();
    const MpRestartStats stats = session.restart_stats();
    if (stats.role[0].last_stall_age_ns > max_stall_age) {
      max_stall_age = stats.role[0].last_stall_age_ns;
    }
    // The child that is about to be respawned must inherit the healthy
    // environment, so clear the trigger as soon as the stall is detected.
    if (!stall_trigger_cleared &&
        stats.role[0].state == MpRoleState::kTerminating) {
      CHECK(unsetenv(kStallEnv) == 0);
      stall_trigger_cleared = true;
    }
    if (stats.role[0].restarts_succeeded >= 1 &&
        stats.role[0].state == MpRoleState::kRunning) {
      break;
    }
    if (std::chrono::steady_clock::now() >= deadline) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  CHECK(stall_trigger_cleared);
  CHECK(unsetenv(kStallEnv) == 0);

  MpRestartStats stats = session.restart_stats();
  CHECK(stats.role[0].restarts_attempted == 1);
  CHECK(stats.role[0].restarts_succeeded == 1);
  CHECK(stats.role[0].state == MpRoleState::kRunning);
  CHECK(stats.role[0].pid > 0);
  CHECK(stats.restarts_attempted == 1);
  CHECK(stats.restarts_succeeded == 1);
  CHECK(stats.restart_giveups == 0);
  // The old incarnation was terminated by the supervisor (SIGTERM) and the
  // stall age at detection exceeded the configured timeout.
  CHECK(stats.role[0].last_term_signal == SIGTERM);
  CHECK(max_stall_age >= 40ULL * 1000 * 1000);

  // The respawned, healthy child must not be restarted again.
  const auto settle = std::chrono::steady_clock::now() +
                      std::chrono::milliseconds(150);
  while (std::chrono::steady_clock::now() < settle) {
    session.PollRestarts();
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  stats = session.restart_stats();
  CHECK(stats.role[0].state == MpRoleState::kRunning);
  CHECK(stats.role[0].restarts_attempted == 1);

  session.SignalStop();
  CHECK(session.ReapChildren());
  session.Cleanup();
}

// Default policy (restart_enabled == false) preserves the old behaviour: the
// dead role stays dead and no restart counters move.
void TestRestartDisabledByDefault() {
  CHECK(setenv(kExitNowEnv, "1", 1) == 0);
  MpSupervisorConfig cfg = MakeConfig();
  cfg.restart_enabled = false;

  MpSupervisorSession session(cfg);
  CHECK(session.Prepare(nullptr));
  CHECK(session.ForkRoles("/proc/self/exe"));

  const bool stopped = PollUntil(
      session,
      [&]() {
        return session.restart_stats().role[0].state == MpRoleState::kStopped;
      },
      1000);
  const MpRestartStats stats = session.restart_stats();
  CHECK(unsetenv(kExitNowEnv) == 0);

  CHECK(stopped);
  CHECK(stats.role[0].state == MpRoleState::kStopped);
  CHECK(stats.role[0].restarts_attempted == 0);
  CHECK(stats.restarts_attempted == 0);
  CHECK(stats.restart_giveups == 0);

  session.SignalStop();
  CHECK(!session.ReapChildren());  // role exited 1 and was not respawned
  session.Cleanup();
}

// OPS-03: ForkRoles() announces READY=1 and PollRestarts() feeds WATCHDOG=1
// when the notify gate is enabled. The child roles inherit NOTIFY_SOCKET but
// never send to it.
void TestNotifyReadyAndWatchdog() {
  char path[128];
  std::snprintf(path, sizeof(path), "/tmp/thyme_sdnotify_%d.sock",
                static_cast<int>(getpid()));
  const int fd = ::socket(AF_UNIX, SOCK_DGRAM, 0);
  CHECK(fd >= 0);
  (void)::unlink(path);
  struct sockaddr_un addr {};
  addr.sun_family = AF_UNIX;
  CHECK(std::strlen(path) < sizeof(addr.sun_path));
  std::memcpy(addr.sun_path, path, std::strlen(path) + 1);
  CHECK(::bind(fd, reinterpret_cast<const struct sockaddr*>(&addr),
               sizeof(addr)) == 0);
  struct timeval tv {};
  tv.tv_sec = 2;
  CHECK(::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) == 0);

  CHECK(setenv("NOTIFY_SOCKET", path, 1) == 0);
  CHECK(setenv("THYME_SD_NOTIFY", "1", 1) == 0);

  MpSupervisorSession session(MakeConfig());
  CHECK(session.Prepare(nullptr));
  CHECK(session.ForkRoles("/proc/self/exe"));

  char buf[64];
  ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
  CHECK(n == 7 && std::memcmp(buf, "READY=1", 7) == 0);

  session.PollRestarts();
  n = ::recv(fd, buf, sizeof(buf), 0);
  CHECK(n == 10 && std::memcmp(buf, "WATCHDOG=1", 10) == 0);

  session.SignalStop();
  (void)session.ReapChildren();
  session.Cleanup();
  (void)::close(fd);
  (void)::unlink(path);
  CHECK(unsetenv("NOTIFY_SOCKET") == 0);
  CHECK(unsetenv("THYME_SD_NOTIFY") == 0);
}

}  // namespace

int main(int argc, char** argv) {
  if (argc >= 2 && std::strcmp(argv[1], "--role") == 0) {
    return RunChild(argc, argv);
  }
  TestImmediateExitGivesUp();
  TestBackoffSpacing();
  TestHealthyRoleNotRestarted();
  TestStallRestart();
  TestRestartDisabledByDefault();
  TestNotifyReadyAndWatchdog();
  return 0;
}
