#include <atomic>
#include <cstdio>
#include <string>
#include <thread>

#include "thyme/check.h"
#include "thyme/record/record_control.h"
#if defined(__linux__)
#include <dirent.h>
#endif
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

using namespace thyme;

namespace {

constexpr int kSkipExit = 77;

#if defined(__linux__)
int CountOpenFds() {
  DIR *dir = opendir("/proc/self/fd");
  CHECK(dir != nullptr);
  int n = 0;
  while (readdir(dir) != nullptr) ++n;  // includes "." and ".."
  closedir(dir);
  return n;
}
#endif

void EnsurePathAbsent(const char *path) {
  unlink(path);
  struct stat st {};
  if (stat(path, &st) == 0) {
    CHECK(unlink(path) == 0);
  }
}

bool PollForCmd(record_control::Server &srv,
                record_control::ControlCmd expected) {
  record_control::ControlCmd cmd = record_control::ControlCmd::kNone;
  for (int i = 0; i < 50; ++i) {
    if (srv.Poll(cmd)) {
      CHECK(cmd == expected);
      return true;
    }
    usleep(10000);
  }
  return false;
}

void TestOpenServerRejectsRegularFile(const char *path) {
  EnsurePathAbsent(path);
  const int fd = open(path, O_CREAT | O_WRONLY, 0600);
  CHECK(fd >= 0);
  close(fd);

  record_control::Server srv;
  CHECK(!srv.Open(path));
  CHECK(srv.AuthRejectCount() == 0);
  EnsurePathAbsent(path);
}

void TestOpenServerRejectsForeignOwnedSocket(const char *path) {
  if (geteuid() != 0) {
    std::fprintf(stderr,
                 "SKIP: foreign-owned socket test requires root (euid=%u)\n",
                 static_cast<unsigned>(geteuid()));
    return;
  }

  unlink(path);
  const int fd = socket(AF_UNIX, SOCK_DGRAM, 0);
  CHECK(fd >= 0);

  sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  std::snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path);
  CHECK(bind(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) == 0);
  CHECK(chown(path, 65534, getegid()) == 0);
  close(fd);

  record_control::Server srv;
  CHECK(!srv.Open(path));
  CHECK(srv.AuthRejectCount() == 0);
  unlink(path);
}

void TestCrossUidAuthReject(record_control::Server &srv) {
#if defined(__linux__) || defined(__APPLE__)
  const uid_t foreign_uid =
      (geteuid() == 0) ? 65534u : static_cast<uid_t>(geteuid() + 1);
  const uint64_t before = srv.AuthRejectCount();

  record_control::ControlCmd cmd = record_control::ControlCmd::kNone;
  CHECK(!srv.ProcessCommand("status", foreign_uid, cmd));
  CHECK(cmd == record_control::ControlCmd::kNone);
  CHECK(srv.AuthRejectCount() == before + 1);

  CHECK(!srv.ProcessCommand("arm", foreign_uid, cmd));
  CHECK(srv.AuthRejectCount() == before + 2);
#else
  (void)srv;
  std::fprintf(stderr,
               "SKIP: peer-credential auth not implemented on this OS\n");
#endif
}

void TestTriggerPrefixRejected(record_control::Server &srv) {
  record_control::ControlCmd cmd = record_control::ControlCmd::kNone;
  CHECK(!srv.ProcessCommand("triggery", geteuid(), cmd));
  CHECK(cmd == record_control::ControlCmd::kNone);
}

// Direct state-machine coverage of the trigger mode: arm/disarm transitions,
// trigger leaving the armed flag alone, phase transitions, and trigger-id
// allocation. Previously only exercised indirectly through recorder_cli.
void TestCommandStateMachine(record_control::Server &srv) {
  record_control::ControlCmd cmd = record_control::ControlCmd::kNone;
  const uid_t me = geteuid();
  using Phase = record_control::RecorderPhase;

  // Default state after Open: armed, phase armed, ids start at 1.
  CHECK(srv.IsArmed());
  CHECK(srv.CurrentPhase() == Phase::kArmed);
  CHECK(srv.NextTriggerId() == 1);
  CHECK(srv.NextTriggerId() == 2);

  // disarm persists; trigger is a command and must not re-arm.
  CHECK(srv.ProcessCommand("disarm", me, cmd));
  CHECK(cmd == record_control::ControlCmd::kDisarm);
  CHECK(!srv.IsArmed());
  CHECK(srv.ProcessCommand("trigger", me, cmd));
  CHECK(cmd == record_control::ControlCmd::kTrigger);
  CHECK(!srv.IsArmed());
  CHECK(srv.ProcessCommand("status", me, cmd));
  CHECK(cmd == record_control::ControlCmd::kStatus);
  CHECK(!srv.IsArmed());

  // arm restores armed.
  CHECK(srv.ProcessCommand("arm", me, cmd));
  CHECK(cmd == record_control::ControlCmd::kArm);
  CHECK(srv.IsArmed());

  // Phase transitions are orthogonal to the armed flag.
  srv.SetPhase(Phase::kPost);
  CHECK(srv.CurrentPhase() == Phase::kPost);
  CHECK(srv.IsArmed());
  srv.SetPhase(Phase::kSettle);
  CHECK(srv.CurrentPhase() == Phase::kSettle);
  srv.SetPhase(Phase::kArmed);
  CHECK(srv.CurrentPhase() == Phase::kArmed);

  // Unknown command: rejected, no state change.
  CHECK(!srv.ProcessCommand("bogus", me, cmd));
  CHECK(cmd == record_control::ControlCmd::kNone);
  CHECK(srv.IsArmed());
}

record_control::ControlRequest Request(record_control::ControlOp op,
                                       uint64_t sequence) {
  record_control::ControlRequest request;
  request.op = static_cast<uint16_t>(op);
  request.sequence = sequence;
  record_control::StampRequestCrc(&request);
  return request;
}

void TestV2Security(record_control::Server &srv, const char *path) {
  record_control::ControlCmd cmd = record_control::ControlCmd::kNone;
  record_control::ControlResponse response{};
  std::string payload;
  auto process = [&](record_control::ControlRequest &request) {
    return srv.ProcessRequest(reinterpret_cast<const uint8_t *>(&request),
                              sizeof(request), geteuid(), getpid(), cmd,
                              &response, &payload);
  };

  record_control::ControlRequest first =
      Request(record_control::ControlOp::kArm, record_control::NextSequence());
  CHECK(process(first));
  CHECK(response.code ==
        static_cast<uint16_t>(record_control::ControlCode::kAccepted));
  CHECK(response.crc32 == record_control::ResponseCrc(response));
  CHECK(srv.ProcessRequest(reinterpret_cast<const uint8_t *>(&first),
                           sizeof(first), geteuid(), getpid() + 1, cmd,
                           &response, &payload));
  CHECK(response.code ==
        static_cast<uint16_t>(record_control::ControlCode::kAccepted));
  const uint64_t replay_before = srv.ReplayRejectCount();
  CHECK(!process(first));
  CHECK(response.code ==
        static_cast<uint16_t>(record_control::ControlCode::kReplayRejected));
  CHECK(srv.ReplayRejectCount() == replay_before + 1);

  record_control::ControlRequest corrupted = Request(
      record_control::ControlOp::kDisarm, record_control::NextSequence());
  corrupted.crc32 ^= 1u;
  const uint64_t crc_before = srv.CrcRejectCount();
  CHECK(!process(corrupted));
  CHECK(response.magic == 0);
  CHECK(srv.CrcRejectCount() == crc_before + 1);
  record_control::StampRequestCrc(&corrupted);
  CHECK(process(corrupted));
  CHECK(!srv.IsArmed());

  CHECK(!process(first));
  CHECK(response.code ==
        static_cast<uint16_t>(record_control::ControlCode::kReplayRejected));

  record_control::Config denied;
  denied.allowed_uids = {static_cast<uint32_t>(geteuid()) + 1u};
  CHECK(srv.ReloadConfig(denied));
  record_control::ControlRequest auth = Request(
      record_control::ControlOp::kStatus, record_control::NextSequence());
  const uint64_t auth_before = srv.AuthRejectCount();
  CHECK(!process(auth));
  CHECK(srv.AuthRejectCount() == auth_before + 1);
  CHECK(response.magic == 0);
  CHECK(srv.ReloadConfig(record_control::Config{}));

  const char legacy[] = "status";
  CHECK(!srv.ProcessRequest(reinterpret_cast<const uint8_t *>(legacy),
                            sizeof(legacy) - 1, geteuid(), getpid(), cmd,
                            &response, &payload));
  CHECK(response.code == static_cast<uint16_t>(
                             record_control::ControlCode::kUnsupportedVersion));

  char self_path[64];
  std::snprintf(self_path, sizeof(self_path), "/tmp/thyme_legacy_%d.sock",
                static_cast<int>(getpid()));
  unlink(self_path);
  const int client = socket(AF_UNIX, SOCK_DGRAM, 0);
  CHECK(client >= 0);
  sockaddr_un self{};
  self.sun_family = AF_UNIX;
  std::snprintf(self.sun_path, sizeof(self.sun_path), "%s", self_path);
  CHECK(bind(client, reinterpret_cast<sockaddr *>(&self), sizeof(self)) == 0);
  sockaddr_un target{};
  target.sun_family = AF_UNIX;
  std::snprintf(target.sun_path, sizeof(target.sun_path), "%s", path);
  const char old_query[] = "status-json";
  CHECK(sendto(client, old_query, sizeof(old_query) - 1, 0,
               reinterpret_cast<sockaddr *>(&target),
               sizeof(target)) == static_cast<ssize_t>(sizeof(old_query) - 1));
  char old_reply[256];
  ssize_t old_size = -1;
  for (int i = 0; i < 50 && old_size < 0; ++i) {
    srv.Poll(cmd);
    old_size = recv(client, old_reply, sizeof(old_reply) - 1, MSG_DONTWAIT);
    usleep(1000);
  }
  CHECK(old_size > 0);
  old_reply[old_size] = '\0';
  CHECK(std::string(old_reply).find("unsupported_version") !=
        std::string::npos);
  close(client);
  unlink(self_path);

  record_control::Config legacy_cfg;
  legacy_cfg.allow_legacy_control = true;
  CHECK(srv.ReloadConfig(legacy_cfg));
  CHECK(srv.ProcessCommand("status", geteuid(), cmd));
  CHECK(cmd == record_control::ControlCmd::kStatus);
  CHECK(srv.LegacyRequestCount() >= 1);
  CHECK(srv.ReloadConfig(record_control::Config{}));

  const std::string audit_path = std::string(path) + ".audit";
  std::FILE *audit = std::fopen(audit_path.c_str(), "rb");
  CHECK(audit != nullptr);
  std::string contents;
  char line[1024];
  while (std::fgets(line, sizeof(line), audit) != nullptr) contents += line;
  std::fclose(audit);
  CHECK(contents.find("reason=bad_crc") != std::string::npos);
  CHECK(contents.find("reason=replay_duplicate") != std::string::npos);
  CHECK(contents.find("reason=auth_uid_not_allowed") != std::string::npos);
  CHECK(contents.find("reason=legacy_control_disabled") != std::string::npos);
  CHECK(contents.find("proto=1") != std::string::npos);
}

// End-to-end leg: the datagram path must drive the same state machine.
void TestDisarmArmViaSocket(record_control::Server &srv, const char *path) {
  CHECK(record_control::SendClientCommand(path, "disarm"));
  CHECK(PollForCmd(srv, record_control::ControlCmd::kDisarm));
  CHECK(!srv.IsArmed());
  CHECK(record_control::SendClientCommand(path, "arm"));
  CHECK(PollForCmd(srv, record_control::ControlCmd::kArm));
  CHECK(srv.IsArmed());
}

void TestCrossUidSendIfPossible(record_control::Server &srv, const char *path) {
#if !(defined(__linux__) || defined(__APPLE__))
  (void)srv;
  (void)path;
  return;
#else
  const uid_t euid = geteuid();
  const uid_t target = (euid > 0) ? (euid - 1) : (euid + 1);
  const uint64_t before = srv.AuthRejectCount();

  // The socket is 0600 (owner-only), so a root-spawned child that setuid()s
  // to a non-owner uid cannot even sendto() it and the datagram never reaches
  // the auth path. Temporarily relax the mode so the cross-uid datagram is
  // delivered and rejected by peer-credential auth, then restore 0600.
  const bool relax = (euid == 0);
  if (relax) CHECK(chmod(path, 0666) == 0);

  pid_t pid = fork();
  CHECK(pid >= 0);
  if (pid == 0) {
    if (setuid(target) != 0) _exit(kSkipExit);
    const bool sent = record_control::SendClientCommand(path, "status");
    _exit(sent ? 0 : 1);
  }

  int st = 0;
  CHECK(waitpid(pid, &st, 0) == pid);
  if (relax) CHECK(chmod(path, 0600) == 0);
  if (!WIFEXITED(st)) return;
  const int code = WEXITSTATUS(st);
  if (code == kSkipExit) {
    std::fprintf(stderr,
                 "SKIP: cross-uid send (setuid to %u failed without root)\n",
                 static_cast<unsigned>(target));
    return;
  }
  CHECK(code == 0);

  record_control::ControlCmd cmd = record_control::ControlCmd::kNone;
  for (int i = 0; i < 50; ++i) {
    srv.Poll(cmd);
    if (srv.AuthRejectCount() > before) break;
    usleep(10000);
  }
  CHECK(srv.AuthRejectCount() == before + 1);
  CHECK(cmd == record_control::ControlCmd::kNone);
#endif
}

void TestReopenDoesNotLeakFd(record_control::Server &srv, const char *path2) {
#if defined(__linux__)
  const int before = CountOpenFds();
  EnsurePathAbsent(path2);
  CHECK(srv.Open(path2));
  // Re-opening must close the previous socket: fd count unchanged.
  CHECK(CountOpenFds() == before);
  // The old path this process bound must be removed; the new one must work.
  CHECK(record_control::SendClientCommand(path2, "status"));
  CHECK(PollForCmd(srv, record_control::ControlCmd::kStatus));
#else
  (void)srv;
  (void)path2;
#endif
}

// ECO-02: the server answers `status`/`status-json` on the sender's bound
// address. A poll thread drives Server::Poll while the client waits.
void TestStatusQuery(record_control::Server &srv, const char *path) {
#if defined(__linux__) || defined(__APPLE__)
  record_control::StatusSnapshot snapshot;
  snapshot.captures_done = 5;
  snapshot.slices_done = 2;
  snapshot.pending_bytes = 4096;
  snapshot.disk_full = true;
  snapshot.triggers_rejected = 6;
  snapshot.descriptors_queued = 7;
  snapshot.pending_evicted = 8;
  snapshot.overrun_before_queue = 9;
  snapshot.samples_written = 10;
  snapshot.frames_written = 11;
  snapshot.holes_written = 12;
  snapshot.torn_reads = 13;
  snapshot.harvest_retries = 14;
  snapshot.session_reattaches = 15;
  snapshot.source_errors = 16;
  snapshot.disk_errors = 17;
  snapshot.sync_errors = 18;
  snapshot.audit_drops = 19;
  snapshot.fsm_event_drops = 20;
  snapshot.service_records_written = 21;
  snapshot.service_records_suppressed = 22;
  snapshot.service_records_dropped = 23;
  snapshot.service_records_truncated = 24;
  snapshot.service_rotations = 25;
  snapshot.service_sink_open = true;
  snapshot.integrity_mode = integrity::ManifestMode::kMixed;
  srv.SetStatusSnapshot(snapshot);
  CHECK(srv.StatusText().find("captures_done=5") != std::string::npos);
  CHECK(srv.StatusText().find("integrity_mode=mixed") != std::string::npos);
  CHECK(srv.StatusJson().find("\"integrity_mode\":\"mixed\"") !=
        std::string::npos);
  CHECK(srv.StatusJson().find("\"kind\":\"thyme-recorder-status\"") !=
        std::string::npos);

  std::atomic<bool> stop{false};
  std::thread poller([&] {
    while (!stop.load(std::memory_order_acquire)) {
      record_control::ControlCmd cmd;
      while (srv.Poll(cmd)) {
      }
      usleep(1000);
    }
  });

  std::string reply;
  std::string error;
  const bool json_ok =
      record_control::QueryRecorderStatus(path, true, 2000, &reply, &error);
  CHECK(json_ok);
  CHECK(error.empty());
  // The length-delimited V2 payload must be complete JSON without a NUL byte
  // appended to the wire; termination remains a local buffer concern only.
  CHECK(!reply.empty());
  CHECK(reply.back() == '}');
  CHECK(reply.find('\0') == std::string::npos);
  CHECK(reply.find("\"schema\":1") != std::string::npos);
  CHECK(reply.find("\"kind\":\"thyme-recorder-status\"") != std::string::npos);
  CHECK(reply.find("\"armed\":true") != std::string::npos);
  CHECK(reply.find("\"captures_done\":5") != std::string::npos);
  CHECK(reply.find("\"disk_full\":true") != std::string::npos);
  CHECK(reply.find("\"integrity_mode\":\"mixed\"") != std::string::npos);
  CHECK(reply.find("\"disk_errors\":17") != std::string::npos);
  CHECK(reply.find("\"sync_errors\":18") != std::string::npos);
  CHECK(reply.find("\"audit_drops\":19") != std::string::npos);
  CHECK(reply.find("\"fsm_event_drops\":20") != std::string::npos);
  CHECK(reply.find("\"service_sink_open\":true") != std::string::npos);
  CHECK(reply.find("\"service_records_written\":21") != std::string::npos);
  CHECK(reply.find("\"service_records_suppressed\":22") != std::string::npos);
  CHECK(reply.find("\"service_records_dropped\":23") != std::string::npos);
  CHECK(reply.find("\"service_records_truncated\":24") != std::string::npos);
  CHECK(reply.find("\"service_rotations\":25") != std::string::npos);

  std::string text;
  const bool text_ok =
      record_control::QueryRecorderStatus(path, false, 2000, &text, &error);
  CHECK(text_ok);
  CHECK(text.find("captures_done=5") != std::string::npos);
  CHECK(text.find("disk_full=true") != std::string::npos);

  stop.store(true, std::memory_order_release);
  poller.join();
#else
  (void)srv;
  (void)path;
#endif
}

}  // namespace

int main() {
#if defined(_WIN32)
  return 0;
#else
  char path[64];
  std::snprintf(path, sizeof(path), "/tmp/thyme_ctl_%d.sock",
                static_cast<int>(getpid()));
  EnsurePathAbsent(path);

  TestOpenServerRejectsRegularFile(path);
  TestOpenServerRejectsForeignOwnedSocket(path);

  record_control::Server srv;
  CHECK(srv.Open(path));
  struct stat st {};
  CHECK(stat(path, &st) == 0);
  CHECK(S_ISSOCK(st.st_mode));
  CHECK((st.st_mode & 0777) == 0600);

  CHECK(record_control::SendClientCommand(path, "status"));
  CHECK(PollForCmd(srv, record_control::ControlCmd::kStatus));
  CHECK(srv.AuthRejectCount() == 0);

  TestCrossUidAuthReject(srv);
  TestCrossUidSendIfPossible(srv, path);
  TestTriggerPrefixRejected(srv);
  record_control::Config legacy_cfg;
  legacy_cfg.allow_legacy_control = true;
  CHECK(srv.ReloadConfig(legacy_cfg));
  TestCommandStateMachine(srv);
  CHECK(srv.ReloadConfig(record_control::Config{}));
  TestV2Security(srv, path);
  TestDisarmArmViaSocket(srv, path);
  TestStatusQuery(srv, path);

  char path2[64];
  std::snprintf(path2, sizeof(path2), "/tmp/thyme_ctl2_%d.sock",
                static_cast<int>(getpid()));
  TestReopenDoesNotLeakFd(srv, path2);

  srv.Close();
  CHECK(stat(path2, &st) != 0);
  EnsurePathAbsent(path);
  EnsurePathAbsent((std::string(path) + ".audit").c_str());
  EnsurePathAbsent((std::string(path2) + ".audit").c_str());
  return 0;
#endif
}
