// OPS-10: read-only JSON status endpoint. Covers the protocol contract over a
// real AF_UNIX stream socket (GET /status, /status?role=, /health), snapshot
// publish/read, malformed/oversized request rejection, slow-client isolation
// and clean shutdown (socket unlink), plus the MpSupervisorSession wiring
// (Prepare starts from config, PublishStatus fills role state, Cleanup stops).
#include "thyme/check.h"
#include "thyme/app/mp_supervisor.h"
#include "thyme/app/status_server.h"

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <poll.h>
#include <string>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

namespace {

using thyme::app::MpNode;
using thyme::app::MpProcess;
using thyme::app::MpSupervisorConfig;
using thyme::app::MpSupervisorSession;
using thyme::app::StatusRoleSnapshot;
using thyme::app::StatusServer;
using thyme::app::StatusSnapshot;

std::string TempPath(const char* tag) {
  char buf[128];
  std::snprintf(buf, sizeof(buf), "/tmp/thyme_status_%d_%s.sock",
                static_cast<int>(getpid()), tag);
  return std::string(buf);
}

int Connect(const char* path) {
  const int fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) return -1;
  struct sockaddr_un addr {};
  addr.sun_family = AF_UNIX;
  if (std::strlen(path) >= sizeof(addr.sun_path)) {
    (void)close(fd);
    return -1;
  }
  std::snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path);
  if (connect(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) !=
      0) {
    (void)close(fd);
    return -1;
  }
  return fd;
}

bool SendAll(int fd, const char* data, std::size_t len) {
  std::size_t sent = 0;
  while (sent < len) {
    const ssize_t n = send(fd, data + sent, len - sent, MSG_NOSIGNAL);
    if (n > 0) {
      sent += static_cast<std::size_t>(n);
    } else if (n < 0 && errno == EINTR) {
      continue;
    } else {
      return false;
    }
  }
  return true;
}

bool ReadAll(int fd, std::string* out, int timeout_ms) {
  const auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::milliseconds(timeout_ms);
  for (;;) {
    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline) return false;
    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                               deadline - now)
                               .count();
    struct pollfd pfd {};
    pfd.fd = fd;
    pfd.events = POLLIN;
    const int rc = poll(&pfd, 1, static_cast<int>(remaining));
    if (rc == 0) return false;
    if (rc < 0) {
      if (errno == EINTR) continue;
      return false;
    }
    char buf[1024];
    const ssize_t n = recv(fd, buf, sizeof(buf), 0);
    if (n > 0) {
      out->append(buf, static_cast<std::size_t>(n));
    } else if (n == 0) {
      return true;
    } else if (errno == EINTR) {
      continue;
    } else {
      return false;
    }
  }
}

std::string Request(const char* path, const char* request_line,
                    int timeout_ms = 2000) {
  const int fd = Connect(path);
  if (fd < 0) return std::string("CONNECT_FAILED");
  if (!SendAll(fd, request_line, std::strlen(request_line))) {
    (void)close(fd);
    return std::string("SEND_FAILED");
  }
  std::string raw;
  const bool ok = ReadAll(fd, &raw, timeout_ms);
  (void)close(fd);
  if (!ok) return raw.empty() ? std::string("TIMEOUT") : raw;
  return raw;
}

std::string BodyOf(const std::string& raw) {
  const std::size_t pos = raw.find("\r\n\r\n");
  if (pos == std::string::npos) return std::string();
  return raw.substr(pos + 4);
}

bool Contains(const std::string& haystack, const char* needle) {
  return haystack.find(needle) != std::string::npos;
}

// Minimal structural JSON check: balanced braces/brackets, closed strings,
// first/last non-space characters are {/}. Enough to catch a truncated or
// malformed status document without pulling in a JSON dependency.
bool LooksLikeJson(const std::string& s) {
  std::size_t begin = 0;
  while (begin < s.size() && (s[begin] == ' ' || s[begin] == '\n')) ++begin;
  std::size_t end = s.size();
  while (end > begin && (s[end - 1] == ' ' || s[end - 1] == '\n')) --end;
  if (end - begin < 2 || s[begin] != '{' || s[end - 1] != '}') return false;

  int depth = 0;
  bool in_string = false;
  bool escaped = false;
  for (std::size_t i = begin; i < end; ++i) {
    const char c = s[i];
    if (in_string) {
      if (escaped) {
        escaped = false;
      } else if (c == '\\') {
        escaped = true;
      } else if (c == '"') {
        in_string = false;
      } else if (static_cast<unsigned char>(c) < 0x20) {
        return false;
      }
      continue;
    }
    if (c == '"') {
      in_string = true;
    } else if (c == '{' || c == '[') {
      ++depth;
    } else if (c == '}' || c == ']') {
      --depth;
      if (depth < 0) return false;
    }
  }
  return depth == 0 && !in_string;
}

void ExpectStatus(const std::string& response, const char* needle) {
  const std::string body = BodyOf(response);
  if (!Contains(response, "HTTP/1.1 200") || !LooksLikeJson(body) ||
      !Contains(body, needle)) {
    std::fprintf(stderr, "unexpected response:\n%s\n", response.c_str());
    CHECK(false);
  }
}

StatusSnapshot MakeSnapshot() {
  StatusSnapshot snap;
  std::snprintf(snap.session_id, sizeof(snap.session_id), "c9_4241");
  std::snprintf(snap.build, sizeof(snap.build), "thyme test");
  snap.uptime_ns = 1500ULL * 1000 * 1000;
  snap.n_roles = 2;
  StatusRoleSnapshot& a = snap.roles[0];
  std::snprintf(a.id, sizeof(a.id), "perception");
  std::snprintf(a.role, sizeof(a.role), "perception");
  a.state = 1;  // running
  a.pid = 4242;
  a.restarts_attempted = 2;
  a.restarts_succeeded = 1;
  a.last_exit_code = 0;
  a.last_term_signal = 0;
  a.last_stall_age_ns = 250ULL * 1000 * 1000;
  a.loops = 900;
  a.alive = 900;
  a.deadline_misses = 3;
  a.drops = 4;
  a.errors = 0;
  a.sched_mismatches = 2;
  a.n_health_errors = 1;
  std::snprintf(a.health_errors[0].node, sizeof(a.health_errors[0].node),
                "camera");
  a.health_errors[0].error_code = 0;
  a.n_health_drops = 1;
  std::snprintf(a.health_drops[0].edge, sizeof(a.health_drops[0].edge),
                "camera");
  a.health_drops[0].drops = 4;
  a.memory_flags = thyme::kMemoryMlockDisabled;
  a.n_health_memory = 1;
  std::snprintf(a.health_memory[0].node, sizeof(a.health_memory[0].node),
                "camera");
  a.health_memory[0].flags = thyme::kMemoryMlockDisabled;
  StatusRoleSnapshot& b = snap.roles[1];
  std::snprintf(b.id, sizeof(b.id), "rest");
  std::snprintf(b.role, sizeof(b.role), "rest");
  b.state = 4;  // gave_up
  b.pid = 0;
  b.drops = 7;
  b.errors = 1;
  b.n_health_errors = 1;
  std::snprintf(b.health_errors[0].node, sizeof(b.health_errors[0].node),
                "worker");
  b.health_errors[0].error_code = 37;
  b.n_health_drops = 1;
  std::snprintf(b.health_drops[0].edge, sizeof(b.health_drops[0].edge),
                "worker");
  b.health_drops[0].drops = 7;
  b.memory_flags = thyme::kMemoryMlockFailed;
  b.n_health_memory = 1;
  std::snprintf(b.health_memory[0].node, sizeof(b.health_memory[0].node),
                "worker");
  b.health_memory[0].flags = thyme::kMemoryMlockFailed;
  snap.healthy = false;
  return snap;
}

void TestProtocol() {
  const std::string path = TempPath("protocol");
  (void)unlink(path.c_str());
  StatusServer server;
  StatusServer::Config cfg;
  cfg.socket_path = path.c_str();
  cfg.client_timeout_ns = 500ULL * 1000 * 1000;
  CHECK(server.Start(cfg));
  CHECK(server.running());

  struct stat st {};
  CHECK(stat(path.c_str(), &st) == 0);
  CHECK(S_ISSOCK(st.st_mode));
  CHECK((st.st_mode & 0777) == 0600);

  CHECK(server.Publish(MakeSnapshot()));

  const std::string full = Request(path.c_str(), "GET /status\n");
  ExpectStatus(full, "\"role_count\":2");
  const std::string full_body = BodyOf(full);
  CHECK(Contains(full_body, "\"session_id\":\"c9_4241\""));
  CHECK(Contains(full_body, "\"build\":\"thyme test\""));
  CHECK(Contains(full_body, "\"uptime_ms\":1500"));
  CHECK(Contains(full_body, "\"state\":\"running\""));
  CHECK(Contains(full_body, "\"pid\":4242"));
  CHECK(Contains(full_body, "\"state\":\"gave_up\""));
  CHECK(Contains(full_body, "\"restarts_attempted\":2"));
  CHECK(Contains(full_body, "\"stall_age_ms\":250"));
  CHECK(Contains(full_body, "\"deadline_misses\":3"));
  CHECK(Contains(full_body, "\"drops\":7"));
  CHECK(Contains(full_body, "\"errors\":1"));
  // SCH-08: requested-vs-actual scheduling mismatch count surfaces per role.
  CHECK(Contains(full_body, "\"sched_mismatches\":2"));
  CHECK(Contains(full_body, "\"error_codes\":{\"camera\":0}"));
  CHECK(Contains(full_body, "\"error_codes\":{\"worker\":37}"));
  CHECK(Contains(full_body, "\"drop_counts\":{\"camera\":4}"));
  CHECK(Contains(full_body, "\"drop_counts\":{\"worker\":7}"));
  CHECK(Contains(full_body,
                "\"memory\":{\"flags\":1,\"state\":\"disabled\",\"workers\":"
                "{\"camera\":{\"flags\":1,\"state\":\"disabled\"}}"));
  CHECK(Contains(full_body,
                "\"memory\":{\"flags\":2,\"state\":\"degraded\",\"workers\":"
                "{\"worker\":{\"flags\":2,\"state\":\"degraded\"}}"));
  CHECK(Contains(full_body, "\"recorder\":{\"present\":false"));
  std::printf("captured GET /status:\n%s\n", full_body.c_str());

  // Per-role query by name, role string and index.
  const std::string by_name = Request(path.c_str(), "GET /status?role=rest\n");
  ExpectStatus(by_name, "\"role\":\"rest\"");
  CHECK(!Contains(BodyOf(by_name), "perception"));
  const std::string by_index = Request(path.c_str(), "GET /status?role=0\n");
  ExpectStatus(by_index, "\"id\":\"perception\"");
  CHECK(!Contains(BodyOf(by_index), "\"rest\""));

  const std::string missing =
      Request(path.c_str(), "GET /status?role=nope\n");
  CHECK(Contains(missing, "HTTP/1.1 404"));
  CHECK(Contains(BodyOf(missing), "unknown role"));

  const std::string post = Request(path.c_str(), "POST /status\n");
  CHECK(Contains(post, "HTTP/1.1 405"));
  CHECK(Contains(BodyOf(post), "read-only"));

  // /health follows the published health bit.
  const std::string bad = Request(path.c_str(), "GET /health\n");
  CHECK(Contains(bad, "HTTP/1.1 200"));
  CHECK(BodyOf(bad) == "{\"ok\":false}\n");

  StatusSnapshot healthy = MakeSnapshot();
  healthy.healthy = true;
  CHECK(server.Publish(healthy));
  const std::string good = Request(path.c_str(), "GET /health\n");
  CHECK(BodyOf(good) == "{\"ok\":true}\n");

  // An oversized line is rejected without crashing; the server keeps serving.
  const int oversized_fd = Connect(path.c_str());
  CHECK(oversized_fd >= 0);
  const std::string huge(2000, 'A');
  CHECK(SendAll(oversized_fd, huge.data(), huge.size()));
  std::string huge_resp;
  CHECK(ReadAll(oversized_fd, &huge_resp, 2000));
  (void)close(oversized_fd);
  CHECK(Contains(huge_resp, "HTTP/1.1 400"));
  CHECK(Contains(BodyOf(huge_resp), "request too long"));
  CHECK(server.rejected_oversized() == 1);

  const std::string after = Request(path.c_str(), "GET /health\n");
  CHECK(BodyOf(after) == "{\"ok\":true}\n");

  // Clean shutdown unlinks the socket path.
  server.Stop();
  CHECK(!server.running());
  CHECK(access(path.c_str(), F_OK) != 0);
}

void TestSlowClientsDoNotBlockFastClient() {
  const std::string path = TempPath("slow");
  (void)unlink(path.c_str());
  StatusServer server;
  StatusServer::Config cfg;
  cfg.socket_path = path.c_str();
  cfg.max_clients = 4;
  cfg.client_timeout_ns = 150ULL * 1000 * 1000;
  CHECK(server.Start(cfg));
  StatusSnapshot snap = MakeSnapshot();
  snap.healthy = true;
  CHECK(server.Publish(snap));

  const int slow1 = Connect(path.c_str());
  const int slow2 = Connect(path.c_str());
  CHECK(slow1 >= 0);
  CHECK(slow2 >= 0);
  CHECK(SendAll(slow1, "GET /sta", 8));  // partial line, never terminated
  CHECK(SendAll(slow2, "GET /", 5));

  const auto start = std::chrono::steady_clock::now();
  const std::string fast = Request(path.c_str(), "GET /health\n", 2000);
  const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::steady_clock::now() - start)
                           .count();
  CHECK(BodyOf(fast) == "{\"ok\":true}\n");
  CHECK(elapsed < 500);  // a stuck peer must not serialize the fast request

  // The idle/partial clients are cut off at their deadline and never leave
  // the server half-broken.
  std::string discarded;
  CHECK(ReadAll(slow1, &discarded, 2000));
  CHECK(ReadAll(slow2, &discarded, 2000));
  (void)close(slow1);
  (void)close(slow2);
  CHECK(server.timed_out_clients() >= 1);
  CHECK(BodyOf(Request(path.c_str(), "GET /health\n")) == "{\"ok\":true}\n");
  server.Stop();
}

void TestConnectionCapIsCounted() {
  // ARCH-05 regression: a connection shed at max_clients must be counted, not
  // silently closed. Four connects against a one-slot server guarantees at
  // least three sheds regardless of accept ordering.
  const std::string path = TempPath("cap");
  (void)unlink(path.c_str());
  StatusServer server;
  StatusServer::Config cfg;
  cfg.socket_path = path.c_str();
  cfg.max_clients = 1;
  cfg.client_timeout_ns = 2000ULL * 1000 * 1000;
  CHECK(server.Start(cfg));
  CHECK(server.rejected_clients() == 0);

  int fds[4];
  for (int i = 0; i < 4; ++i) fds[i] = Connect(path.c_str());
  for (int i = 0; i < 4; ++i) CHECK(fds[i] >= 0);

  const auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::milliseconds(2000);
  while (server.rejected_clients() == 0 &&
         std::chrono::steady_clock::now() < deadline) {
    struct timespec req {0, 2L * 1000 * 1000};
    nanosleep(&req, nullptr);
  }
  CHECK(server.rejected_clients() >= 1);

  for (int i = 0; i < 4; ++i) (void)close(fds[i]);
  server.Stop();
}

void TestSupervisorWiring() {
  const std::string path = TempPath("supervisor");
  (void)unlink(path.c_str());
  const MpProcess procs[] = {{"worker", "worker"}};
  const MpNode nodes[] = {{"worker", "tick", "worker", 10}};
  MpSupervisorConfig cfg;
  cfg.processes = procs;
  cfg.nodes = nodes;
  cfg.health_n = 1;
  cfg.status_socket_path = path.c_str();

  {
    MpSupervisorSession session(cfg);
    CHECK(session.Prepare(nullptr));
    CHECK(session.status_server()->running());
    const thyme::SharedHealthPage* health = session.health_page();
    CHECK(health != nullptr);
    const_cast<thyme::HealthSlot&>(health->slots[0]).SetError(37);
    const_cast<thyme::HealthSlot&>(health->slots[0]).AddDrops(5);
    const_cast<thyme::HealthSlot&>(health->slots[0]).SetMemoryFlags(
        thyme::kMemoryMlockDisabled);
    session.PublishStatus();

    const std::string status = Request(path.c_str(), "GET /status\n");
    ExpectStatus(status, "\"role_count\":1");
    const std::string body = BodyOf(status);
    CHECK(Contains(body, "\"id\":\"worker\""));
    CHECK(Contains(body, "\"state\":\"not_started\""));
    CHECK(Contains(body, "\"session_id\":\"c9_"));
    CHECK(Contains(body, "\"errors\":1"));
    CHECK(Contains(body, "\"error_codes\":{\"worker\":37}"));
    CHECK(Contains(body, "\"drop_counts\":{\"worker\":5}"));
    CHECK(Contains(body,
                  "\"memory\":{\"flags\":1,\"state\":\"disabled\",\"workers\":"
                  "{\"worker\":{\"flags\":1,\"state\":\"disabled\"}}"));

    // No role has been forked, so the readiness probe is false.
    CHECK(BodyOf(Request(path.c_str(), "GET /health\n")) == "{\"ok\":false}\n");

    session.Cleanup();
    CHECK(access(path.c_str(), F_OK) != 0);
  }
}

void TestResourceBlock() {
  const std::string path = TempPath("resources");
  (void)unlink(path.c_str());
  StatusServer server;
  StatusServer::Config cfg;
  cfg.socket_path = path.c_str();
  CHECK(server.Start(cfg));

  StatusSnapshot snap = MakeSnapshot();
  snap.healthy = true;
  snap.resources_checked = true;
  snap.resources_fatal = false;
  snap.resource_warnings = 2;
  snap.nofile_soft = 524288;
  snap.nofile_required = 130;
  snap.shm_free_bytes = 3800000000ULL;
  snap.shm_required_bytes = 100663296ULL;
  snap.disk_free_bytes = 82000000000ULL;
  snap.disk_required_bytes = 0;
  CHECK(server.Publish(snap));

  std::string status = Request(path.c_str(), "GET /status\n");
  ExpectStatus(status, "\"resources\":{\"checked\":true,\"fatal\":false");
  const std::string body = BodyOf(status);
  CHECK(Contains(body, "\"warnings\":2"));
  CHECK(Contains(body, "\"nofile\":{\"soft\":524288,\"required\":130}"));
  CHECK(Contains(body, "\"shm\":{\"free_bytes\":3800000000,"
                       "\"required_bytes\":100663296}"));
  CHECK(Contains(body, "\"ok\":true"));

  // A fatal resource item makes /status ok=false while /health keeps its
  // documented role-liveness meaning.
  snap.resources_fatal = true;
  CHECK(server.Publish(snap));
  status = Request(path.c_str(), "GET /status\n");
  CHECK(Contains(BodyOf(status), "\"fatal\":true"));
  CHECK(Contains(BodyOf(status), "\"ok\":false"));
  CHECK(BodyOf(Request(path.c_str(), "GET /health\n")) == "{\"ok\":true}\n");
  server.Stop();
}

}  // namespace

int main() {
  TestProtocol();
  TestSlowClientsDoNotBlockFastClient();
  TestConnectionCapIsCounted();
  TestSupervisorWiring();
  TestResourceBlock();
  std::printf("test_status_server: OK\n");
  return 0;
}
