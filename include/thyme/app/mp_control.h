#pragma once
#include "thyme/runtime/shm_page.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <type_traits>

namespace thyme {
namespace app {

// Shared control page for a supervised multi-process graph. Lives in SHM
// (created by the supervisor, attached by each role); every field is a
// lock-free atomic so cross-process writers/readers make progress.
//
// ARCH-11 v3: the page is self-describing. Offset 0 is the 64 B
// ShmSuperHeader (magic@0 "THYME_MP", version@8, layout_hash/page_bytes/
// feature_bits, reserved-zero); all control counters follow it. The supervisor
// creates it with ShmRegion::CreateDescribed(MpControl::Spec()) and roles
// attach with OpenDescribed, so magic/layout/page-size/feature/reserved
// mismatches fail with a diagnostic instead of being read at guessed offsets.
//
// RT-10 (docs/reference/cacheline-audit.md): this is a low-frequency counter page, not a
// per-frame hot line — it is intentionally NOT padded to one field per line
// (that would cost ~1.5 KB and buy nothing: no field here is touched per
// frame). Scalars are grouped in the first cache line after the super-header,
// followed by the two fixed counter arrays. sizeof/offsets are page ABI.
struct alignas(64) MpControl {
  static constexpr std::size_t kMaxProcSlots = 16;
  static constexpr std::size_t kMaxCrossFrameSlots = 8;
  static constexpr uint64_t kMagic = 0x5448594D455F4D50ULL;  // "THYME_MP"
  // SHM layout version (S-D3: bump only, no dual-read). Runs never mix
  // layouts: SHM names embed the supervisor pid, so a stale peer cannot
  // attach. v3 adds the super-header/magic (v2 had a bare abi_version word at
  // offset 0), moves every counter and widens nothing in this page.
  //
  // OPS-02/ARCH-17: the layout deliberately has no config-hash field -- adding
  // one would change the wire format. Attachers validate this page's magic +
  // abi_version + layout_hash + page_bytes + feature_bits and
  // SharedHealthPage::Header::session_id (checked against the pid in the name
  // prefix); the full config/graph identity lives in the separate
  // `<prefix>_id` MpSessionIdentityPage, which roles attach read-only. A
  // pre-ARCH-17 session has no identity page and falls back to these header
  // checks with an explicit warning.
  static constexpr uint32_t kMpControlAbiVersion = 3;
  // No optional behaviour is negotiated today; attach requires equality.
  static constexpr uint64_t kFeatureBits = 0;

  ShmSuperHeader super;

  // Control scalars: one cache line of infrequently written words.
  std::atomic<uint32_t> stop{0};
  // Module -> supervisor trigger relay (not mirror DATA). The supervisor
  // forwards to the recorder Unix socket as a "trigger" datagram.
  std::atomic<uint32_t> trigger_req_seq{0};
  std::atomic<uint32_t> trigger_ack_seq{0};
  std::atomic<uint32_t> trigger_reason_code{0};
  std::atomic<uint64_t> trigger_relay_sent{0};
  std::atomic<uint64_t> trigger_relay_debounced{0};

  // Per-process loop counters (OPS-01 success gate).
  std::atomic<uint64_t> proc_loops[kMaxProcSlots]{};

  // Generic cross-process frame counters, one per gated channel. gen_graph
  // emits the channel -> slot binding (kGraphCrossGates) from graph.yaml
  // edges annotated mp_gate: true; the consumer role's ShmLatestSource
  // increments its slot per received frame.
  std::atomic<uint64_t> cross_frames[kMaxCrossFrameSlots]{};

  // Explicit tail so the page size is stable and reserved words are
  // validated as zero (a future field must take one of these, not move an
  // existing counter).
  uint64_t reserved[4] = {};

  static constexpr ShmPageSpec Spec();

  uint64_t magic() const {
    return super.magic.load(std::memory_order_acquire);
  }
  uint32_t abi_version() const {
    return super.abi_version.load(std::memory_order_acquire);
  }
  uint64_t layout_hash() const {
    return super.layout_hash.load(std::memory_order_acquire);
  }
  uint64_t page_bytes() const {
    return super.page_bytes.load(std::memory_order_acquire);
  }
  uint64_t feature_bits() const {
    return super.feature_bits.load(std::memory_order_acquire);
  }
  uint32_t stop_requested() const {
    return stop.load(std::memory_order_acquire);
  }

  // Initialize the super-header. The region is zero-filled, so every counter
  // starts at 0; magic is release-stored last.
  void Init() {
    super.magic.store(0, std::memory_order_release);
    for (uint64_t& value : reserved) value = 0;
    InitShmSuperHeader(&super, Spec());
  }

  bool Validate(ShmPageError* error = nullptr) const {
    if (ValidateShmSuperHeader(this, sizeof(*this), Spec(), error) !=
        ShmPageErrc::kOk) {
      return false;
    }
    for (const uint64_t value : reserved) {
      if (value != 0) {
        if (error != nullptr) {
          *error = ShmPageError{ShmPageErrc::kBadReserved, 0, value};
        }
        return false;
      }
    }
    return true;
  }
};

static_assert(std::atomic<uint32_t>::is_always_lock_free &&
                  std::atomic<uint64_t>::is_always_lock_free,
              "MpControl lives in SHM: its atomics must be lock-free");

// MpControl page layout, kMpControlAbiVersion == 3 (S-D3: bump only, no
// dual-read; SHM names embed the supervisor pid). Supervisors create the page
// at sizeof(MpControl) and roles validate the full super-header before reading
// any counter, so a field move is a version bump
// (docs/reference/abi-compatibility.md).
static_assert(sizeof(MpControl) == 320,
              "MpControl page size is the supervisor's SHM size");
static_assert(alignof(MpControl) == 64,
              "MpControl starts with the 64 B super-header");
static_assert(offsetof(MpControl, super) == 0, "super-header leads the page");
static_assert(offsetof(MpControl, super) + offsetof(ShmSuperHeader, magic) == 0,
              "page magic stays at offset 0");
static_assert(offsetof(ShmSuperHeader, abi_version) == 8, "version at 8");
static_assert(offsetof(MpControl, stop) == 64,
              "control scalars follow the super-header");
static_assert(offsetof(MpControl, trigger_req_seq) == 68,
              "trigger relay block offset is page ABI");
static_assert(offsetof(MpControl, trigger_ack_seq) == 72,
              "trigger relay block offset is page ABI");
static_assert(offsetof(MpControl, trigger_reason_code) == 76,
              "trigger relay block offset is page ABI");
static_assert(offsetof(MpControl, trigger_relay_sent) == 80,
              "trigger relay counters offset is page ABI");
static_assert(offsetof(MpControl, trigger_relay_debounced) == 88,
              "trigger relay counters offset is page ABI");
static_assert(offsetof(MpControl, proc_loops) == 96,
              "proc_loops offset is page ABI");
static_assert(offsetof(MpControl, cross_frames) == 224,
              "cross_frames offset is page ABI");
static_assert(offsetof(MpControl, reserved) == 288,
              "the explicit tail starts after both counter arrays");

inline constexpr uint64_t MpControlLayoutHash() {
  return ShmLayoutHash(
      0x4D504333ULL /* "MPC3" */, MpControl::kMpControlAbiVersion,
      sizeof(MpControl), alignof(MpControl), offsetof(MpControl, super),
      sizeof(ShmSuperHeader), offsetof(MpControl, stop),
      sizeof(std::atomic<uint32_t>), offsetof(MpControl, trigger_req_seq),
      sizeof(std::atomic<uint32_t>), offsetof(MpControl, trigger_ack_seq),
      sizeof(std::atomic<uint32_t>), offsetof(MpControl, trigger_reason_code),
      sizeof(std::atomic<uint32_t>), offsetof(MpControl, trigger_relay_sent),
      sizeof(std::atomic<uint64_t>),
      offsetof(MpControl, trigger_relay_debounced),
      sizeof(std::atomic<uint64_t>), offsetof(MpControl, proc_loops),
      sizeof(MpControl::proc_loops), MpControl::kMaxProcSlots,
      offsetof(MpControl, cross_frames), sizeof(MpControl::cross_frames),
      MpControl::kMaxCrossFrameSlots, offsetof(MpControl, reserved),
      sizeof(MpControl::reserved));
}
static_assert(MpControlLayoutHash() != 0,
              "0 is reserved for an unknown layout identity");

inline constexpr ShmPageSpec MpControl::Spec() {
  return ShmPageSpec{MpControl::kMagic, MpControl::kMpControlAbiVersion,
                     MpControlLayoutHash(), sizeof(MpControl),
                     MpControl::kFeatureBits};
}

// Compose a shared-memory name from a process prefix and a suffix
// ("<prefix><suffix>"), same convention on the supervisor and role sides.
inline void MpName(char* out, std::size_t cap, const char* prefix,
                   const char* suffix) {
  std::snprintf(out, cap, "%s%s", prefix, suffix);
}

// How long a role waits for the supervisor's SHM objects to appear.
inline constexpr uint64_t kMpOpenTimeoutNs = 1000000000ULL;
// How long the supervisor waits for an already-forked child on the fork
// failure path before escalating to SIGKILL.
inline constexpr uint64_t kMpWaitTimeoutNs = 2000000000ULL;

}  // namespace app
}  // namespace thyme
