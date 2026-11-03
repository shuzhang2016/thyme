#pragma once

// trigger_protocol.h — wire format of the trigger-center Unix datagram API
// (design doc 5.1). Little-endian fixed structs; both sides are thyme
// components on Linux, same as record_control.
//
// Request V4: fixed-size TriggerRequestV4 (source is a fixed 64-byte field per
// the design). V2 appended an anti-replay `sequence`; V3 (SEC-04) appended the
// response deadline and CRC32. V4 (TRG-19) appends request_id and a new CRC32,
// turning the V3 CRC slots into reserved zero fields so every old offset stays
// unchanged. The server accepts V3/V4; V1 (88 B, no sequence) and V2 (96 B, no
// CRC) retain their explicit compat flags and never gain idempotent retry.
//
// Response: TriggerResponseV4 carries the request_id, decision, effective
// priority and flags, with a CRC32 trailer. V1/V2/V3 replies keep their exact
// legacy layouts. A duplicate V4 request returns the cached first decision with
// kRespFlagDeduplicated instead of executing the operation again.
//
// E2E profile for control messages (SEC-04)
// -----------------------------------------
// * CRC32 (IEEE 802.3, the same Crc32() used by FrameHeader) over every
//   request/response byte preceding the `crc32` field. The server validates
//   the request CRC BEFORE sequence/replay and priority handling and answers
//   a bad CRC as kMalformedRequest (audited, no reply — the same policy as
//   any other malformed datagram). The client validates the response CRC.
// * Monotonic sequence per (uid, pid): the client seeds it from
//   CLOCK_MONOTONIC; the server keeps a 64-entry sliding window and rejects
//   0/duplicate/too-old values with kReplayRejected.
// * Response deadline: `response_timeout_ms` is the caller's declared E2E
//   deadline (1..60000 ms); the server answers synchronously within one
//   Tick, and the client enforces the deadline with CLOCK_MONOTONIC, failing
//   the call with kTransportTimeout (distinct from kRecorderOffline). The
//   field is advisory to the server (it validates the range but has no
//   reply queue to service later).
//
// Compatibility (docs/reference/abi-compatibility.md rule 1): appending a field
// is a layout change, so kTriggerVersion is 4. Datagram size selects V1/V2/V3/
// V4 and the version must match. Rejected V1/V2 requests get their matching
// 16/24-byte response; V3 keeps its 32-byte CRC response and replay policy.
//
// TRG-07 (same V3 layout — the flags field is the extension point, sizes are
// unchanged): kFlagCancel turns the request into Cancel(timestamp_ns =
// trigger_id); kFlagWaitOutcome registers a deferred outcome waiter
// (timestamp_ns = trigger_id filter, kWaitFilterAny = any) whose reply arrives
// later, within response_timeout_ms. Both require kFlagHasTimestamp so a
// pre-TRG-07 server rejects them as kInvalidTimestamp (a trigger id is far
// below CLOCK_MONOTONIC ns, and kWaitFilterAny is far above the future
// tolerance) instead of recording a bogus capture. Because flags is the
// extension point for a future opcode, any *unknown* flag bit is rejected as
// malformed on every version (V1/V2/V3): a server must never record a trigger
// for a request whose semantics it does not implement.

#include "thyme/contracts/frame_header.h"

#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace thyme {
namespace trigger_proto {

inline constexpr uint32_t kTriggerMagic = 0x47495254u;  // "TRIG" (LE)

// V1: no sequence/nonce. V2: sequence. V3: deadline + CRC. V4: request_id.
inline constexpr uint16_t kTriggerVersionLegacy = 1;
inline constexpr uint16_t kTriggerVersionV2 = 2;
inline constexpr uint16_t kTriggerVersionV3 = 3;
inline constexpr uint16_t kTriggerVersion = 4;

// SEC-04 response deadline bounds (V3 request field).
inline constexpr uint32_t kDefaultResponseTimeoutMs = 100;
inline constexpr uint32_t kMinResponseTimeoutMs = 1;
inline constexpr uint32_t kMaxResponseTimeoutMs = 60000;

inline constexpr uint16_t kFlagHasTimestamp = 1u << 0;
inline constexpr uint16_t kFlagForce = 1u << 1;  // manual-confirmation preempt
// TRG-07: cancel the pending/running capture whose id is in timestamp_ns
// (kFlagHasTimestamp must also be set — see the header comment). source is
// ignored. Reply: kCancelled / kUnknownTrigger / kBusy (rollback failed,
// retry). V3 only.
inline constexpr uint16_t kFlagCancel = 1u << 2;
// TRG-07: wait for the outcome of a capture instead of submitting one.
// timestamp_ns is the trigger_id filter (kWaitFilterAny = next outcome of any
// capture); kFlagHasTimestamp must be set. The reply is deferred until a
// matching outcome is sealed or the response_timeout_ms budget lapses (the
// client then reports kTransportTimeout). Waiter-table overflow is answered
// immediately with kBusy. V3 only.
inline constexpr uint16_t kFlagWaitOutcome = 1u << 3;
// On-wire encoding of the "any trigger" WaitOutcome filter. Not a valid
// trigger id (ids start at 1), and rejected by pre-TRG-07 servers as an
// out-of-bounds timestamp.
inline constexpr uint64_t kWaitFilterAny = ~0ull;

// Response flags (V2 onwards).
inline constexpr uint32_t kRespFlagPriorityClamped = 1u << 0;
// TRG-07: this response is an outcome notification answering a
// kFlagWaitOutcome request; `code` carries the outcome (kCaptureSealed,
// kPreemptedByHigher, kStartFailed, kPreemptFailed, kCancelled).
inline constexpr uint32_t kRespFlagOutcomeNotification = 1u << 1;
// TRG-19: the V4 response came from the bounded dedup window; the operation
// was not executed a second time. V1-V3 never set this bit.
inline constexpr uint32_t kRespFlagDeduplicated = 1u << 2;

// Legacy (V1) request: 88 bytes. Kept only so the server can recognise and
// explicitly reject/accept old peers instead of mis-parsing them.
struct TriggerRequestV1 {
  uint32_t magic = 0;
  uint16_t version = 0;
  uint16_t flags = 0;
  int32_t priority_hint = -1;   // negative = use the configured default
  uint64_t timestamp_ns = 0;    // valid only with kFlagHasTimestamp
  char source[64] = {};         // trigger source name, NUL-terminated
};

// Legacy (V2) request: 96 bytes, no CRC trailer.
struct TriggerRequestV2 {
  uint32_t magic = 0;
  uint16_t version = 0;
  uint16_t flags = 0;
  int32_t priority_hint = -1;
  uint64_t timestamp_ns = 0;
  char source[64] = {};
  // Per-sender anti-replay sequence (V2). The client initialises it from
  // CLOCK_MONOTONIC and increments per call, so it is monotonic across calls
  // and across process restarts. 0 is reserved/invalid. The server keeps a
  // per-(uid,pid) sliding window.
  uint64_t sequence = 0;
};

// V3 request. The unsuffixed name is retained for source compatibility.
struct TriggerRequest {
  uint32_t magic = 0;
  uint16_t version = 0;
  uint16_t flags = 0;
  int32_t priority_hint = -1;   // negative = use the configured default
  uint64_t timestamp_ns = 0;    // valid only with kFlagHasTimestamp
  char source[64] = {};         // trigger source name, NUL-terminated
  uint64_t sequence = 0;        // anti-replay (see V2)
  // Caller's E2E response deadline in milliseconds (1..60000). Advisory to
  // the server, enforced by the client with CLOCK_MONOTONIC.
  uint32_t response_timeout_ms = kDefaultResponseTimeoutMs;
  // CRC32 over every byte before this field (see RequestCrc).
  uint32_t crc32 = 0;
};
using TriggerRequestV3 = TriggerRequest;

// Current V4 request. reserved0 occupies V3's CRC offset and must be zero.
struct TriggerRequestV4 {
  uint32_t magic = 0;
  uint16_t version = 0;
  uint16_t flags = 0;
  int32_t priority_hint = -1;
  uint64_t timestamp_ns = 0;
  char source[64] = {};
  uint64_t sequence = 0;
  uint32_t response_timeout_ms = kDefaultResponseTimeoutMs;
  uint32_t reserved0 = 0;
  // Stable across bounded retries of one logical operation; nonzero.
  uint64_t request_id = 0;
  uint32_t crc32 = 0;
};

enum class TriggerCode : uint16_t {
  kAccepted = 0,
  kMuted = 1,               // rejected while CAPTURING (low/equal priority) or MUTED
  kQuotaExceeded = 2,       // count or memory quota
  kUnknownSource = 3,
  // TRG-07 enabled: emitted as an outcome notification when a running capture
  // is truncated+sealed by a higher-priority preempt. Notification-only; never
  // a Submit reply (§3.19: no late error codes on TriggerResponse).
  kPreemptedByHigher = 4,
  kRecorderOffline = 5,     // also: client-side socket/setup failure
  kBusy = 6,                // accepted-not-started trigger holds the queue / preempting; retry
  kInvalidTimestamp = 7,    // timestamp_ns out of bounds (too old = all holes / too future)
  kUnsupportedVersion = 8,  // V1/V2 request while legacy peers are disabled
  kReplayRejected = 9,      // duplicate/old sequence (SEC-02)
  kAuthRejected = 10,       // audit-only: peer uid/pid not allowed (no reply)
  kMalformedRequest = 11,   // audit-only: bad size/magic/version/CRC (no reply)
  // SEC-04: no reply within the client-declared deadline (client-side only;
  // the server never sends it).
  kTransportTimeout = 12,
  // TRG-07: cancel applied (Cancel reply) and the notification outcome of a
  // cancelled capture.
  kCancelled = 13,
  // TRG-07: Cancel target is neither the pending nor the running capture
  // (unknown or already completed). Explicit rejection, never silent success.
  kUnknownTrigger = 14,
  // TRG-07 notification outcome: the capture finished and was sealed.
  kCaptureSealed = 15,
  // TRG-07 notification outcome: the deferred start_capture failed (T6
  // rollback already happened; see CenterStats::start_failures).
  kStartFailed = 16,
  // TRG-07 notification outcome: the deferred preempt failed and the accepted
  // trigger was dropped (CenterStats::preempt_failures).
  kPreemptFailed = 17,
};

// V1 response: 16 bytes. The server uses this layout when it answers a V1
// request, so old clients keep working (only with legacy peers enabled).
struct TriggerResponseV1 {
  uint32_t magic = 0;
  uint16_t version = 0;
  uint16_t code = 0;
  uint64_t trigger_id = 0;
};

// V2 response: 24 bytes, no CRC trailer.
struct TriggerResponseV2 {
  uint32_t magic = 0;
  uint16_t version = 0;
  uint16_t code = 0;
  uint64_t trigger_id = 0;
  // Priority actually used by the FSM after the [security] clamp + the rule's
  // "hint may only lower" rule. -1 is never returned on an accepted reply.
  int32_t effective_priority = 0;
  uint32_t flags = 0;  // kRespFlagPriorityClamped
};

// V3 response. The unsuffixed name is retained for source compatibility.
struct TriggerResponse {
  uint32_t magic = 0;
  uint16_t version = 0;
  uint16_t code = 0;
  uint64_t trigger_id = 0;
  int32_t effective_priority = 0;
  uint32_t flags = 0;
  uint32_t reserved0 = 0;
  uint32_t crc32 = 0;
};
using TriggerResponseV3 = TriggerResponse;

// Current V4 response. reserved1 occupies V3's CRC offset and must be zero.
struct TriggerResponseV4 {
  uint32_t magic = 0;
  uint16_t version = 0;
  uint16_t code = 0;
  uint64_t trigger_id = 0;
  int32_t effective_priority = 0;
  uint32_t flags = 0;
  uint32_t reserved0 = 0;
  uint32_t reserved1 = 0;
  uint64_t request_id = 0;
  uint32_t crc32 = 0;
};

// ---- E2E CRC helpers ------------------------------------------------------
inline uint32_t RequestCrc(const TriggerRequest& r) {
  return Crc32(reinterpret_cast<const uint8_t*>(&r),
               offsetof(TriggerRequest, crc32));
}

inline void StampRequestCrc(TriggerRequest* r) { r->crc32 = RequestCrc(*r); }

inline uint32_t RequestCrcV4(const TriggerRequestV4& r) {
  return Crc32(reinterpret_cast<const uint8_t*>(&r),
               offsetof(TriggerRequestV4, crc32));
}

inline void StampRequestCrc(TriggerRequestV4* r) { r->crc32 = RequestCrcV4(*r); }

inline uint32_t ResponseCrc(const TriggerResponse& r) {
  return Crc32(reinterpret_cast<const uint8_t*>(&r),
               offsetof(TriggerResponse, crc32));
}

inline void StampResponseCrc(TriggerResponse* r) { r->crc32 = ResponseCrc(*r); }

inline uint32_t ResponseCrcV4(const TriggerResponseV4& r) {
  return Crc32(reinterpret_cast<const uint8_t*>(&r),
               offsetof(TriggerResponseV4, crc32));
}

inline void StampResponseCrc(TriggerResponseV4* r) {
  r->crc32 = ResponseCrcV4(*r);
}

static_assert(sizeof(TriggerRequestV1) == 88, "TriggerRequestV1 wire size");
static_assert(sizeof(TriggerRequestV2) == 96, "TriggerRequestV2 wire size");
static_assert(sizeof(TriggerRequest) == 104, "TriggerRequest wire size");
static_assert(sizeof(TriggerRequestV4) == 120, "TriggerRequestV4 wire size");
static_assert(sizeof(TriggerResponseV1) == 16, "TriggerResponseV1 wire size");
static_assert(sizeof(TriggerResponseV2) == 24, "TriggerResponseV2 wire size");
static_assert(sizeof(TriggerResponse) == 32, "TriggerResponse wire size");
static_assert(sizeof(TriggerResponseV4) == 48, "TriggerResponseV4 wire size");
// Each version must stay a pure append of the previous one: the common
// prefix is the same bytes.
static_assert(offsetof(TriggerRequestV1, source) ==
                  offsetof(TriggerRequestV2, source),
              "V2 appends fields only");
static_assert(offsetof(TriggerRequestV2, source) ==
                  offsetof(TriggerRequest, source),
              "V3 appends fields only");
static_assert(offsetof(TriggerRequestV2, sequence) ==
                  offsetof(TriggerRequest, sequence),
              "V3 appends fields only");
static_assert(offsetof(TriggerRequest, response_timeout_ms) == 96,
              "V3 timeout follows the V2 struct");
static_assert(offsetof(TriggerRequest, crc32) == 100, "V3 CRC is the trailer");
static_assert(offsetof(TriggerRequestV4, response_timeout_ms) == 96,
              "V4 preserves the V3 prefix");
static_assert(offsetof(TriggerRequestV4, reserved0) == 100,
              "V4 reserves the V3 CRC slot");
static_assert(offsetof(TriggerRequestV4, request_id) == 104,
              "V4 request id follows the V3 layout");
static_assert(offsetof(TriggerRequestV4, crc32) == 112,
              "V4 CRC is the trailer");
static_assert(offsetof(TriggerResponseV1, trigger_id) ==
                  offsetof(TriggerResponseV2, trigger_id),
              "V2 response appends fields only");
static_assert(offsetof(TriggerResponseV2, effective_priority) ==
                  offsetof(TriggerResponse, effective_priority),
              "V3 response appends fields only");
static_assert(offsetof(TriggerResponse, crc32) == 28,
              "V3 response CRC is the trailer");
static_assert(offsetof(TriggerResponseV4, reserved1) == 28,
              "V4 reserves the V3 response CRC slot");
static_assert(offsetof(TriggerResponseV4, request_id) == 32,
              "V4 response request id follows the V3 layout");
static_assert(offsetof(TriggerResponseV4, crc32) == 40,
              "V4 response CRC is the trailer");
static_assert(std::is_standard_layout<TriggerRequest>::value &&
                  std::is_trivially_copyable<TriggerRequest>::value,
              "TriggerRequest crosses processes");
static_assert(std::is_standard_layout<TriggerRequestV4>::value &&
                  std::is_trivially_copyable<TriggerRequestV4>::value,
              "TriggerRequestV4 crosses processes");
static_assert(std::is_standard_layout<TriggerRequestV1>::value &&
                  std::is_trivially_copyable<TriggerRequestV1>::value,
              "TriggerRequestV1 crosses processes");
static_assert(std::is_standard_layout<TriggerRequestV2>::value &&
                  std::is_trivially_copyable<TriggerRequestV2>::value,
              "TriggerRequestV2 crosses processes");
static_assert(std::is_standard_layout<TriggerResponse>::value &&
                  std::is_trivially_copyable<TriggerResponse>::value,
              "TriggerResponse crosses processes");
static_assert(std::is_standard_layout<TriggerResponseV4>::value &&
                  std::is_trivially_copyable<TriggerResponseV4>::value,
              "TriggerResponseV4 crosses processes");
static_assert(std::is_standard_layout<TriggerResponseV1>::value &&
                  std::is_trivially_copyable<TriggerResponseV1>::value,
              "TriggerResponseV1 crosses processes");
static_assert(std::is_standard_layout<TriggerResponseV2>::value &&
                  std::is_trivially_copyable<TriggerResponseV2>::value,
              "TriggerResponseV2 crosses processes");

}  // namespace trigger_proto
}  // namespace thyme
