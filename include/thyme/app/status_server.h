#pragma once
// OPS-10: read-only JSON status endpoint for external monitoring.
//
// A StatusServer owns one AF_UNIX SOCK_STREAM socket (mode 0600, euid-owned)
// and a small dedicated thread. It answers a line-oriented, read-only
// protocol and closes the connection after one response:
//
//   GET /status            full snapshot (process/role states, counters)
//   GET /status?role=<id>  one role by index, process id, or role name
//   GET /health            {"ok":true|false} (liveness/readiness probe)
//
// There is no write surface: POST/PUT/DELETE and unknown paths are rejected.
// The request line is capped at kMaxRequestBytes, the response at
// kMaxResponseBytes, and concurrent clients at Config::max_clients. All of
// them are fixed inline buffers: serving a request performs no allocation.
// Slow clients are serviced non-blockingly with a per-connection deadline, so
// a client that never finishes its request cannot delay other clients (and
// cannot delay the app: the socket is served by the status thread, not the
// caller's tick).
//
// Snapshot publication is lock-free and bounded: the app calls Publish() from
// its own thread (StatusServer keeps two preallocated slots and flips the
// active one; a publish that races an in-flight read is skipped rather than
// blocking). The server thread copies the active slot when a request arrives.
//
// Test the endpoint from a shell (no CLI is shipped):
//
//   curl --unix-socket /run/thyme/status.sock http://localhost/status
//   curl --unix-socket /run/thyme/status.sock http://localhost/health
//   socat - UNIX-CONNECT:/run/thyme/status.sock <<< 'GET /status?role=0'
//
// Clean shutdown closes the listening socket and unlinks the bound path.
// Start() mirrors record_control/trigger_center stale-path policy: an
// existing socket owned by the euid is replaced; a non-socket path or a path
// owned by another uid is refused (never unlinked).

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <pthread.h>

#ifndef THYME_VERSION_STRING
#define THYME_VERSION_STRING "thyme"
#endif

namespace thyme {
namespace app {

// JSON state names align with MpRoleState's numeric values (OPS-01).
struct StatusHealthError {
  char node[32]{};
  uint32_t error_code = 0;
};

struct StatusHealthDrop {
  char edge[32]{};
  uint64_t drops = 0;
};

struct StatusHealthMemory {
  char node[32]{};
  uint32_t flags = 0;
};

struct StatusRoleSnapshot {
  static constexpr std::size_t kMaxHealthErrors = 64;

  char id[32]{};
  char role[32]{};
  uint8_t state = 0;  // MpRoleState (0 not_started ... 5 stopped)
  int32_t pid = 0;
  uint32_t restarts_attempted = 0;
  uint32_t restarts_succeeded = 0;
  int32_t last_exit_code = -1;   // -1: signaled or never reaped
  int32_t last_term_signal = 0;  // 0: normal exit or never reaped
  uint64_t last_stall_age_ns = 0;
  uint64_t loops = 0;  // MpControl::proc_loops for this process
  // Aggregated over the graph nodes owned by this process.
  uint32_t alive = 0;
  uint32_t deadline_misses = 0;
  // RT-23: 64-bit, straight from HealthSlot::drop_cnt.
  uint64_t drops = 0;
  uint32_t errors = 0;
  // SCH-08: threads whose requested scheduling (policy/priority/affinity)
  // is not what the kernel actually applied (e.g. FIFO w/o CAP_SYS_NICE).
  uint32_t sched_mismatches = 0;
  // Bitwise OR of worker HealthSlot::memory_flags; never folded into errors.
  uint32_t memory_flags = 0;
  uint32_t n_health_errors = 0;
  StatusHealthError health_errors[kMaxHealthErrors]{};
  uint32_t n_health_drops = 0;
  StatusHealthDrop health_drops[kMaxHealthErrors]{};
  uint32_t n_health_memory = 0;
  StatusHealthMemory health_memory[kMaxHealthErrors]{};
};

// POD snapshot published by the app thread and copied by the server thread.
struct StatusSnapshot {
  static constexpr std::size_t kMaxRoles = 16;

  char session_id[48]{};  // MP session identity (c9_<supervisor pid>)
  char build[48]{};       // THYME_VERSION_STRING
  uint64_t uptime_ns = 0;
  bool healthy = false;
  uint32_t n_roles = 0;
  StatusRoleSnapshot roles[kMaxRoles]{};

  // Trigger relay counters from MpControl (cheap, no recorder I/O).
  bool has_recorder = false;
  uint64_t trigger_relay_sent = 0;
  uint64_t trigger_relay_debounced = 0;
  uint32_t trigger_req_seq = 0;
  uint32_t trigger_ack_seq = 0;

  // OPS-05: this process's append-only log budget (rotating_file.h). The
  // budget is per process: the status endpoint reports the process that
  // serves it; MP roles enforce and account their own, so the fleet bound is
  // the per-process bound summed over the running processes.
  uint64_t log_budget_limit_bytes = 0;  // 0 = per-file caps only
  uint64_t log_budget_used_bytes = 0;
  uint64_t log_budget_drops = 0;        // records dropped at the budget
  uint64_t log_rotations = 0;           // rotations across all writers

  // OPS-07: startup resource self-check (see runtime/resource_check.h). The
  // host runs it once at startup; checked=false means no check was performed
  // (an embedder that never called it), not that everything passed.
  // nofile_soft/shm_free/... are UINT64_MAX for "unlimited".
  bool resources_checked = false;
  bool resources_fatal = false;
  uint32_t resource_warnings = 0;
  uint64_t nofile_soft = 0;
  uint64_t nofile_required = 0;
  uint64_t shm_free_bytes = 0;
  uint64_t shm_required_bytes = 0;
  uint64_t disk_free_bytes = 0;
  uint64_t disk_required_bytes = 0;
};

class StatusServer {
 public:
  static constexpr std::size_t kMaxRoles = StatusSnapshot::kMaxRoles;
  static constexpr std::size_t kMaxClients = 4;
  static constexpr std::size_t kMaxRequestBytes = 512;
  static constexpr std::size_t kMaxResponseBytes = 16384;

  struct Config {
    // Required, absolute or relative UNIX socket path (< sun_path).
    const char* socket_path = nullptr;
    // Concurrent connections; clamped to kMaxClients.
    uint32_t max_clients = 4;
    // A connection is closed when it has not completed within this bound.
    uint64_t client_timeout_ns = 1000ULL * 1000 * 1000;
  };

  StatusServer() = default;
  ~StatusServer();
  StatusServer(const StatusServer&) = delete;
  StatusServer& operator=(const StatusServer&) = delete;

  // Binds/listens and starts the serving thread. False on bad config, an
  // unusable path, bind/listen failure, or thread creation failure; no
  // partially bound socket is left behind.
  bool Start(const Config& cfg);
  // Idempotent; joins the serving thread, closes the socket, unlinks the
  // bound path.
  void Stop();
  bool running() const {
    return running_.load(std::memory_order_acquire);
  }
  const char* socket_path() const { return path_; }

  // Non-blocking: copies the snapshot into the inactive slot and flips it.
  // Returns false when a reader is copying the active slot (the update is
  // skipped; the next tick publishes again).
  bool Publish(const StatusSnapshot& snapshot);

  // Test/observability counters.
  uint64_t requests_served() const {
    return requests_served_.load(std::memory_order_relaxed);
  }
  uint64_t rejected_oversized() const {
    return rejected_oversized_.load(std::memory_order_relaxed);
  }
  uint64_t timed_out_clients() const {
    return timed_out_clients_.load(std::memory_order_relaxed);
  }
  // ARCH-05: a connection accepted while max_clients are already in use is
  // closed immediately (never queued in-process); every shed is counted.
  uint64_t rejected_clients() const {
    return rejected_clients_.load(std::memory_order_relaxed);
  }
  // accept(2) failures other than EINTR/EAGAIN/EWOULDBLOCK (e.g. EMFILE).
  uint64_t accept_errors() const {
    return accept_errors_.load(std::memory_order_relaxed);
  }

 private:
  struct Client {
    int fd = -1;
    bool writing = false;
    // Request already rejected (e.g. too long): finish the response, shut the
    // write side, then discard input until EOF so close() never resets a
    // connection with queued data and loses the error response.
    bool draining = false;
    uint64_t deadline_ns = 0;
    std::size_t in_len = 0;
    char in[kMaxRequestBytes] = {};
    std::size_t out_len = 0;
    std::size_t out_sent = 0;
    char out[kMaxResponseBytes] = {};
  };

  static void* Trampoline(void* self);
  void Run();
  void AcceptClients();
  void HandleRead(Client& client);
  void HandleWrite(Client& client);
  void HandleRequest(Client& client, const char* line, std::size_t len);
  void Respond(Client& client, int code, const char* reason, const char* body,
               std::size_t body_len);
  void CloseClient(Client& client);

  bool CopySnapshot(StatusSnapshot* out) const;

  int listen_fd_ = -1;
  char path_[108]{};
  std::size_t max_clients_ = kMaxClients;
  uint64_t client_timeout_ns_ = 1000ULL * 1000 * 1000;
  pthread_t thread_{};
  std::atomic<bool> running_{false};
  std::atomic<bool> stop_{false};

  Client clients_[kMaxClients];

  // Double-buffer publication. Only the app thread writes; only the server
  // thread reads. A publish skips while reader_active_ is set, so a buffer is
  // never written while it (or the slot about to be published) is read.
  mutable std::atomic<bool> reader_active_{false};
  std::atomic<uint32_t> active_slot_{0};
  std::atomic<bool> published_{false};
  StatusSnapshot slots_[2];

  std::atomic<uint64_t> requests_served_{0};
  std::atomic<uint64_t> rejected_oversized_{0};
  std::atomic<uint64_t> timed_out_clients_{0};
  std::atomic<uint64_t> rejected_clients_{0};
  std::atomic<uint64_t> accept_errors_{0};
};

}  // namespace app
}  // namespace thyme
