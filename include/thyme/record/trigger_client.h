#pragma once

// trigger_client.h — header-only V4 trigger-center client (design doc 5.1).
// One logical request gets up to three identical attempts inside one E2E
// deadline (default 100 ms), with SO_SNDTIMEO bounding each send. There are no
// client threads or exceptions. The reply must match request_id and CRC32.
// Timeout means result-unknown, never proof that the center did not execute.
//
// Semantics: kAccepted means the trigger is **queued** (a trigger_id has been
// assigned); the center's Tick starts/advances the capture asynchronously — it
// does NOT mean the capture has started or produced a package; **the package
// directory/notification is the source of truth**. kBusy means the previous
// trigger has not started yet (queue full / preemption in progress) and the
// call may be retried later; kRecorderOffline is the only real "offline"; a
// client timeout (transport_ok=false) only means no reply was received.
//
// Accepted semantics: after Accepted no error code can be returned
// asynchronously. A failed preempt/start rolls the FSM back in the center and
// counts start_failures/preempt_failures; the caller determines the final
// result from the package directory/status query instead of waiting for a
// "late" error code.
//
// Cancel + asynchronous completion notification semantics:
// * Cancel(socket, trigger_id) cancels a queued-but-not-started or a running
//   capture: kCancelled = cancelled (a not-started trigger is dropped and the
//   FSM rolled back; a running one is truncated and sealed as on preempt,
//   keeping what was already written to disk); kUnknownTrigger = unknown or
//   already completed id (explicit rejection, never silent success); kBusy =
//   the cancel could not be applied (the capture is still running), retry.
// * WaitOutcome(socket, trigger_id, timeout) blocks until one capture outcome
//   notification arrives (trigger_id = 0 means any), the reply carries
//   kRespFlagOutcomeNotification, and code is kCaptureSealed /
//   kPreemptedByHigher / kStartFailed / kPreemptFailed / kCancelled; kBusy
//   means the server's waiter table is full (retry); a timeout is still
//   kTransportTimeout (the server does not answer; notifications have their
//   own semantics and §3.19 still holds for TriggerResponse — no late error
//   codes). A single wait is capped at 60 s (the protocol's
//   response_timeout_ms maximum); longer waits are looped by the caller.

#include "thyme/record/trigger_protocol.h"

#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>

#if !defined(_WIN32)
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#endif

namespace thyme {
namespace trigger_client {

// Default E2E response deadline. The center answers within one Tick; 100 ms
// leaves room for a busy control loop while keeping caller latency bounded.
inline constexpr uint64_t kDefaultTimeoutNs = 100000000ULL;

struct Result {
  bool transport_ok = false;  // false: no valid reply within the deadline
  trigger_proto::TriggerCode code =
      trigger_proto::TriggerCode::kRecorderOffline;
  uint64_t trigger_id = 0;
  // V2+: priority the FSM actually used, and whether the request's hint was
  // clamped (by [security].max_priority_hint or by the rule's cap).
  int32_t effective_priority = 0;
  bool priority_clamped = false;
  // TRG-07: raw response flags. kRespFlagOutcomeNotification marks an outcome
  // notification; kRespFlagDeduplicated marks a V4 retry-cache hit.
  uint32_t response_flags = 0;
  uint64_t request_id = 0;
  uint32_t attempts = 0; // includes the first send; bounded by kMaxAttempts
  uint32_t retries = 0;
  bool deduplicated = false;
  // transport_ok=false + kTransportTimeout means the outcome is unknown. The
  // same request_id may be retried safely; it never means "not executed".
};

// Per-process monotonic request sequence (V2 anti-replay). Initialised from
// CLOCK_MONOTONIC so a restarted process continues above its old values
// instead of reusing them; a monotonic clock never runs backwards within a
// boot, and the server forgets its replay windows when it restarts.
#if !defined(_WIN32)
inline uint64_t MonoNowNs() {
  struct timespec ts {};
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<uint64_t>(ts.tv_sec) * 1000000000ULL +
         static_cast<uint64_t>(ts.tv_nsec);
}

inline uint64_t NextSequence() {
  static const uint64_t base = [] {
    struct timespec ts {};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1000000000ULL +
           static_cast<uint64_t>(ts.tv_nsec);
  }();
  static std::atomic<uint64_t> counter{0};
  const uint64_t seq =
      base + counter.fetch_add(1, std::memory_order_relaxed);
  return seq == 0 ? 1 : seq;
}
inline uint64_t NextRequestId() {
  static const uint64_t base = MonoNowNs();
  static std::atomic<uint64_t> counter{0};
  const uint64_t id = base + counter.fetch_add(1, std::memory_order_relaxed);
  return id == 0 ? 1 : id;
}
#endif

inline constexpr uint32_t kMaxAttempts = 3;

// V4 bounded roundtrip. One private socket and one request identity are reused
// for every attempt; the total timeout is split across attempts. SO_SNDTIMEO
// bounds a blocked send as well as poll() bounds the reply wait.
inline Result RoundTrip(const char* socket_path,
                        const trigger_proto::TriggerRequestV4& req,
                        uint32_t wire_ms,
                        uint32_t max_attempts = kMaxAttempts) {
  Result out;
  out.request_id = req.request_id;
  if (max_attempts == 0 || max_attempts > kMaxAttempts) return out;
#if defined(_WIN32)
  (void)socket_path;
  (void)req;
  (void)wire_ms;
  return out;
#else
  const uint64_t start_ns = MonoNowNs();
  const uint64_t total_ns = static_cast<uint64_t>(wire_ms) * 1000000ULL;
  const uint64_t deadline_ns = start_ns + total_ns;
  const int fd = socket(AF_UNIX, SOCK_DGRAM, 0);
  if (fd < 0) return out;

  // The reply needs a peer address: bind a private path for this call.
  static std::atomic<uint32_t> call_counter{0};
  char self[108];
  std::snprintf(self, sizeof(self), "/tmp/thyme_trig_%d_%u",
                static_cast<int>(getpid()),
                call_counter.fetch_add(1, std::memory_order_relaxed));
  unlink(self);
  sockaddr_un self_addr{};
  self_addr.sun_family = AF_UNIX;
  std::snprintf(self_addr.sun_path, sizeof(self_addr.sun_path), "%s", self);
  if (bind(fd, reinterpret_cast<sockaddr*>(&self_addr), sizeof(self_addr)) !=
      0) {
    close(fd);
    return out;
  }
  const auto cleanup = [&]() {
    unlink(self);
    close(fd);
  };

  sockaddr_un srv{};
  srv.sun_family = AF_UNIX;
  std::snprintf(srv.sun_path, sizeof(srv.sun_path), "%s", socket_path);
  for (uint32_t attempt = 0; attempt < max_attempts; ++attempt) {
    const uint64_t now = MonoNowNs();
    if (now >= deadline_ns)
      break;
    out.attempts += 1;
    out.retries = out.attempts - 1;
    const uint64_t attempt_deadline =
        start_ns + total_ns * (attempt + 1) / max_attempts;
    bool sent_ok = false;
    bool send_failed = false;
    while (!sent_ok && !send_failed && MonoNowNs() < attempt_deadline) {
      const uint64_t remain_ns = attempt_deadline - MonoNowNs();
      timeval tv{};
      tv.tv_sec = static_cast<time_t>(remain_ns / 1000000000ULL);
      tv.tv_usec =
          static_cast<suseconds_t>((remain_ns % 1000000000ULL) / 1000ULL);
      if (tv.tv_sec == 0 && tv.tv_usec == 0)
        tv.tv_usec = 1;
      if (setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv)) != 0) {
        cleanup();
        return out;
      }
      const ssize_t sent =
          sendto(fd, &req, sizeof(req), 0, reinterpret_cast<sockaddr *>(&srv),
                 sizeof(srv));
      if (sent == static_cast<ssize_t>(sizeof(req))) {
        sent_ok = true;
      } else if (sent < 0 && errno == EINTR) {
        continue;
      } else if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
        break;
      } else {
        send_failed = true;
      }
    }
    if (send_failed) {
      cleanup();
      return out;
    }
    while (sent_ok) {
      const uint64_t receive_now = MonoNowNs();
      if (receive_now >= attempt_deadline)
        break;
      uint64_t remain_ms =
          (attempt_deadline - receive_now + 999999ULL) / 1000000ULL;
      if (remain_ms == 0)
        remain_ms = 1;
      pollfd pfd{};
      pfd.fd = fd;
      pfd.events = POLLIN;
      const int pr = poll(&pfd, 1, static_cast<int>(remain_ms));
      if (pr < 0) {
        if (errno == EINTR)
          continue;
        cleanup();
        return out;
      }
      if (pr == 0)
        break;
      if ((pfd.revents & POLLIN) == 0)
        continue;
      trigger_proto::TriggerResponseV4 resp{};
      const ssize_t n = recv(fd, &resp, sizeof(resp), MSG_DONTWAIT);
      if (n < 0) {
        if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)
          continue;
        cleanup();
        return out;
        }
      if (n != static_cast<ssize_t>(sizeof(resp)) ||
          resp.magic != trigger_proto::kTriggerMagic ||
          resp.version != trigger_proto::kTriggerVersion ||
          resp.request_id != req.request_id || resp.reserved0 != 0 ||
          resp.reserved1 != 0 ||
          resp.crc32 != trigger_proto::ResponseCrcV4(resp)) {
        continue;
      }
      out.transport_ok = true;
      out.code = static_cast<trigger_proto::TriggerCode>(resp.code);
      out.trigger_id = resp.trigger_id;
      out.effective_priority = resp.effective_priority;
      out.priority_clamped =
          (resp.flags & trigger_proto::kRespFlagPriorityClamped) != 0;
      out.response_flags = resp.flags;
      out.deduplicated =
          (resp.flags & trigger_proto::kRespFlagDeduplicated) != 0;
      cleanup();
      return out;
    }
  }
  out.code = trigger_proto::TriggerCode::kTransportTimeout;
  cleanup();
  return out;
#endif
}

#if !defined(_WIN32)
// Clamp a caller deadline to the protocol's 1..60000 ms window (0 ns -> 1 ms).
inline uint32_t WireTimeoutMs(uint64_t timeout_ns) {
  uint64_t wire_ms64 = timeout_ns / 1000000ULL;
  if (timeout_ns % 1000000ULL != 0)
    wire_ms64 += 1; // round up, no overflow
  if (wire_ms64 < trigger_proto::kMinResponseTimeoutMs) {
    wire_ms64 = trigger_proto::kMinResponseTimeoutMs;
  }
  if (wire_ms64 > trigger_proto::kMaxResponseTimeoutMs) {
    wire_ms64 = trigger_proto::kMaxResponseTimeoutMs;
  }
  return static_cast<uint32_t>(wire_ms64);
}
#endif

// timestamp_ns == 0 -> the server uses CLOCK_MONOTONIC now (has_timestamp
// stays unset).
// timeout_ns is the E2E deadline (clamped to the protocol's 1..60000 ms
// window; 0 means "one millisecond"). A zero/negative-value guard is applied
// before the value reaches the wire.
inline Result Trigger(const char* socket_path, const char* source,
                      uint64_t timestamp_ns = 0, int32_t priority_hint = -1,
                      bool force = false,
                      uint64_t timeout_ns = kDefaultTimeoutNs) {
  Result out;
#if defined(_WIN32)
  (void)socket_path;
  (void)source;
  (void)timestamp_ns;
  (void)priority_hint;
  (void)force;
  (void)timeout_ns;
  return out;
#else
  if (socket_path == nullptr || source == nullptr || source[0] == '\0' ||
      std::strlen(source) >= sizeof(trigger_proto::TriggerRequestV4::source)) {
    return out;
  }
  const uint32_t wire_ms = WireTimeoutMs(timeout_ns);

  trigger_proto::TriggerRequestV4 req;
  req.magic = trigger_proto::kTriggerMagic;
  req.version = trigger_proto::kTriggerVersion;
  req.priority_hint = priority_hint;
  req.sequence = NextSequence();
  req.response_timeout_ms = wire_ms;
  req.request_id = NextRequestId();
  if (timestamp_ns != 0) {
    req.flags |= trigger_proto::kFlagHasTimestamp;
    req.timestamp_ns = timestamp_ns;
  }
  if (force)
    req.flags |= trigger_proto::kFlagForce;
  std::snprintf(req.source, sizeof(req.source), "%s", source);
  trigger_proto::StampRequestCrc(&req);
  return RoundTrip(socket_path, req, wire_ms);
#endif
}

// TRG-07: cancel a pending (accepted, not yet started) or running capture.
// Codes: kCancelled (applied), kUnknownTrigger (unknown/completed id — an
// explicit rejection, not silent success), kBusy (the rollback hook failed
// and the capture keeps running; retry). kTransportTimeout/kRecorderOffline
// keep their transport meanings.
inline Result Cancel(const char* socket_path, uint64_t trigger_id,
                     uint64_t timeout_ns = kDefaultTimeoutNs) {
  Result out;
#if defined(_WIN32)
  (void)socket_path;
  (void)trigger_id;
  (void)timeout_ns;
  return out;
#else
  if (socket_path == nullptr || trigger_id == 0)
    return out;
  const uint32_t wire_ms = WireTimeoutMs(timeout_ns);

  trigger_proto::TriggerRequestV4 req;
  req.magic = trigger_proto::kTriggerMagic;
  req.version = trigger_proto::kTriggerVersion;
  // kFlagHasTimestamp is required by the server for cancel; it also makes a
  // pre-TRG-07 server reject the request as kInvalidTimestamp (a trigger id
  // is far below CLOCK_MONOTONIC ns) instead of recording a bogus capture.
  req.flags = trigger_proto::kFlagCancel | trigger_proto::kFlagHasTimestamp;
  req.timestamp_ns = trigger_id;
  req.sequence = NextSequence();
  req.response_timeout_ms = wire_ms;
  req.request_id = NextRequestId();
  trigger_proto::StampRequestCrc(&req);
  return RoundTrip(socket_path, req, wire_ms);
#endif
}

// TRG-07: block until the center reports the outcome of trigger_id
// (0 = the next outcome of any capture) or the deadline lapses. A delivered
// outcome has transport_ok=true and kRespFlagOutcomeNotification in
// response_flags; `code` is kCaptureSealed / kPreemptedByHigher /
// kStartFailed / kPreemptFailed / kCancelled and `trigger_id` the capture it
// describes. kBusy (no notification flag) means the server's waiter table is
// full — retry. A lapsed deadline is kTransportTimeout: the capture may still
// finish later, so re-wait or fall back to the package directory.
inline Result WaitOutcome(const char* socket_path, uint64_t trigger_id,
                          uint64_t timeout_ns = kDefaultTimeoutNs) {
  Result out;
#if defined(_WIN32)
  (void)socket_path;
  (void)trigger_id;
  (void)timeout_ns;
  return out;
#else
  if (socket_path == nullptr)
    return out;
  const uint32_t wire_ms = WireTimeoutMs(timeout_ns);

  trigger_proto::TriggerRequestV4 req;
  req.magic = trigger_proto::kTriggerMagic;
  req.version = trigger_proto::kTriggerVersion;
  req.flags =
      trigger_proto::kFlagWaitOutcome | trigger_proto::kFlagHasTimestamp;
  // kWaitFilterAny is the on-wire "any" encoding (0 stays a valid id filter).
  req.timestamp_ns =
      trigger_id == 0 ? trigger_proto::kWaitFilterAny : trigger_id;
  req.sequence = NextSequence();
  req.response_timeout_ms = wire_ms;
  req.request_id = NextRequestId();
  trigger_proto::StampRequestCrc(&req);
  // A deferred wait has no immediate response to cache. Keep one attempt; a
  // caller may issue a fresh wait after timeout instead of duplicating a waiter.
  return RoundTrip(socket_path, req, wire_ms, 1);
#endif
}

}  // namespace trigger_client
}  // namespace thyme
