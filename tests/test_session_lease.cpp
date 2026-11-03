// ARCH-14: topology session lease. A live second writer gets kBusy and does
// not replace the holder. A dead holder is reclaimed as kStolen (the
// restarted recorder reattaches the same page). Channel membership is the
// view over ProducerGate: same pid+start, or an unheld gate.

#include "thyme/check.h"
#include "thyme/record/triggered_recorder.h"
#include "thyme/runtime/pinned_slot_store.h"
#include "thyme/runtime/session_lease.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

using namespace thyme;

namespace {

void TestSameProcessBusy() {
  char name[64];
  std::snprintf(name, sizeof(name), "/tsess_busy_%ld",
                static_cast<long>(getpid()));
  ShmRegion::UnlinkName(name);
  ShmRegion first;
  ShmRegion second;
  uint64_t token_a = 0;
  uint64_t token_b = 0;
  const uint64_t now = ProducerGateNowNs();
  CHECK(SessionLeaseAcquire(&first, name, SessionRole::kRecorder, 11, now, 0,
                            &token_a) == SessionLeaseResult::kOk);
  CHECK(token_a != 0);
  auto* page = static_cast<SessionLeasePage*>(first.data());
  CHECK(page->session_id.load() == 11);
  CHECK(page->role.load() == static_cast<uint32_t>(SessionRole::kRecorder));
  CHECK(SessionLeaseAcquire(&second, name, SessionRole::kReplay, 22, now, 0,
                            &token_b) == SessionLeaseResult::kBusy);
  CHECK(token_b == 0);
  CHECK(second.data() == nullptr);
  CHECK(page->owner_token.load() == token_a);
  CHECK(page->session_id.load() == 11);

  CHECK(SessionLeaseJoin(page, token_a, 7) == SessionLeaseResult::kOk);
  CHECK(SessionLeaseJoin(page, token_a, 7) == SessionLeaseResult::kOk);
  CHECK(page->member_count.load() == 1);
  ProducerGate gate;
  CHECK(ProducerGateAcquire(gate, ProducerGateNewToken(), now, 0) ==
        GateResult::kOk);
  CHECK(SessionLeaseGateMatches(*page, 7, gate));
  ProducerGate other;
  other.owner_token.store(9);
  other.owner_pid.store(1);
  other.owner_start.store(1);
  CHECK(!SessionLeaseGateMatches(*page, 7, other));
  CHECK(!SessionLeaseGateMatches(*page, 8, gate));
  CHECK(SessionLeaseLeave(page, token_b, 7) == SessionLeaseResult::kNotHeld);
  CHECK(SessionLeaseLeave(page, token_a, 7) == SessionLeaseResult::kOk);
  CHECK(!SessionLeaseGateMatches(*page, 7, gate));

  for (uint32_t id = 0; id < kSessionLeaseMaxMembers; ++id) {
    CHECK(SessionLeaseJoin(page, token_a, id) == SessionLeaseResult::kOk);
  }
  CHECK(SessionLeaseJoin(page, token_a, 1000) == SessionLeaseResult::kFull);
  CHECK(SessionLeaseHeartbeat(page, token_a, now + 1));
  CHECK(!SessionLeaseHeartbeat(page, token_b, now + 1));

  CHECK(SessionLeaseRelease(page, token_a) == SessionLeaseResult::kOk);
  CHECK(page->owner_token.load() == kSessionLeaseUnheld);
  CHECK(SessionLeaseAcquire(&second, name, SessionRole::kDriver, 33, now, 0,
                            &token_b) == SessionLeaseResult::kOk);
  CHECK(static_cast<SessionLeasePage*>(second.data())->session_id.load() == 33);
  CHECK(SessionLeaseRelease(static_cast<SessionLeasePage*>(second.data()),
                            token_b) == SessionLeaseResult::kOk);
  second.Unmap();
  first.Unmap();
  ShmRegion::UnlinkName(name);
}

void TestCrashReattach() {
  char name[64];
  std::snprintf(name, sizeof(name), "/tsess_crash_%ld",
                static_cast<long>(getpid()));
  ShmRegion::UnlinkName(name);
  int sv[2];
  CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
  const pid_t pid = fork();
  CHECK(pid >= 0);
  if (pid == 0) {
    close(sv[0]);
    ShmRegion region;
    uint64_t token = 0;
    const SessionLeaseResult got = SessionLeaseAcquire(
        &region, name, SessionRole::kRecorder, 42, ProducerGateNowNs(), 0,
        &token);
    if (got != SessionLeaseResult::kOk) _exit(2);
    const char ready = 'A';
    if (write(sv[1], &ready, 1) != 1) _exit(3);
    char go = 0;
    if (read(sv[1], &go, 1) != 1) _exit(4);
    _exit(0);
  }
  close(sv[1]);
  char ready = 0;
  CHECK(read(sv[0], &ready, 1) == 1 && ready == 'A');
  ShmRegion parent;
  uint64_t token = 0;
  const uint64_t now = ProducerGateNowNs();
  CHECK(SessionLeaseAcquire(&parent, name, SessionRole::kReplay, 99, now, 0,
                            &token) == SessionLeaseResult::kBusy);
  CHECK(token == 0);
  const char go = 'B';
  CHECK(write(sv[0], &go, 1) == 1);
  int status = 0;
  CHECK(waitpid(pid, &status, 0) == pid);
  CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
  close(sv[0]);
  CHECK(SessionLeaseAcquire(&parent, name, SessionRole::kRecorder, 100, now, 0,
                            &token) == SessionLeaseResult::kStolen);
  auto* page = static_cast<SessionLeasePage*>(parent.data());
  CHECK(page->owner_pid.load() == static_cast<uint64_t>(getpid()));
  CHECK(page->session_id.load() == 100);
  CHECK(page->steals.load() == 1);
  CHECK(SessionLeaseRelease(page, token) == SessionLeaseResult::kOk);
  parent.Unmap();
  ShmRegion::UnlinkName(name);
}

void TestRecorderRejectsSecondWriter() {
  const std::string shm =
      "/arch14bf_" + std::to_string(static_cast<long>(getpid()));
  const std::string dir =
      "/tmp/thyme_arch14_" + std::to_string(static_cast<long>(getpid()));
  PinnedSlotWriter writer;
  CHECK(writer.Create(shm.c_str(), 64, 4, 0));
  triggered::Config cfg;
  triggered::SourceConfig source;
  source.kind = triggered::SourceConfig::Kind::kPinnedSlot;
  source.shm_name = shm;
  source.channel_name = "cam";
  cfg.sources.push_back(source);
  cfg.out_dir = dir;
  triggered::TriggeredRecorder first;
  triggered::TriggeredRecorder second;
  CHECK(first.Init(cfg));
  CHECK(first.init_lease_result() == SessionLeaseResult::kOk);
  CHECK(!second.Init(cfg));
  CHECK(second.init_lease_result() == SessionLeaseResult::kBusy);
  first.Shutdown();
  CHECK(second.Init(cfg));
  CHECK(second.init_lease_result() == SessionLeaseResult::kOk);
  second.Shutdown();
  writer.Close();
  ShmRegion::UnlinkName(shm.c_str());
  char lease[64];
  CHECK(SessionLeaseNameForRoot(dir.c_str(), lease, sizeof(lease)));
  ShmRegion::UnlinkName(lease);
  rmdir(dir.c_str());
}

}  // namespace

int main() {
  TestSameProcessBusy();
  TestCrashReattach();
  TestRecorderRejectsSecondWriter();
  return 0;
}
