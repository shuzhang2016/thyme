#include "thyme/app/status_server.h"

#include "thyme/app/time.h"
#include "thyme/runtime/health_slot.h"
#include "thyme/runtime/log.h"

#include <cerrno>
#include <csignal>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

namespace thyme {
namespace app {
namespace {

constexpr uint64_t kDefaultClientTimeoutNs = 1000ULL * 1000 * 1000;

const char* RoleStateName(uint8_t state) {
  switch (state) {
    case 0:
      return "not_started";
    case 1:
      return "running";
    case 2:
      return "terminating";
    case 3:
      return "waiting_backoff";
    case 4:
      return "gave_up";
    case 5:
      return "stopped";
    default:
      return "unknown";
  }
}

// Bounded JSON writer: never exceeds its preallocated buffer and never
// allocates. Once overflow is observed every further append is ignored.
class JsonBuf {
 public:
  JsonBuf(char* buf, std::size_t cap) : buf_(buf), cap_(cap) {
    if (cap_ > 0) buf_[0] = '\0';
  }

  void Raw(const char* s) {
    if (!ok_ || s == nullptr) return;
    const std::size_t n = std::strlen(s);
    if (n >= cap_ - len_) {
      ok_ = false;
      return;
    }
    std::memcpy(buf_ + len_, s, n);
    len_ += n;
    buf_[len_] = '\0';
  }

  void Fmt(const char* fmt, ...) {
    if (!ok_ || len_ >= cap_) {
      ok_ = false;
      return;
    }
    va_list ap;
    va_start(ap, fmt);
    const int n = std::vsnprintf(buf_ + len_, cap_ - len_, fmt, ap);
    va_end(ap);
    if (n < 0) {
      ok_ = false;
      return;
    }
    if (static_cast<std::size_t>(n) >= cap_ - len_) {
      ok_ = false;
      return;
    }
    len_ += static_cast<std::size_t>(n);
  }

  void Escaped(const char* s) {
    Raw("\"");
    for (const char* p = s; p != nullptr && *p != '\0' && ok_; ++p) {
      const unsigned char ch = static_cast<unsigned char>(*p);
      if (ch == '"' || ch == '\\') {
        char esc[3] = {'\\', static_cast<char>(ch), '\0'};
        Raw(esc);
      } else if (ch < 0x20) {
        char esc[8];
        std::snprintf(esc, sizeof(esc), "\\u%04x", static_cast<unsigned>(ch));
        Raw(esc);
      } else {
        char one[2] = {static_cast<char>(ch), '\0'};
        Raw(one);
      }
    }
    Raw("\"");
  }

  bool ok() const { return ok_; }
  std::size_t size() const { return len_; }

 private:
  char* buf_;
  std::size_t cap_;
  std::size_t len_ = 0;
  bool ok_ = true;
};

const char* RoleMemoryStateName(const StatusRoleSnapshot& r) {
  const uint32_t n =
      r.n_health_memory < StatusRoleSnapshot::kMaxHealthErrors
          ? r.n_health_memory
          : StatusRoleSnapshot::kMaxHealthErrors;
  if (n == 0) {
    return MemoryHealthStateName(r.memory_flags);
  }
  const char* state = MemoryHealthStateName(r.health_memory[0].flags);
  for (uint32_t i = 1; i < n; ++i) {
    if (std::strcmp(state, MemoryHealthStateName(r.health_memory[i].flags)) != 0) {
      return "mixed";
    }
  }
  return state;
}

void AppendRole(JsonBuf& j, const StatusRoleSnapshot& r) {
  j.Raw("{\"id\":");
  j.Escaped(r.id);
  j.Raw(",\"role\":");
  j.Escaped(r.role);
  j.Fmt(",\"state\":\"%s\",\"pid\":%lld", RoleStateName(r.state),
        static_cast<long long>(r.pid));
  j.Fmt(",\"restarts_attempted\":%u,\"restarts_succeeded\":%u",
        static_cast<unsigned>(r.restarts_attempted),
        static_cast<unsigned>(r.restarts_succeeded));
  j.Fmt(",\"last_exit_code\":%lld,\"last_term_signal\":%lld",
        static_cast<long long>(r.last_exit_code),
        static_cast<long long>(r.last_term_signal));
  j.Fmt(",\"stall_age_ms\":%llu,\"loops\":%llu",
        static_cast<unsigned long long>(r.last_stall_age_ns / 1000000ULL),
        static_cast<unsigned long long>(r.loops));
  j.Fmt(",\"health\":{\"alive\":%u,\"deadline_misses\":%u,\"drops\":%llu,"
        "\"errors\":%u,\"sched_mismatches\":%u,\"error_codes\":{",
        static_cast<unsigned>(r.alive),
        static_cast<unsigned>(r.deadline_misses),
        static_cast<unsigned long long>(r.drops),
        static_cast<unsigned>(r.errors),
        static_cast<unsigned>(r.sched_mismatches));
  const uint32_t n_errors =
      r.n_health_errors < StatusRoleSnapshot::kMaxHealthErrors
          ? r.n_health_errors
          : StatusRoleSnapshot::kMaxHealthErrors;
  for (uint32_t i = 0; i < n_errors; ++i) {
    if (i != 0) j.Raw(",");
    j.Escaped(r.health_errors[i].node);
    j.Fmt(":%u", static_cast<unsigned>(r.health_errors[i].error_code));
  }
  j.Raw("},\"drop_counts\":{");
  const uint32_t n_drops =
      r.n_health_drops < StatusRoleSnapshot::kMaxHealthErrors
          ? r.n_health_drops
          : StatusRoleSnapshot::kMaxHealthErrors;
  for (uint32_t i = 0; i < n_drops; ++i) {
    if (i != 0) j.Raw(",");
    j.Escaped(r.health_drops[i].edge);
    j.Fmt(":%llu",
          static_cast<unsigned long long>(r.health_drops[i].drops));
  }
  j.Fmt("},\"memory\":{\"flags\":%u,\"state\":\"%s\",\"workers\":{",
        static_cast<unsigned>(r.memory_flags), RoleMemoryStateName(r));
  const uint32_t n_memory =
      r.n_health_memory < StatusRoleSnapshot::kMaxHealthErrors
          ? r.n_health_memory
          : StatusRoleSnapshot::kMaxHealthErrors;
  for (uint32_t i = 0; i < n_memory; ++i) {
    if (i != 0) {
      j.Raw(",");
    }
    j.Escaped(r.health_memory[i].node);
    j.Fmt(":{\"flags\":%u,\"state\":\"%s\"}",
          static_cast<unsigned>(r.health_memory[i].flags),
          MemoryHealthStateName(r.health_memory[i].flags));
  }
  j.Raw("}}}}");
}

// Returns false when role_filter names no role.
bool BuildStatusJson(char* out, std::size_t cap, const StatusSnapshot& s,
                     const char* role_filter) {
  JsonBuf j(out, cap);
  // `/health` stays the role-liveness probe (documented body). The /status
  // `ok` additionally folds in the OPS-07 resource self-check: a fatal host
  // requirement is not "ok" even when every role happens to be running.
  const bool ok_now = s.healthy && !(s.resources_checked && s.resources_fatal);
  const char* ok = ok_now ? "true" : "false";
  int role_index = -1;
  if (role_filter != nullptr) {
    bool numeric = role_filter[0] != '\0';
    for (const char* p = role_filter; *p != '\0'; ++p) {
      if (*p < '0' || *p > '9') {
        numeric = false;
        break;
      }
    }
    if (numeric) {
      unsigned long v = 0;
      for (const char* p = role_filter; *p != '\0'; ++p) {
        v = v * 10UL + static_cast<unsigned long>(*p - '0');
        if (v > 1000UL) break;
      }
      if (v < static_cast<unsigned long>(s.n_roles)) {
        role_index = static_cast<int>(v);
      }
    }
    if (role_index < 0) {
      for (std::size_t i = 0; i < s.n_roles; ++i) {
        if (std::strcmp(s.roles[i].id, role_filter) == 0 ||
            std::strcmp(s.roles[i].role, role_filter) == 0) {
          role_index = static_cast<int>(i);
          break;
        }
      }
    }
    if (role_index < 0) return false;
  }

  j.Fmt("{\"ok\":%s,\"session_id\":", ok);
  j.Escaped(s.session_id);
  j.Raw(",\"build\":");
  j.Escaped(s.build);
  j.Fmt(",\"uptime_ms\":%llu",
        static_cast<unsigned long long>(s.uptime_ns / 1000000ULL));
  if (role_filter != nullptr) {
    j.Raw(",\"role\":");
    AppendRole(j, s.roles[role_index]);
  } else {
    j.Fmt(",\"role_count\":%u,\"roles\":[", static_cast<unsigned>(s.n_roles));
    for (std::size_t i = 0;
         i < s.n_roles && i < StatusSnapshot::kMaxRoles; ++i) {
      if (i != 0) j.Raw(",");
      AppendRole(j, s.roles[i]);
    }
    j.Raw("]");
  }
  j.Fmt(",\"recorder\":{\"present\":%s,\"trigger_relay_sent\":%llu,"
        "\"trigger_relay_debounced\":%llu,\"trigger_req_seq\":%u,"
        "\"trigger_ack_seq\":%u}",
        s.has_recorder ? "true" : "false",
        static_cast<unsigned long long>(s.trigger_relay_sent),
        static_cast<unsigned long long>(s.trigger_relay_debounced),
        static_cast<unsigned>(s.trigger_req_seq),
        static_cast<unsigned>(s.trigger_ack_seq));
  // OPS-05: rotation/budget counters for this process's append-only logs.
  j.Fmt(",\"logging\":{\"budget_limit_bytes\":%llu,"
        "\"budget_used_bytes\":%llu,\"budget_drops\":%llu,"
        "\"rotations\":%llu}",
        static_cast<unsigned long long>(s.log_budget_limit_bytes),
        static_cast<unsigned long long>(s.log_budget_used_bytes),
        static_cast<unsigned long long>(s.log_budget_drops),
        static_cast<unsigned long long>(s.log_rotations));
  // OPS-07: startup resource self-check results. `ok` above already folds in
  // a fatal resource item; the block below carries the evidence.
  j.Fmt(",\"resources\":{\"checked\":%s,\"fatal\":%s,\"warnings\":%u,"
        "\"nofile\":{\"soft\":%llu,\"required\":%llu},"
        "\"shm\":{\"free_bytes\":%llu,\"required_bytes\":%llu},"
        "\"disk\":{\"free_bytes\":%llu,\"required_bytes\":%llu}}}",
        s.resources_checked ? "true" : "false",
        s.resources_fatal ? "true" : "false",
        static_cast<unsigned>(s.resource_warnings),
        static_cast<unsigned long long>(s.nofile_soft),
        static_cast<unsigned long long>(s.nofile_required),
        static_cast<unsigned long long>(s.shm_free_bytes),
        static_cast<unsigned long long>(s.shm_required_bytes),
        static_cast<unsigned long long>(s.disk_free_bytes),
        static_cast<unsigned long long>(s.disk_required_bytes));
  if (!j.ok()) {
    out[0] = '\0';
    return false;
  }
  return true;
}

}  // namespace

StatusServer::~StatusServer() { Stop(); }

bool StatusServer::Start(const Config& cfg) {
  if (running_.load(std::memory_order_acquire)) return false;
  if (cfg.socket_path == nullptr || cfg.socket_path[0] == '\0') return false;

  struct sockaddr_un addr {};
  addr.sun_family = AF_UNIX;
  if (std::strlen(cfg.socket_path) >= sizeof(addr.sun_path)) return false;
  std::snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", cfg.socket_path);
  std::snprintf(path_, sizeof(path_), "%s", cfg.socket_path);

  // Stale-path policy mirrors record_control::Server::Open: replace only a
  // socket owned by this euid; never unlink a non-socket or another uid's
  // path. (An unowned stale socket is left for the deployment sweep.)
  struct stat st {};
  if (stat(path_, &st) == 0) {
    if (!S_ISSOCK(st.st_mode) || st.st_uid != geteuid()) {
      THYME_LOG_ERROR("status_server: refusing path %s (not an euid-owned "
                      "socket)",
                      path_);
      path_[0] = '\0';
      return false;
    }
    (void)unlink(path_);
  }

  listen_fd_ = socket(AF_UNIX, SOCK_STREAM, 0);
  if (listen_fd_ < 0) {
    path_[0] = '\0';
    return false;
  }
  if (bind(listen_fd_, reinterpret_cast<struct sockaddr*>(&addr),
           sizeof(addr)) != 0) {
    (void)close(listen_fd_);
    listen_fd_ = -1;
    path_[0] = '\0';
    return false;
  }
  // ARCH-05: the endpoint contract is a 0600, non-blocking, close-on-exec
  // socket. Every one of those steps is checked; a silent downgrade (a
  // world-readable status page, or an accept() that can block the server
  // thread) is treated as a startup failure.
  if (chmod(path_, S_IRUSR | S_IWUSR) != 0) {
    THYME_LOG_ERROR("status_server: chmod %s failed: %s", path_,
                    std::strerror(errno));
    (void)close(listen_fd_);
    listen_fd_ = -1;
    (void)unlink(path_);
    path_[0] = '\0';
    return false;
  }
  if (listen(listen_fd_, static_cast<int>(kMaxClients)) != 0) {
    THYME_LOG_ERROR("status_server: listen %s failed: %s", path_,
                    std::strerror(errno));
    (void)close(listen_fd_);
    listen_fd_ = -1;
    (void)unlink(path_);
    path_[0] = '\0';
    return false;
  }
  const int flags = fcntl(listen_fd_, F_GETFL, 0);
  if (flags < 0 || fcntl(listen_fd_, F_SETFL, flags | O_NONBLOCK) != 0) {
    THYME_LOG_ERROR("status_server: fcntl O_NONBLOCK failed: %s",
                    std::strerror(errno));
    (void)close(listen_fd_);
    listen_fd_ = -1;
    (void)unlink(path_);
    path_[0] = '\0';
    return false;
  }
  if (fcntl(listen_fd_, F_SETFD, FD_CLOEXEC) != 0) {
    THYME_LOG_WARN("status_server: fcntl FD_CLOEXEC failed: %s",
                   std::strerror(errno));
  }

  max_clients_ = cfg.max_clients == 0 ? 1 : static_cast<std::size_t>(cfg.max_clients);
  if (max_clients_ > kMaxClients) max_clients_ = kMaxClients;
  client_timeout_ns_ =
      cfg.client_timeout_ns == 0 ? kDefaultClientTimeoutNs : cfg.client_timeout_ns;
  for (std::size_t i = 0; i < kMaxClients; ++i) {
    clients_[i] = Client{};
  }
  published_.store(false, std::memory_order_release);
  stop_.store(false, std::memory_order_release);
  running_.store(true, std::memory_order_release);
  if (pthread_create(&thread_, nullptr, &StatusServer::Trampoline, this) != 0) {
    running_.store(false, std::memory_order_release);
    (void)close(listen_fd_);
    listen_fd_ = -1;
    (void)unlink(path_);
    path_[0] = '\0';
    return false;
  }
  return true;
}

void StatusServer::Stop() {
  if (!running_.exchange(false, std::memory_order_acq_rel)) return;
  stop_.store(true, std::memory_order_release);
  (void)pthread_join(thread_, nullptr);
  if (listen_fd_ >= 0) {
    (void)close(listen_fd_);
    listen_fd_ = -1;
  }
  for (std::size_t i = 0; i < kMaxClients; ++i) {
    if (clients_[i].fd >= 0) {
      (void)close(clients_[i].fd);
      clients_[i] = Client{};
    }
  }
  if (path_[0] != '\0') {
    (void)unlink(path_);
    path_[0] = '\0';
  }
  published_.store(false, std::memory_order_release);
  reader_active_.store(false, std::memory_order_release);
}

bool StatusServer::Publish(const StatusSnapshot& snapshot) {
  // Single writer (app thread). Skip the update if a request is copying the
  // active slot; writing the inactive slot is safe regardless (a reader that
  // starts after this load reads the current active slot, never the one being
  // written). The next tick publishes again, so skipping loses nothing.
  if (reader_active_.load(std::memory_order_acquire)) return false;
  const uint32_t next =
      active_slot_.load(std::memory_order_acquire) ^ 1U;
  slots_[next] = snapshot;
  active_slot_.store(next, std::memory_order_release);
  published_.store(true, std::memory_order_release);
  return true;
}

bool StatusServer::CopySnapshot(StatusSnapshot* out) const {
  const bool published = published_.load(std::memory_order_acquire);
  reader_active_.store(true, std::memory_order_release);
  const uint32_t index = active_slot_.load(std::memory_order_acquire);
  *out = slots_[index];
  reader_active_.store(false, std::memory_order_release);
  return published;
}

void* StatusServer::Trampoline(void* self) {
  static_cast<StatusServer*>(self)->Run();
  return nullptr;
}

void StatusServer::Run() {
  while (!stop_.load(std::memory_order_acquire)) {
    struct pollfd pfds[1 + kMaxClients];
    int client_slot[1 + kMaxClients];
    int nfds = 0;
    pfds[nfds].fd = listen_fd_;
    pfds[nfds].events = POLLIN;
    pfds[nfds].revents = 0;
    client_slot[nfds] = -1;
    ++nfds;
    for (std::size_t i = 0; i < kMaxClients; ++i) {
      if (clients_[i].fd < 0) continue;
      pfds[nfds].fd = clients_[i].fd;
      pfds[nfds].events = clients_[i].writing ? POLLOUT : POLLIN;
      pfds[nfds].revents = 0;
      client_slot[nfds] = static_cast<int>(i);
      ++nfds;
    }

    const int rc = poll(pfds, static_cast<nfds_t>(nfds), 10);
    if (rc > 0) {
      if ((pfds[0].revents & POLLIN) != 0) AcceptClients();
      for (int k = 1; k < nfds; ++k) {
        Client& client = clients_[client_slot[k]];
        if (client.fd < 0) continue;
        const short revents = pfds[k].revents;
        if (client.writing) {
          if ((revents & (POLLOUT | POLLERR | POLLHUP)) != 0) {
            HandleWrite(client);
          }
        } else if ((revents & (POLLIN | POLLERR | POLLHUP)) != 0) {
          HandleRead(client);
        }
      }
    }

    const uint64_t now = NowMonoNs();
    for (std::size_t i = 0; i < kMaxClients; ++i) {
      Client& client = clients_[i];
      if (client.fd < 0) continue;
      if (now >= client.deadline_ns) {
        timed_out_clients_.fetch_add(1, std::memory_order_relaxed);
        CloseClient(client);
      }
    }
  }

  for (std::size_t i = 0; i < kMaxClients; ++i) {
    if (clients_[i].fd >= 0) CloseClient(clients_[i]);
  }
}

void StatusServer::AcceptClients() {
  for (;;) {
    struct sockaddr_un peer {};
    socklen_t peer_len = sizeof(peer);
    const int fd = accept(listen_fd_, reinterpret_cast<struct sockaddr*>(&peer),
                          &peer_len);
    if (fd < 0) {
      if (errno == EINTR) continue;
      if (errno != EAGAIN && errno != EWOULDBLOCK) {
        // EMFILE/ENFILE and friends: the socket backs up instead of the
        // server thread spinning, but the failure must be visible.
        accept_errors_.fetch_add(1, std::memory_order_relaxed);
        THYME_LOG_WARN_RATED(::thyme::log::kOnceThenEvery1000,
                             "status_server: accept failed: %s",
                             std::strerror(errno));
      }
      return;
    }
    const int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) != 0) {
      // Without O_NONBLOCK a slow client would block the whole status
      // thread; refuse this connection instead of degrading silently.
      THYME_LOG_WARN_RATED(::thyme::log::kOnceThenEvery1000,
                           "status_server: client fcntl O_NONBLOCK failed: %s",
                           std::strerror(errno));
      (void)close(fd);
      continue;
    }
    if (fcntl(fd, F_SETFD, FD_CLOEXEC) != 0) {
      THYME_LOG_WARN_RATED(::thyme::log::kOnceThenEvery1000,
                           "status_server: client FD_CLOEXEC failed: %s",
                           std::strerror(errno));
    }

    std::size_t used = 0;
    std::size_t free_slot = kMaxClients;
    for (std::size_t i = 0; i < kMaxClients; ++i) {
      if (clients_[i].fd >= 0) {
        ++used;
      } else if (free_slot == kMaxClients) {
        free_slot = i;
      }
    }
    if (used >= max_clients_ || free_slot == kMaxClients) {
      // Cap reached: shed load, never queue in-process. Counted so a client
      // storm is visible in the status counters instead of looking like a
      // dropped connection with no explanation.
      rejected_clients_.fetch_add(1, std::memory_order_relaxed);
      (void)close(fd);
      continue;
    }
    Client& client = clients_[free_slot];
    client = Client{};
    client.fd = fd;
    client.deadline_ns = NowMonoNs() + client_timeout_ns_;
  }
}

void StatusServer::HandleRead(Client& client) {
  char buf[256];
  if (client.draining) {
    for (;;) {
      const ssize_t n = recv(client.fd, buf, sizeof(buf), MSG_DONTWAIT);
      if (n > 0) continue;
      if (n == 0) {
        CloseClient(client);
        return;
      }
      if (errno == EINTR) continue;
      if (errno == EAGAIN || errno == EWOULDBLOCK) return;
      CloseClient(client);
      return;
    }
  }
  for (;;) {
    const ssize_t n = recv(client.fd, buf, sizeof(buf), MSG_DONTWAIT);
    if (n > 0) {
      std::size_t take = static_cast<std::size_t>(n);
      if (client.in_len + take > kMaxRequestBytes) {
        take = kMaxRequestBytes - client.in_len;
      }
      std::memcpy(client.in + client.in_len, buf, take);
      client.in_len += take;
      const void* nl = std::memchr(client.in, '\n', client.in_len);
      if (nl != nullptr) {
        const char* line = static_cast<const char*>(nl);
        HandleRequest(client, client.in,
                      static_cast<std::size_t>(line - client.in) + 1);
        return;
      }
      if (client.in_len >= kMaxRequestBytes ||
          take < static_cast<std::size_t>(n)) {
        rejected_oversized_.fetch_add(1, std::memory_order_relaxed);
        static const char kTooLong[] =
            "{\"ok\":false,\"error\":\"request too long\"}\n";
        client.draining = true;
        Respond(client, 400, "Bad Request", kTooLong, sizeof(kTooLong) - 1);
        return;
      }
    } else if (n < 0 && errno == EINTR) {
      continue;
    } else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      return;
    } else {
      CloseClient(client);
      return;
    }
  }
}

void StatusServer::HandleWrite(Client& client) {
  while (client.out_sent < client.out_len) {
    const ssize_t n =
        send(client.fd, client.out + client.out_sent,
             client.out_len - client.out_sent, MSG_DONTWAIT | MSG_NOSIGNAL);
    if (n > 0) {
      client.out_sent += static_cast<std::size_t>(n);
    } else if (n < 0 && errno == EINTR) {
      continue;
    } else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      return;  // poll() waits for POLLOUT until the deadline
    } else {
      CloseClient(client);
      return;
    }
  }
  if (client.draining) {
    // Half-close so the peer keeps reading the error response; the input is
    // discarded until EOF (or the connection deadline).
    (void)shutdown(client.fd, SHUT_WR);
    client.writing = false;
    client.out_len = 0;
    client.out_sent = 0;
    return;
  }
  CloseClient(client);
}

void StatusServer::HandleRequest(Client& client, const char* line,
                                 std::size_t len) {
  requests_served_.fetch_add(1, std::memory_order_relaxed);
  std::size_t n = len;
  while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r')) --n;

  std::size_t method_len = 0;
  while (method_len < n && line[method_len] != ' ') ++method_len;
  if (method_len == n || method_len + 1 >= n) {
    static const char kBad[] = "{\"ok\":false,\"error\":\"bad request\"}\n";
    Respond(client, 400, "Bad Request", kBad, sizeof(kBad) - 1);
    return;
  }
  if (method_len != 3 || std::memcmp(line, "GET", 3) != 0) {
    static const char kReadOnly[] =
        "{\"ok\":false,\"error\":\"read-only endpoint\"}\n";
    Respond(client, 405, "Method Not Allowed", kReadOnly,
            sizeof(kReadOnly) - 1);
    return;
  }

  const char* target = line + method_len + 1;
  std::size_t target_len = n - method_len - 1;
  const void* space = std::memchr(target, ' ', target_len);
  if (space != nullptr) {
    target_len = static_cast<std::size_t>(
        static_cast<const char*>(space) - target);
  }
  if (target_len == 0) {
    static const char kBad[] = "{\"ok\":false,\"error\":\"bad request\"}\n";
    Respond(client, 400, "Bad Request", kBad, sizeof(kBad) - 1);
    return;
  }

  const void* qmark = std::memchr(target, '?', target_len);
  const std::size_t path_len =
      qmark == nullptr
          ? target_len
          : static_cast<std::size_t>(static_cast<const char*>(qmark) - target);
  const char* query = qmark == nullptr ? nullptr : static_cast<const char*>(qmark) + 1;
  const std::size_t query_len =
      qmark == nullptr ? 0 : target_len - path_len - 1;

  StatusSnapshot snapshot;
  const bool published = CopySnapshot(&snapshot);

  if (path_len == 7 && std::memcmp(target, "/health", 7) == 0 &&
      query_len == 0) {
    const bool ok = published && snapshot.healthy;
    static const char kOk[] = "{\"ok\":true}\n";
    static const char kNotOk[] = "{\"ok\":false}\n";
    if (ok) {
      Respond(client, 200, "OK", kOk, sizeof(kOk) - 1);
    } else {
      Respond(client, 200, "OK", kNotOk, sizeof(kNotOk) - 1);
    }
    return;
  }

  if (path_len != 7 || std::memcmp(target, "/status", 7) != 0) {
    static const char kNotFound[] = "{\"ok\":false,\"error\":\"not found\"}\n";
    Respond(client, 404, "Not Found", kNotFound, sizeof(kNotFound) - 1);
    return;
  }

  char role_filter[64];
  const char* filter = nullptr;
  if (query_len > 0) {
    if (query_len < 6 || std::memcmp(query, "role=", 5) != 0 ||
        query_len - 5 >= sizeof(role_filter)) {
      static const char kBad[] = "{\"ok\":false,\"error\":\"bad query\"}\n";
      Respond(client, 400, "Bad Request", kBad, sizeof(kBad) - 1);
      return;
    }
    std::memcpy(role_filter, query + 5, query_len - 5);
    role_filter[query_len - 5] = '\0';
    filter = role_filter;
  }

  char body[kMaxResponseBytes];
  if (!BuildStatusJson(body, sizeof(body), snapshot, filter)) {
    static const char kNoRole[] =
        "{\"ok\":false,\"error\":\"unknown role\"}\n";
    Respond(client, 404, "Not Found", kNoRole, sizeof(kNoRole) - 1);
    return;
  }
  Respond(client, 200, "OK", body, std::strlen(body));
}

void StatusServer::Respond(Client& client, int code, const char* reason,
                           const char* body, std::size_t body_len) {
  int header = std::snprintf(
      client.out, sizeof(client.out),
      "HTTP/1.1 %d %s\r\nContent-Type: application/json\r\n"
      "Content-Length: %zu\r\nConnection: close\r\n\r\n",
      code, reason, body_len);
  if (header < 0 ||
      static_cast<std::size_t>(header) + body_len > sizeof(client.out)) {
    static const char kFallback[] =
        "{\"ok\":false,\"error\":\"response too large\"}\n";
    body = kFallback;
    body_len = sizeof(kFallback) - 1;
    header = std::snprintf(
        client.out, sizeof(client.out),
        "HTTP/1.1 500 Internal Server Error\r\n"
        "Content-Type: application/json\r\nContent-Length: %zu\r\n"
        "Connection: close\r\n\r\n",
        body_len);
    if (header < 0 ||
        static_cast<std::size_t>(header) + body_len > sizeof(client.out)) {
      CloseClient(client);
      return;
    }
  }
  std::memcpy(client.out + static_cast<std::size_t>(header), body, body_len);
  client.out_len = static_cast<std::size_t>(header) + body_len;
  client.out_sent = 0;
  client.writing = true;
  HandleWrite(client);
}

void StatusServer::CloseClient(Client& client) {
  if (client.fd >= 0) (void)close(client.fd);
  client = Client{};
}

}  // namespace app
}  // namespace thyme
