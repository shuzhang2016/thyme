#pragma once
// pinned_slot_store.h — payload-agnostic large-frame SHM channel (triggered
// acquisition, design doc 4.3). Unifies the contracts repo's cloud_channel.h
// and image_channel.h pattern (per-slot seqlock + monotonic head + futex
// epoch, Begin/Commit direct-to-shm pack) into one thyme runtime component
// whose slot_bytes / slot_count / meta_blob_bytes are runtime Create
// parameters — cloud (MB-scale variable), image (fixed NV12), and h264
// (64 KB x many slots) are parameterizations of the same component.
//
// Region layout (single shm region, variable size — size it with
// PinnedSlotWriter::RegionBytes):
//
//   [PinnedSlotPageHeader]
//   [meta blob: round_up(meta_blob_bytes, 64)]   channel-level metadata
//   [slot x slot_count], slot_stride bytes each:
//       [PinnedSlotHeader 64B: seq(seqlock)/mono_ns/data_len/flags/crc32]
//       [payload area: round_up(slot_bytes, 64)]
//       [PinnedSlotFooter 64B: magic + frame_index — written LAST by Commit as
//        the payload-complete marker]
//
// Synchronization mirrors the existing channels: slot seqlock 0 = write in
// progress, else publish index + 1; the footer magic validates completion the
// same way the camera footer does. Single producer: no interleaving of
// Begin/Commit pairs. Readers are follow-mode with a per-reader cursor and
// hand out zero-copy views into the slot.
//
// Per-slot lease (roadmap §3.18, lease ABI v5): a separate shm object
// "<name>.lease" holds the lease table (one 64B cell per slot: pin count,
// copy-active arbitration, owner identity + last-pin epoch). Readers map the
// channel payload region READ-ONLY and the lease region READ-WRITE; the writer
// maps both RW. Peer version must match exactly — lease and no-lease processes
// must never mix on one channel, and v2/v3/v4 peers are rejected because their
// epoch/copy invariants differ. Lifetime rule (P3-8): every
// PinnedSlotReader::Lease must be destroyed/Reset before its reader's Close();
// debug builds abort on a violation.
//
// BF-12..BF-14 layering: application code should use PinnedSlotReader::Next()/
// NextLease() (a move-only BorrowedFrame with data/size/mono_ns/index,
// still_valid() and a Release() result); Lease/AcquireLease/ReleaseLease stay
// for recorder/probe code that needs random access or raw generations.
// Snapshot metadata is HeaderSnapshot (CopyTo copies no payload bytes);
// payload retention across a lease goes through CopyPayloadTo() or
// ReadFrameStable().

#include "thyme/contracts/frame_header.h"
#include "thyme/runtime/pinned_slot_footer.h"
#include "thyme/runtime/pinned_slot_gate.h"
#include "thyme/runtime/pinned_slot_lease.h"
#include "thyme/runtime/detail/atomic_payload.h"
#include "thyme/runtime/futex.h"
#include "thyme/runtime/shm_region.h"
#include "thyme/runtime/waitable.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <type_traits>
#include <utility>
#include <unistd.h>

namespace thyme {

inline constexpr uint64_t kPinnedSlotStoreMagic =
    0x5448594D42464331ULL;  // "THYMBFC1"
inline constexpr uint32_t kPinnedSlotStoreVersion =
    4;  // v4: ARCH-11 super-header (fixed prefix; meta blob + slots follow);
        // v3 added the REP-10 producer gate. v1..v3 are rejected.

struct alignas(64) PinnedSlotPageHeader {
  static constexpr uint64_t kMagic = kPinnedSlotStoreMagic;
  static constexpr uint32_t kVersion = kPinnedSlotStoreVersion;

  ShmSuperHeader super;
  alignas(64) std::atomic<uint64_t> magic{0};
  std::atomic<uint32_t> version{0};
  std::atomic<uint32_t> slot_count{0};
  std::atomic<uint32_t> slot_bytes{0};  // max payload bytes per slot
  std::atomic<uint32_t> slot_stride{0};
  std::atomic<uint32_t> meta_blob_bytes{0};
  std::atomic<uint32_t> flags{0};  // kPinnedSlotFlag*
  std::atomic<uint64_t> dropped_oversize{0};  // Begin(len > slot_bytes)
  alignas(64) std::atomic<uint64_t> head{0};  // total frames published
  alignas(64) std::atomic<uint32_t> epoch{0};  // futex word
  // REP-10: replay producer gate. Writers consult it in Begin()/Commit(); the
  // replay holder (PinnedSlotInjector) owns it with a heartbeat lease.
  alignas(64) ProducerGate gate;
  // Commits aborted because a replay gate appeared between Begin and Commit
  // (a producer that started before the replay must not publish late).
  std::atomic<uint64_t> gate_aborted{0};
  // Meta blob (round_up(meta_blob_bytes, 64) bytes) follows this struct,
  // then slot_count slots of slot_stride bytes each.

  static constexpr uint64_t LayoutHash() {
    return ShmLayoutHash(
        kMagic, kVersion, static_cast<uint64_t>(sizeof(PinnedSlotPageHeader)),
        static_cast<uint64_t>(alignof(PinnedSlotPageHeader)),
        static_cast<uint64_t>(offsetof(PinnedSlotPageHeader, super)),
        static_cast<uint64_t>(offsetof(PinnedSlotPageHeader, slot_count)),
        static_cast<uint64_t>(offsetof(PinnedSlotPageHeader, slot_bytes)),
        static_cast<uint64_t>(offsetof(PinnedSlotPageHeader, slot_stride)),
        static_cast<uint64_t>(offsetof(PinnedSlotPageHeader,
                                      meta_blob_bytes)),
        static_cast<uint64_t>(offsetof(PinnedSlotPageHeader, flags)),
        static_cast<uint64_t>(offsetof(PinnedSlotPageHeader, head)),
        static_cast<uint64_t>(offsetof(PinnedSlotPageHeader, epoch)),
        static_cast<uint64_t>(offsetof(PinnedSlotPageHeader, gate)),
        static_cast<uint64_t>(offsetof(PinnedSlotPageHeader, gate_aborted)),
        static_cast<uint64_t>(sizeof(ProducerGate)));
  }

  // page_bytes covers the fixed header only; the meta blob and slots are
  // appended at runtime (same pattern as RecordMirrorRingPage's arena).
  static constexpr ShmPageSpec Spec() {
    return ShmPageSpec{kMagic, kVersion, LayoutHash(),
                       static_cast<uint64_t>(sizeof(PinnedSlotPageHeader)), 0};
  }
};

// Channel page layout, kPinnedSlotStoreVersion == 4 (docs/reference/abi-compatibility.md).
// MetaOffset() == sizeof(PinnedSlotPageHeader) and SlotsOffset() build on it,
// so the size IS the wire offset of the meta blob — changing either requires a
// version bump. PinnedSlotReader opens the described fixed prefix before
// remapping the full region.
static_assert(sizeof(PinnedSlotPageHeader) == 384,
              "channel header is 6 cache lines (meta blob follows at 384)");
static_assert(offsetof(PinnedSlotPageHeader, super) == 0,
              "super-header leads the channel page");
static_assert(offsetof(PinnedSlotPageHeader, version) == 72,
              "version offset is channel ABI");
static_assert(offsetof(PinnedSlotPageHeader, slot_count) == 76,
              "geometry offsets are channel ABI");
static_assert(offsetof(PinnedSlotPageHeader, slot_stride) == 84,
              "geometry offsets are channel ABI");
static_assert(offsetof(PinnedSlotPageHeader, meta_blob_bytes) == 88,
              "geometry offsets are channel ABI");
static_assert(offsetof(PinnedSlotPageHeader, dropped_oversize) == 96,
              "dropped_oversize follows the geometry block");
static_assert(offsetof(PinnedSlotPageHeader, head) == 128,
              "head owns its own cache line");
static_assert(offsetof(PinnedSlotPageHeader, epoch) == 192,
              "epoch futex word owns its own cache line");
static_assert(offsetof(PinnedSlotPageHeader, gate) == 256,
              "REP-10 gate owns its own cache line");
static_assert(offsetof(PinnedSlotPageHeader, gate_aborted) == 320,
              "gate_aborted follows the gate line");
static_assert(std::is_standard_layout<PinnedSlotPageHeader>::value &&
                  std::is_trivially_copyable<PinnedSlotPageHeader>::value,
              "PinnedSlotPageHeader must be POD for SHM");
static_assert(std::atomic<uint32_t>::is_always_lock_free &&
                  std::atomic<uint64_t>::is_always_lock_free,
              "PinnedSlotStore lives in SHM: lock-free atomics required");

// RT-09: bounded retry budget of the PinnedSlotReader seqlock loops (NextLease,
// ReadFrame, TryRead). One greppable constant for the retry-loop audit;
// exhaustion reports kTorn/false, never a torn payload.
inline constexpr int kPinnedSlotReadRetries = 16;

class PinnedSlotReader;

// Single-producer side. Created by the producer node (owns the region).
class PinnedSlotWriter {
 public:
  // REP-10: Begin() distinguishes a blocked write (replay owns the channel)
  // from oversize and pinned-slot skips. A null payload with kGated means the
  // replay gate refused the write; nothing was written.
  enum class BeginStatus : uint8_t {
    kOk = 0,
    kOversize,
    kPinnedSkip,
    kGated,
  };

  struct BeginView {
    uint8_t* payload = nullptr;          // slot payload area (data_len bytes)
    PinnedSlotHeader* header = nullptr;  // set ->flags before Commit
    BeginStatus status = BeginStatus::kOk;
  };

  // Create() refuses to replace a channel that a live replay gate protects
  // (kGated) or whose header version is not this build's (kIncompatible).
  enum class CreateStatus : uint8_t {
    kOk = 0,
    kInvalidArgument,
    kCreateFailed,
    kGated,
    kIncompatible,
  };

  PinnedSlotWriter() = default;
  ~PinnedSlotWriter() { Close(); }
  PinnedSlotWriter(const PinnedSlotWriter&) = delete;
  PinnedSlotWriter& operator=(const PinnedSlotWriter&) = delete;

  static constexpr uint32_t RoundUp64(uint32_t n) { return (n + 63u) & ~63u; }

  static constexpr std::size_t SlotStride(uint32_t slot_bytes) {
    return sizeof(PinnedSlotHeader) + RoundUp64(slot_bytes) +
           sizeof(PinnedSlotFooter);
  }

  static constexpr std::size_t MetaOffset() { return sizeof(PinnedSlotPageHeader); }

  static constexpr std::size_t SlotsOffset(uint32_t meta_blob_bytes) {
    return MetaOffset() + RoundUp64(meta_blob_bytes);
  }

  static std::size_t RegionBytes(uint32_t slot_bytes, uint32_t slot_count,
                                 uint32_t meta_blob_bytes) {
    return SlotsOffset(meta_blob_bytes) +
           static_cast<std::size_t>(slot_count) * SlotStride(slot_bytes);
  }

  // BF-01: lease table lives in a separate shm object "<name>.lease" so
  // readers map the payload RO but the lease table RW.
  static std::string LeaseShmName(const char* name) {
    return std::string(name) + ".lease";
  }
  static std::size_t LeaseRegionBytes(uint32_t slot_count) {
    return sizeof(PinnedSlotLeaseHeader) +
           static_cast<std::size_t>(slot_count) * sizeof(PinnedSlotLeaseCell);
  }

  // Single writer: drop a stale region from a previous run, then create.
  // steal_timeout_ns: BF-04 dead-reader lease recovery threshold; 0 is
  // rejected — it would mean "steal immediately", which is almost certainly a
  // config bug (disable steal by passing a huge timeout instead).
  //
  // REP-10: Create() never replaces a channel whose replay producer gate is
  // live — not even with force_replace — because that would detach a running
  // replay's target and let two writers publish. force_replace only overrides
  // the incompatible-version refusal (an explicit upgrade/rebuild decision).
  bool Create(const char* name, uint32_t slot_bytes, uint32_t slot_count,
              uint32_t meta_blob_bytes,
              uint64_t steal_timeout_ns = kPinnedSlotLeaseDefaultStealTimeoutNs,
              bool force_replace = false) {
    create_status_ = CreateStatus::kOk;
    last_error_.clear();
    if (name == nullptr || slot_bytes == 0 || slot_count == 0 ||
        steal_timeout_ns == 0) {
      create_status_ = CreateStatus::kInvalidArgument;
      last_error_ = "invalid channel geometry";
      return false;
    }
    if (!CheckReplaceAllowed(name, force_replace)) {
      return false;
    }
    // OPS-02: verified-stale reclamation belongs to ShmRegion::Create. An
    // unconditional unlink here could detach a live non-replay producer.
    if (!region_.Create(name, RegionBytes(slot_bytes, slot_count,
                                          meta_blob_bytes))) {
      create_status_ = CreateStatus::kCreateFailed;
      last_error_ = "cannot create payload region";
      return false;
    }
    if (!lease_region_.Create(LeaseShmName(name).c_str(),
                              LeaseRegionBytes(slot_count))) {
      region_.Unmap();
      ShmRegion::UnlinkName(name);
      create_status_ = CreateStatus::kCreateFailed;
      last_error_ = "cannot create lease region";
      return false;
    }
    header_ = new (region_.data()) PinnedSlotPageHeader;
    lease_ = new (lease_region_.data()) PinnedSlotLeaseHeader;
    lease_->slot_count.store(slot_count, std::memory_order_relaxed);
    lease_->steal_timeout_ns.store(steal_timeout_ns, std::memory_order_relaxed);
    lease_->version.store(kPinnedSlotLeaseVersion, std::memory_order_relaxed);
    header_->slot_bytes.store(slot_bytes, std::memory_order_relaxed);
    header_->slot_stride.store(static_cast<uint32_t>(SlotStride(slot_bytes)),
                               std::memory_order_relaxed);
    header_->slot_count.store(slot_count, std::memory_order_relaxed);
    header_->meta_blob_bytes.store(meta_blob_bytes, std::memory_order_relaxed);
    // REC-21: transport CRC defaults off (bulk channels rely on the REC-18
    // package manifest; the drain records a payload_crc for on-demand
    // verify). Opt back in with SetCrcEnabled(true).
    header_->flags.store(0, std::memory_order_relaxed);
    header_->version.store(PinnedSlotPageHeader::kVersion,
                           std::memory_order_relaxed);
    // Publish the lease table first, then the channel magic, so a reader that
    // observes a valid channel header also observes a valid lease header.
    InitShmSuperHeader(&lease_->super, PinnedSlotLeaseHeader::Spec());
    InitShmSuperHeader(&header_->super, PinnedSlotPageHeader::Spec());
    create_status_ = CreateStatus::kOk;
    return true;
  }

  // Attach to a channel prepared by the supervisor or another process. The
  // payload and lease mappings are RW; geometry and both super-headers are
  // validated before any producer state is touched.
  bool Open(const char* name, uint64_t timeout_ns) {
    Close();
    ShmPageError error;
    if (!region_.OpenDescribed(name, PinnedSlotPageHeader::Spec(), timeout_ns,
                               /*read_only=*/true, &error)) {
      create_status_ = CreateStatus::kIncompatible;
      return false;
    }
    auto* h = static_cast<const PinnedSlotPageHeader*>(region_.data());
    const uint64_t deadline = MonoNs() + timeout_ns;
    if (h->slot_count.load(std::memory_order_acquire) == 0 ||
        h->slot_bytes.load(std::memory_order_acquire) == 0) {
      region_.Unmap();
      create_status_ = CreateStatus::kIncompatible;
      return false;
    }
    const uint32_t slot_bytes = h->slot_bytes.load(std::memory_order_acquire);
    const uint32_t slot_count = h->slot_count.load(std::memory_order_acquire);
    const uint32_t meta_blob_bytes =
        h->meta_blob_bytes.load(std::memory_order_acquire);
    region_.Unmap();
    const uint64_t remaining = MonoNs() < deadline ? deadline - MonoNs() : 0;
    if (!region_.OpenRetry(name,
                           RegionBytes(slot_bytes, slot_count, meta_blob_bytes),
                           remaining, /*read_only=*/false) ||
        ValidateShmSuperHeader(region_.data(), region_.size(),
                               PinnedSlotPageHeader::Spec(), &error) !=
            ShmPageErrc::kOk) {
      region_.Unmap();
      create_status_ = CreateStatus::kIncompatible;
      return false;
    }
    header_ = static_cast<PinnedSlotPageHeader*>(region_.data());
    const std::string lease_name = LeaseShmName(name);
    if (!lease_region_.OpenDescribed(lease_name.c_str(),
                                     PinnedSlotLeaseHeader::Spec(), remaining,
                                     /*read_only=*/false, &error)) {
      region_.Unmap();
      header_ = nullptr;
      create_status_ = CreateStatus::kIncompatible;
      return false;
    }
    lease_region_.Unmap();
    if (!lease_region_.OpenRetry(lease_name.c_str(), LeaseRegionBytes(slot_count),
                                 remaining, /*read_only=*/false)) {
      region_.Unmap();
      header_ = nullptr;
      create_status_ = CreateStatus::kIncompatible;
      return false;
    }
    auto* lh = static_cast<PinnedSlotLeaseHeader*>(lease_region_.data());
    if (ValidateShmSuperHeader(lease_region_.data(), lease_region_.size(),
                               PinnedSlotLeaseHeader::Spec(), &error) !=
            ShmPageErrc::kOk ||
        lh->slot_count.load(std::memory_order_acquire) != slot_count) {
      lease_region_.Unmap();
      region_.Unmap();
      header_ = nullptr;
      create_status_ = CreateStatus::kIncompatible;
      return false;
    }
    lease_ = lh;
    create_status_ = CreateStatus::kOk;
    return true;
  }

  CreateStatus create_status() const { return create_status_; }
  const std::string& last_error() const { return last_error_; }

  void Close() {
    // REP-10: a replay-held gate is a lease; release it on normal teardown so
    // producers resume immediately (a crash is recovered by writers).
    if (header_ != nullptr && gate_token_ != kProducerGateUnheld) {
      ProducerGateRelease(header_->gate, gate_token_);
    }
    gate_token_ = kProducerGateUnheld;
    header_ = nullptr;
    lease_ = nullptr;
    lease_region_.Unmap();
    region_.Unmap();
  }

  // Lease table access (BF-01); the writer side is RW-mapped.
  PinnedSlotLeaseHeader* lease() { return lease_; }
  const PinnedSlotLeaseHeader* lease() const { return lease_; }

  // Channel-level metadata area (meta_blob_bytes from Create); the producer
  // writes geometry/encoding parameters here once after Create, consumers
  // read it via PinnedSlotReader::MetaBlob().
  void* MetaBlob() {
    return reinterpret_cast<uint8_t*>(header_) + MetaOffset();
  }
  const void* MetaBlob() const {
    return reinterpret_cast<const uint8_t*>(header_) + MetaOffset();
  }

  void SetCrcEnabled(bool enabled) {
    if (header_ == nullptr) return;
    uint32_t f = header_->flags.load(std::memory_order_relaxed);
    f = enabled ? (f | kPinnedSlotFlagCrcEnabled) : (f & ~kPinnedSlotFlagCrcEnabled);
    header_->flags.store(f, std::memory_order_relaxed);
  }

  // ---- REP-10 producer gate -------------------------------------------------

  // The gate lives in the channel header. Null while the writer is closed.
  ProducerGate* gate() { return header_ != nullptr ? &header_->gate : nullptr; }
  const ProducerGate* gate() const {
    return header_ != nullptr ? &header_->gate : nullptr;
  }

  // Take the gate for this writer (replay/host path). The writer becomes the
  // only writer allowed through Begin()/Commit(); other writers get kGated.
  // A live holder yields GateResult::kBusy; a dead/expired one is stolen.
  //
  // The token identifies the *holder*, not the writer object: two injectors
  // sharing one writer object use different tokens, so the second gets kBusy
  // instead of silently sharing the channel.
  GateResult AcquireProducerGateWithNewToken(
      uint64_t timeout_ns = kProducerGateDefaultTimeoutNs) {
    return AcquireProducerGate(ProducerGateNewToken(), timeout_ns);
  }

  GateResult AcquireProducerGate(
      uint64_t token, uint64_t timeout_ns = kProducerGateDefaultTimeoutNs) {
    if (header_ == nullptr || token == kProducerGateUnheld ||
        token == kProducerGateAcquiring) {
      return GateResult::kInvalidArgument;
    }
    const GateResult r = ProducerGateAcquire(header_->gate, token,
                                             ProducerGateNowNs(), timeout_ns);
    if (r == GateResult::kOk || r == GateResult::kStolen) {
      gate_token_ = token;
    }
    return r;
  }

  GateResult ReleaseProducerGate() {
    return ReleaseProducerGate(gate_token_);
  }

  GateResult ReleaseProducerGate(uint64_t token) {
    if (header_ == nullptr || token == kProducerGateUnheld) {
      return GateResult::kNotHeld;
    }
    const GateResult r = ProducerGateRelease(header_->gate, token);
    if (gate_token_ == token) gate_token_ = kProducerGateUnheld;
    return r;
  }

  bool HeartbeatProducerGate() {
    return HeartbeatProducerGate(gate_token_);
  }

  bool HeartbeatProducerGate(uint64_t token) {
    if (header_ == nullptr || token == kProducerGateUnheld) return false;
    return ProducerGateHeartbeat(header_->gate, token, ProducerGateNowNs());
  }

  // The token this writer currently writes with (owner when gated).
  uint64_t producer_gate_token() const { return gate_token_; }

  bool owns_producer_gate() const {
    return header_ != nullptr && gate_token_ != kProducerGateUnheld &&
           header_->gate.owner_token.load(std::memory_order_acquire) ==
               gate_token_;
  }

  bool producer_gate_held_by_other() const {
    return header_ != nullptr &&
           ProducerGateHeldByOther(header_->gate, gate_token_,
                                   ProducerGateNowNs());
  }

  uint64_t gate_rejects() const {
    return header_ != nullptr
               ? header_->gate.rejects.load(std::memory_order_relaxed)
               : 0;
  }

  uint64_t gate_steals() const {
    return header_ != nullptr
               ? header_->gate.steals.load(std::memory_order_relaxed)
               : 0;
  }

  uint64_t gate_aborted() const {
    return header_ != nullptr
               ? header_->gate_aborted.load(std::memory_order_relaxed)
               : 0;
  }

  // Begin a frame with room for data_len payload bytes.
  //
  // REP-10: a live replay producer gate blocks the write BEFORE any slot state
  // is touched — BeginView::status == kGated and payload/header are null. This
  // is a distinct result from oversize and pinned-slot skips.
  //
  // BF-03: a pinned target slot is SKIPPED (head advances, the frame is not
  // published — readers see a hole, pinned_skip counts); the writer never
  // waits for or overwrites a lease. Callers distinguish: payload==nullptr +
  // pending_skipped() == true => skip; payload==nullptr + status==kOversize
  // => oversize; payload==nullptr + status==kGated => replay owns the channel.
  // The caller fills payload[0..data_len) and optionally header->flags, then
  // calls Commit(). Single producer: no interleaving of Begin/Commit pairs.
  BeginView Begin(uint32_t data_len) {
    pending_gated_ = false;
    pending_skipped_ = false;
    if (header_ == nullptr) {
      return {nullptr, nullptr, BeginStatus::kGated};
    }
    if (header_->gate.owner_token.load(std::memory_order_relaxed) !=
            kProducerGateUnheld &&
        !ProducerGateAllowsWrite(header_->gate, gate_token_,
                                 ProducerGateNowNs())) {
      pending_gated_ = true;
      return {nullptr, nullptr, BeginStatus::kGated};
    }
    const uint32_t slot_bytes =
        header_->slot_bytes.load(std::memory_order_relaxed);
    if (data_len > slot_bytes) {
      header_->dropped_oversize.fetch_add(1, std::memory_order_relaxed);
      return {nullptr, nullptr, BeginStatus::kOversize};
    }
    pending_head_ = header_->head.load(std::memory_order_relaxed);
    pending_len_ = data_len;
    PinnedSlotHeader& slot = SlotHeader(pending_head_);
    // Set seq = write-in-progress first, then check the pin.
    pending_saved_seq_ = slot.seq.load(std::memory_order_relaxed);
    slot.seq.store(0, std::memory_order_seq_cst);
    std::atomic_thread_fence(std::memory_order_release);
    if (lease_ != nullptr) {
      PinnedSlotLeaseCell& cell = LeaseCellOfW(pending_head_);
      const uint64_t lease_word = cell.pin_gen.load(std::memory_order_acquire);
      const uint32_t pin = LeasePinOf(lease_word);
      const bool copy_active = LeaseCopyActiveOf(lease_word);
      if ((pin > 0 || copy_active) && !StealIfExpired(cell)) {
        slot.seq.store(pending_saved_seq_, std::memory_order_release);
        lease_->pinned_skip.fetch_add(1, std::memory_order_relaxed);
        header_->head.store(pending_head_ + 1, std::memory_order_release);
        header_->epoch.fetch_add(1, std::memory_order_seq_cst);
        // Multi-reader epoch: wake every waiting reader. PERF-14 made
    // FutexWakeOne a true wake-one; this channel is broadcast (many readers
    // share header_->epoch), so a wake-one would strand the readers it did
    // not pick until the next commit.
    FutexWakeAll(&header_->epoch);
    NotifyWakeConsumers();  // let readers see head skip the hole
        pending_skipped_ = true;
        return {nullptr, nullptr, BeginStatus::kPinnedSkip};
      }
    }
    return {SlotPayload(pending_head_), &slot, BeginStatus::kOk};
  }

  void SetWakeWord(std::atomic<uint32_t>* wake_word) {
    wake_word_count_ = 0;
    (void)AddWakeWord(wake_word);
  }
  bool AddWakeWord(std::atomic<uint32_t>* wake_word) {
    if (wake_word == nullptr) return false;
    for (uint32_t i = 0; i < wake_word_count_; ++i) {
      if (wake_words_[i] == wake_word) return true;
    }
    if (wake_word_count_ >= 8) return false;
    wake_words_[wake_word_count_++] = wake_word;
    return true;
  }
  std::atomic<uint32_t>* NotifyWakeWord() const {
    return wake_word_count_ > 0 ? wake_words_[0] : nullptr;
  }

  // Publish the frame packed into Begin()'s slot. Order: header fields, then
  // the footer (magic last, payload-complete marker), then the seqlock, head,
  // and futex wake. A skipped Begin already advanced head — Commit is a no-op.
  void Commit() { Commit(MonoNs()); }

  // Lets the caller tell the two reasons Begin returned nothing — oversize
  // (counted in dropped_oversize) and pinned skip (counted in pinned_skip,
  // reported immediately by pending_skipped()).
  bool pending_skipped() const { return pending_skipped_; }
  bool pending_gated() const { return pending_gated_; }
  uint64_t pinned_skip_count() const {
    return lease_ != nullptr
               ? lease_->pinned_skip.load(std::memory_order_relaxed)
               : 0;
  }

  void Commit(uint64_t mono_ns) {
    if (pending_gated_) {
      pending_gated_ = false;
      return;
    }
    if (pending_skipped_) {
      pending_skipped_ = false;
      return;
    }
    if (header_ == nullptr) return;
    if (header_->gate.owner_token.load(std::memory_order_relaxed) !=
            kProducerGateUnheld &&
        !ProducerGateAllowsWrite(header_->gate, gate_token_,
                                 ProducerGateNowNs())) {
      // A replay gate appeared between Begin and Commit: do not publish. The
      // slot stays at seq==0 (write-in-progress) so readers never observe the
      // half-written frame; head is not advanced and the replay will own the
      // slot. Counted so the double-write attempt is visible, not silent.
      header_->gate_aborted.fetch_add(1, std::memory_order_relaxed);
      return;
    }
    PinnedSlotHeader& slot = SlotHeader(pending_head_);
    uint8_t* payload = SlotPayload(pending_head_);
    const bool crc_on =
        (header_->flags.load(std::memory_order_relaxed) &
         kPinnedSlotFlagCrcEnabled) != 0;
    slot.mono_ns.store(mono_ns, std::memory_order_relaxed);
    slot.data_len.store(pending_len_, std::memory_order_relaxed);
    slot.crc32.store(crc_on ? Crc32(payload, pending_len_) : 0,
                     std::memory_order_relaxed);
    PinnedSlotFooter& footer = SlotFooter(pending_head_);
    footer.frame_index.store(pending_head_, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
    footer.magic.store(kPinnedSlotFooterMagic, std::memory_order_release);
    slot.seq.store(pending_head_ + 1, std::memory_order_release);
    header_->head.store(pending_head_ + 1, std::memory_order_release);
    header_->epoch.fetch_add(1, std::memory_order_seq_cst);
    // Multi-reader epoch: wake every waiting reader. PERF-14 made
    // FutexWakeOne a true wake-one; this channel is broadcast (many readers
    // share header_->epoch), so a wake-one would strand the readers it did
    // not pick until the next commit.
    FutexWakeAll(&header_->epoch);
    NotifyWakeConsumers();
  }

  uint64_t dropped_oversize() const {
    return header_ != nullptr
               ? header_->dropped_oversize.load(std::memory_order_relaxed)
               : 0;
  }

  uint64_t head() const {
    return header_ != nullptr
               ? header_->head.load(std::memory_order_acquire)
               : 0;
  }

  // Channel geometry (Create parameters). 0 when the writer is not open.
  // Replay injection (REP-04) reads these to validate a package frame against
  // the target before Begin().
  uint32_t slot_bytes() const {
    return header_ != nullptr
               ? header_->slot_bytes.load(std::memory_order_acquire)
               : 0;
  }
  uint32_t slot_count() const {
    return header_ != nullptr
               ? header_->slot_count.load(std::memory_order_acquire)
               : 0;
  }

 private:
  friend class PinnedSlotReader;

  // REP-10: inspect an existing channel before unlinking it. A live replay
  // gate always wins (never detach a running replay's target); a foreign
  // header version needs an explicit force_replace.
  bool CheckReplaceAllowed(const char* name, bool force_replace) {
    ShmRegion probe;
    if (!probe.OpenRetry(name, sizeof(PinnedSlotPageHeader), 0,
                         /*read_only=*/true)) {
      return true;
    }
    const auto* h =
        static_cast<const PinnedSlotPageHeader*>(probe.data());
    const uint64_t magic = h->super.magic.load(std::memory_order_acquire);
    if (magic == PinnedSlotPageHeader::kMagic) {
      const uint32_t version =
          h->super.abi_version.load(std::memory_order_acquire);
      if (version != PinnedSlotPageHeader::kVersion) {
        if (!force_replace) {
          create_status_ = CreateStatus::kIncompatible;
          last_error_ =
              "existing channel has an incompatible header version " +
              std::to_string(version) + " (expected " +
              std::to_string(PinnedSlotPageHeader::kVersion) +
              "); pass force_replace to rebuild it";
          probe.Unmap();
          return false;
        }
      } else if (ProducerGateHolderLive(h->gate, ProducerGateNowNs())) {
        const uint64_t pid =
            h->gate.owner_pid.load(std::memory_order_acquire);
        create_status_ = CreateStatus::kGated;
        last_error_ =
            "channel is gated by a live replay holder (pid " +
            std::to_string(pid) +
            "); stop the replay instead of replacing the channel";
        probe.Unmap();
        return false;
      }
    }
    probe.Unmap();
    return true;
  }

  PinnedSlotLeaseCell& LeaseCellOfW(uint64_t frame_index) {
    return reinterpret_cast<PinnedSlotLeaseCell*>(lease_ + 1)[
        frame_index % header_->slot_count.load(std::memory_order_relaxed)];
  }

  // A pin older than steal_timeout_ns belongs to a dead reader; the writer
  // steals the slot (counted) rather than skip forever. Expiry is re-checked
  // on every pass of the CAS loop: if the epoch was refreshed by a concurrent
  // acquire (release-CAS ordering, see the cell invariant) the steal is
  // abandoned — stealing a live reader's pin would make it read a torn frame.
  // pin>0 && epoch==0/stale has two sources: a reader killed before stamping
  // (dead pin) and an acquire in flight (pinned, not yet stamped). Both are
  // reclaimable: the former is a zombie, while the latter's final seq re-check
  // sees seq=0 (the writer stored 0 first) and returns kTorn.
  // v5: copy-active cannot be force-stolen on timeout. A steal is allowed only
  // when /proc also confirms that the copier matching pid/start is dead; an
  // active owner, or one whose identity cannot be trusted, keeps being skipped.
  bool StealIfExpired(PinnedSlotLeaseCell& cell) {
    const uint64_t timeout =
        lease_->steal_timeout_ns.load(std::memory_order_relaxed);
    uint64_t cur = cell.pin_gen.load(std::memory_order_acquire);
    for (;;) {
      const uint32_t pin = LeasePinOf(cur);
      const bool copy_active = LeaseCopyActiveOf(cur);
      if (pin == 0 && !copy_active) {
        return false;
      }
      const uint64_t epoch = cell.epoch.load(std::memory_order_seq_cst);
      if (epoch != 0 && MonoNs() - epoch <= timeout) {
        return false;
      }
      if (copy_active) {
        const uint64_t pid =
            cell.copy_owner_pid.load(std::memory_order_acquire);
        const uint64_t start =
            cell.copy_owner_start.load(std::memory_order_acquire);
        if (pid == 0 || start == 0 || ProducerGateProcessAlive(pid, start)) {
          return false;
        }
      }
      // steal: generation+1, pin/active cleared to 0 — an old guard's release
      // then skips on the generation mismatch, so it neither underflows nor
      // decrements a new reader's pin. The epoch keeps its old value: it is
      // harmless once pin==0, and clearing it would erase an epoch a
      // concurrent acquire wrote first (invariant).
      if (cell.pin_gen.compare_exchange_weak(
              cur, LeasePack(LeaseGenOf(cur) + 1, 0),
              std::memory_order_acq_rel, std::memory_order_relaxed)) {
        cell.copy_owner_start.store(0, std::memory_order_release);
        cell.copy_owner_pid.store(0, std::memory_order_release);
        lease_->lease_steal.fetch_add(1, std::memory_order_relaxed);
        return true;
      }
    }
  }

  uint8_t* SlotBase(uint64_t index) {
    const uint32_t slot_count =
        header_->slot_count.load(std::memory_order_relaxed);
    const uint32_t slot_stride =
        header_->slot_stride.load(std::memory_order_relaxed);
    const uint32_t meta_blob_bytes =
        header_->meta_blob_bytes.load(std::memory_order_relaxed);
    return reinterpret_cast<uint8_t*>(header_) +
           SlotsOffset(meta_blob_bytes) +
           (index % slot_count) * static_cast<uint64_t>(slot_stride);
  }

  PinnedSlotHeader& SlotHeader(uint64_t index) {
    return *reinterpret_cast<PinnedSlotHeader*>(SlotBase(index));
  }

  uint8_t* SlotPayload(uint64_t index) {
    return SlotBase(index) + sizeof(PinnedSlotHeader);
  }

  PinnedSlotFooter& SlotFooter(uint64_t index) {
    const uint32_t slot_bytes =
        header_->slot_bytes.load(std::memory_order_relaxed);
    return *reinterpret_cast<PinnedSlotFooter*>(SlotBase(index) +
                                              sizeof(PinnedSlotHeader) +
                                              RoundUp64(slot_bytes));
  }

  void NotifyWakeConsumers() {
    for (uint32_t i = 0; i < wake_word_count_; ++i) {
      wake_words_[i]->fetch_add(1, std::memory_order_seq_cst);
      FutexWakeAll(wake_words_[i]);
    }
  }

  static uint64_t MonoNs() {
    struct timespec ts {};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1000000000ULL +
           static_cast<uint64_t>(ts.tv_nsec);
  }

  ShmRegion region_;
  ShmRegion lease_region_;
  PinnedSlotPageHeader* header_{nullptr};
  PinnedSlotLeaseHeader* lease_{nullptr};
  uint64_t pending_head_{0};
  uint64_t pending_saved_seq_{0};
  uint32_t pending_len_{0};
  bool pending_skipped_{false};
  bool pending_gated_{false};
  std::atomic<uint32_t>* wake_words_[8]{};
  uint32_t wake_word_count_{0};
  uint64_t gate_token_{kProducerGateUnheld};  // REP-10: 0 = plain producer
  CreateStatus create_status_{CreateStatus::kOk};
  std::string last_error_;
};

// Consumer side: maps the region (probe-then-remap, geometry is runtime) and
// follows the writer with a per-reader cursor, exactly like
// thyme::SpmcReader(Follow). TryRead/WaitView hand out a zero-copy view into
// the slot — no consumer-side memcpy unless the consumer wants to retain the
// frame.
class PinnedSlotReader : public Waitable {
 public:
  // RT-11/BF-13: View is move-only — a copyable bare-pointer view invites
  // store-and-use-async misuse. CopyTo() produces a HeaderSnapshot (BF-13):
  // metadata only, payload stays a borrowed shm pointer. Ownership rules:
  //
  //   * CopyTo() copies metadata; it does NOT copy or own payload bytes.
  //   * A HeaderSnapshot's payload pointer is valid only while the source
  //     frame is still pinned (Lease still_valid() == true) or known
  //     unrewritten (e.g. a scratch view from ReadFrameStable).
  //   * To retain bytes across the lease, use View::CopyPayloadTo() while the
  //     lease/view is still valid, or ReadFrameStable() (caller-owned scratch),
  //     or an explicit deep copy. Reading a HeaderSnapshot payload after its
  //     lease was released/stolen is NOT a supported path — the slot may have
  //     been rewritten.
  //
  // In non-NDEBUG builds a view produced by a plain ReadFrame carries a
  // slot-seq watch: if the slot's seq changed by the time the view is
  // destroyed, the view was held across a rewrite — abort to expose the
  // misuse. (TryRead cursor views are consumed immediately and
  // ReadFrameStable/lease views are already protected, so none of those arm
  // the watch.) Every borrowed View must be destroyed before the reader is
  // closed or its SHM mapping is replaced; the watch does not own the mapping.
  struct View {
    const uint8_t* payload = nullptr;
    uint32_t data_len = 0;
    uint64_t mono_ns = 0;
    uint32_t flags = 0;
    uint32_t crc32 = 0;
    uint64_t frame_index = 0;  // publish index (0-based, monotonic)
    uint64_t slot_seq = 0;     // slot seqlock value observed (frame_index + 1);
                               // drain-side re-validation: a later ReadFrame
                               // with a different slot_seq means overwritten
    bool crc_ok = false;       // crc disabled or verified for this view

    View() = default;
    View(const View&) = delete;
    View& operator=(const View&) = delete;
    View(View&& o) noexcept { *this = std::move(o); }
    View& operator=(View&& o) noexcept {
      if (this != &o) {
        DisarmWatch();
        payload = o.payload;
        data_len = o.data_len;
        mono_ns = o.mono_ns;
        flags = o.flags;
        crc32 = o.crc32;
        frame_index = o.frame_index;
        slot_seq = o.slot_seq;
        crc_ok = o.crc_ok;
        seq_watch_ = o.seq_watch_;
        lease_cell_ = o.lease_cell_;
        lease_gen_ = o.lease_gen_;
        lease_owner_pid_ = o.lease_owner_pid_;
        lease_owner_start_ = o.lease_owner_start_;
        o.seq_watch_ = nullptr;
        o.lease_cell_ = nullptr;
        o.lease_gen_ = 0;
        o.lease_owner_pid_ = 0;
        o.lease_owner_start_ = 0;
      }
      return *this;
    }
    ~View() { CheckWatch(); }

    // BF-13: metadata-only snapshot. Every scalar is owned by the snapshot;
    // payload is a BORROWED pointer into the shm slot (CopyTo never copies
    // payload bytes). It is valid only while the frame is pinned (Lease) or
    // known unrewritten. Use View::CopyPayloadTo() or ReadFrameStable() to
    // retain the bytes.
    struct HeaderSnapshot {
      const uint8_t* payload = nullptr;
      uint32_t data_len = 0;
      uint64_t mono_ns = 0;
      uint32_t flags = 0;
      uint32_t crc32 = 0;
      uint64_t frame_index = 0;
      uint64_t slot_seq = 0;
      bool crc_ok = false;
    };
    // Legacy name (pre-BF-13); kept source-compatible, same metadata-only
    // semantics. Prefer HeaderSnapshot in new code.
    using Snapshot = HeaderSnapshot;

    void CopyTo(HeaderSnapshot& out) const {
      out.payload = payload;
      out.data_len = data_len;
      out.mono_ns = mono_ns;
      out.flags = flags;
      out.crc32 = crc32;
      out.frame_index = frame_index;
      out.slot_seq = slot_seq;
      out.crc_ok = crc_ok;
    }

    // BF-13: copy the payload bytes (not metadata) into caller-owned storage.
    // Returns false when the destination is too small, or when this view can
    // no longer prove the source is intact:
    //   * lease-backed views: the lease generation must still match and pin
    //     must be > 0 (a steal/release invalidates the copy);
    //   * plain ReadFrame views: the armed slot-seq watch must still match.
    // For plain TryRead views (no watch) this is a best-effort copy; use
    // ReadFrameStable() when torn data must be impossible.
    // On false the caller must discard whatever was written to dst.
    bool CopyPayloadTo(uint8_t* dst, size_t cap) const {
      if (dst == nullptr || payload == nullptr || data_len > cap) return false;
      if (!SourceIntact()) return false;
      if (lease_cell_ != nullptr) {
        auto* cell = const_cast<PinnedSlotLeaseCell*>(lease_cell_);
        const LeaseCopyBeginResult begin = LeaseCopyBegin(
            *cell, lease_gen_, lease_owner_pid_, lease_owner_start_);
        if (begin == LeaseCopyBeginResult::kInvalid) {
          return false;
        }
        if (begin == LeaseCopyBeginResult::kOk) {
          std::memcpy(dst, payload, data_len);
          const bool intact = SourceIntact();
          LeaseCopyEnd(*cell, lease_gen_, lease_owner_pid_);
          return intact;
        }
        if (!SourceIntact()) {
          return false;
        }
      }
      detail::AtomicPayloadCopyOut(dst, payload, data_len);
      return SourceIntact();
    }

   private:
    friend class PinnedSlotReader;
    void ArmWatch(const std::atomic<uint64_t>* slot_seq_ptr) {
      seq_watch_ = slot_seq_ptr;
    }
    void DisarmWatch() { seq_watch_ = nullptr; }
    // BF-12/BF-13: pointed at the lease cell that protects this view during
    // AcquireLease. Cleared for plain reads and owned scratch views.
    void SetLeaseCheck(const PinnedSlotLeaseCell* cell, uint32_t gen,
                       uint64_t owner_pid, uint64_t owner_start) {
      lease_cell_ = cell;
      lease_gen_ = gen;
      lease_owner_pid_ = owner_pid;
      lease_owner_start_ = owner_start;
    }
    void ClearLeaseCheck() {
      lease_cell_ = nullptr;
      lease_gen_ = 0;
      lease_owner_pid_ = 0;
      lease_owner_start_ = 0;
    }
    bool SourceIntact() const {
      if (lease_cell_ != nullptr) {
        const uint64_t word =
            lease_cell_->pin_gen.load(std::memory_order_acquire);
        return LeaseGenOf(word) == lease_gen_ && LeasePinOf(word) > 0;
      }
      if (seq_watch_ != nullptr) {
        return seq_watch_->load(std::memory_order_acquire) == slot_seq;
      }
      return true;
    }
    void CheckWatch() const {
#if !defined(NDEBUG)
      if (seq_watch_ != nullptr &&
          seq_watch_->load(std::memory_order_acquire) != slot_seq) {
        std::fprintf(stderr,
                     "PinnedSlotReader::View destroyed after its slot was "
                     "rewritten (frame_index=%llu, expected seq %llu, got "
                     "%llu) — store a HeaderSnapshot via CopyTo() instead\n",
                     static_cast<unsigned long long>(frame_index),
                     static_cast<unsigned long long>(slot_seq),
                     static_cast<unsigned long long>(
                         seq_watch_->load(std::memory_order_acquire)));
        std::abort();
      }
#endif
    }
    const std::atomic<uint64_t>* seq_watch_ = nullptr;
    const PinnedSlotLeaseCell* lease_cell_ = nullptr;
    uint32_t lease_gen_ = 0;
    uint64_t lease_owner_pid_ = 0;
    uint64_t lease_owner_start_ = 0;
  };

  // Random-access read result for ReadFrame (triggered-acquisition pre-window
  // lookback and drain-time re-validation).
  enum class ReadResult : uint8_t {
    kOk,            // view filled and validated
    kNotPublished,  // frame_index >= head (not written yet)
    kOverrun,       // slot already holds a newer frame (evicted hole)
    kTorn,          // seqlock/footer/crc inconsistent (writer mid-commit or
                    // killed mid-write); counted in torn_read_count()
  };

  PinnedSlotReader() = default;
  PinnedSlotReader(const PinnedSlotReader&) = delete;
  PinnedSlotReader& operator=(const PinnedSlotReader&) = delete;

  // lease_read_only = true: read-only observation mode — the lease table is
  // mapped RO as well and never written; TryRead/ReadFrame work as usual and
  // AcquireLease explicitly returns kTorn (unavailable). Default false: the
  // lease table is mapped RW (required by the lease protocol).
  bool Open(const char* name, uint64_t timeout_ns, bool lease_read_only = false) {
    lease_read_only_ = lease_read_only;
    copy_owner_pid_ = static_cast<uint64_t>(getpid());
    copy_owner_start_ = ProducerGateProcessStart(copy_owner_pid_);
    // Phase 1: described open of the fixed channel prefix.
    ShmPageError error;
    if (!region_.OpenDescribed(name, PinnedSlotPageHeader::Spec(), timeout_ns,
                               /*read_only=*/true, &error)) {
      return false;
    }
    auto* h = static_cast<const PinnedSlotPageHeader*>(region_.data());
    const uint64_t deadline = MonoNs() + timeout_ns;
    if (h->slot_count.load(std::memory_order_acquire) == 0 ||
        h->slot_bytes.load(std::memory_order_acquire) == 0) {
      std::fprintf(stderr, "PinnedSlotStore: %s layout mismatch\n", name);
      region_.Unmap();
      return false;
    }
    const uint32_t slot_bytes =
        h->slot_bytes.load(std::memory_order_acquire);
    const uint32_t slot_count =
        h->slot_count.load(std::memory_order_acquire);
    const uint32_t meta_blob_bytes =
        h->meta_blob_bytes.load(std::memory_order_acquire);
    region_.Unmap();
    // Full map with the real region size.
    const uint64_t remaining =
        MonoNs() < deadline ? deadline - MonoNs() : 0;
    if (!region_.OpenRetry(name,
                           PinnedSlotWriter::RegionBytes(slot_bytes, slot_count,
                                                       meta_blob_bytes),
                           remaining, /*read_only=*/true)) {
      return false;
    }
    if (ValidateShmSuperHeader(region_.data(), region_.size(),
                               PinnedSlotPageHeader::Spec(), &error) !=
        ShmPageErrc::kOk) {
      region_.Unmap();
      return false;
    }
    header_ = static_cast<const PinnedSlotPageHeader*>(region_.data());
    // BF-01: the lease table is a separate RW mapping (payload stays RO).
    const std::string lease_name = PinnedSlotWriter::LeaseShmName(name);
    if (!lease_region_.OpenDescribed(lease_name.c_str(),
                                     PinnedSlotLeaseHeader::Spec(), remaining,
                                     lease_read_only, &error)) {
      region_.Unmap();
      header_ = nullptr;
      return false;
    }
    lease_region_.Unmap();
    if (!lease_region_.OpenRetry(lease_name.c_str(),
                                 PinnedSlotWriter::LeaseRegionBytes(slot_count),
                                 remaining, lease_read_only)) {
      region_.Unmap();
      header_ = nullptr;
      return false;
    }
    auto* lh = static_cast<PinnedSlotLeaseHeader*>(lease_region_.data());
    if (ValidateShmSuperHeader(lease_region_.data(), lease_region_.size(),
                               PinnedSlotLeaseHeader::Spec(), &error) !=
            ShmPageErrc::kOk ||
        lh->slot_count.load(std::memory_order_acquire) != slot_count) {
      std::fprintf(stderr, "PinnedSlotStore: %s lease layout mismatch\n",
                   name);
      lease_region_.Unmap();
      region_.Unmap();
      header_ = nullptr;
      return false;
    }
    lease_ = lh;
    return true;
  }

  void Close() {
    header_ = nullptr;
    lease_ = nullptr;
    lease_region_.Unmap();
    region_.Unmap();
  }

  const void* MetaBlob() const {
    return reinterpret_cast<const uint8_t*>(header_) +
           PinnedSlotWriter::MetaOffset();
  }

  // Lease table access (BF-01). Null when not attached.
  const PinnedSlotLeaseHeader* lease() const { return lease_; }

  // BF-12: configured dead-reader steal threshold for this channel (0 when
  // not attached). Acquire() caches it on each lease.
  uint64_t steal_timeout_ns() const {
    return lease_ != nullptr
               ? lease_->steal_timeout_ns.load(std::memory_order_relaxed)
               : 0;
  }

  // True between a successful Open and Close. Used by Lease's debug
  // lifetime assertion (P3-8).
  bool IsOpen() const { return header_ != nullptr; }

  // BF-02: per-slot lease RAII guard. The view stays valid (the writer skips
  // pinned slots instead of overwriting them) until destruction/Reset.
  // Lifetime rule (P3-8): a live Lease must not outlive its reader's
  // Close() — destroy or Reset every Lease before closing the reader. Debug
  // builds abort on a violation; release builds silently skip the unpin
  // (ReleaseLease no-ops once lease_ is null), which would leak a pin.
  //
  // BF-12 usage contract:
  //   Lease l = reader.Acquire(index);
  //   if (!l.valid()) { /* acquire failed: no pin, no payload */ }
  //   use(l.view());                 // payload is safe while still_valid()
  //   if (!l.still_valid()) { /* stolen: discard the payload */ }
  //   LeaseReleaseResult rr = l.Release();  // kOk / kStolen / kAlreadyReleased
  //
  // valid() means "acquire succeeded and this lease owns (or owned) a pin".
  // It is NOT a liveness check: the writer may steal the slot after the
  // timeout, at which point valid() stays true but still_valid() returns
  // false. Check still_valid() after any long operation on the payload.
  class Lease {
   public:
    Lease() = default;
    ~Lease() { Reset(); }
    Lease(const Lease&) = delete;
    Lease& operator=(const Lease&) = delete;
    Lease(Lease&& o) noexcept { *this = std::move(o); }
    Lease& operator=(Lease&& o) noexcept {
      if (this != &o) {
        Reset();
        reader_ = o.reader_;
        index_ = o.index_;
        gen_ = o.gen_;
        acquired_at_ns_ = o.acquired_at_ns_;
        released_at_ns_ = o.released_at_ns_;
        steal_timeout_ns_ = o.steal_timeout_ns_;
        invalidated_ = o.invalidated_;
        view_ = std::move(o.view_);  // View is move-only (RT-11)
        valid_ = o.valid_;
        o.reader_ = nullptr;
        o.valid_ = false;
        o.index_ = 0;
        o.gen_ = 0;
        o.acquired_at_ns_ = 0;
        o.released_at_ns_ = 0;
        o.steal_timeout_ns_ = 0;
        o.invalidated_ = false;
      }
      return *this;
    }

    // "Acquire succeeded": this lease owns a pin until Release/Reset. Does
    // NOT mean the pin is still alive — see still_valid().
    bool valid() const { return valid_; }

    // BF-12: true while the lease's generation still matches the cell and the
    // pin count is > 0. False after release, after the writer steals the slot
    // (generation advanced), or when the reader is read-only. The first
    // mismatch observed by this lease increments the reader's
    // lease_invalidated_count() (at most once per lease).
    bool still_valid() const {
      if (reader_ == nullptr || !valid_ || !reader_->IsOpen()) return false;
      const bool held = reader_->LeaseStillValid(index_, gen_);
      if (!held) NoteInvalidated();
      return held;
    }

    // BF-12: lease generation (steal advances it). Meaningful only when
    // acquired; a fresh channel legitimately starts at generation 0.
    uint32_t generation() const { return gen_; }

    // BF-12: monotonic nanoseconds since acquisition. Keeps advancing while
    // valid() is true (even if the slot was stolen) and freezes at
    // Release/Reset.
    uint64_t held_ns() const {
      if (acquired_at_ns_ == 0) return 0;
      const uint64_t end =
          valid_ ? PinnedSlotReader::MonoNs()
                 : (released_at_ns_ != 0 ? released_at_ns_
                                         : PinnedSlotReader::MonoNs());
      return end > acquired_at_ns_ ? end - acquired_at_ns_ : 0;
    }

    // BF-12: configured dead-reader steal timeout (cached at acquisition, so
    // it survives the reader closing). 0 when this lease was never acquired.
    uint64_t steal_timeout_ns() const { return steal_timeout_ns_; }

    // BF-12: acquisition time + steal timeout, in the same monotonic clock as
    // View::mono_ns. The writer's actual expiry uses the epoch stamped after
    // validation (slightly later), so this deadline is conservative: a still
    // valid lease may remain stealable a little longer than this value.
    uint64_t deadline_ns() const {
      return acquired_at_ns_ != 0 ? acquired_at_ns_ + steal_timeout_ns_ : 0;
    }

    const View& view() const { return view_; }

    // BF-13: guarded payload copy. Refuses once the lease is no longer valid
    // (released/stolen), so the borrowed slot bytes are never copied on a
    // lease that can no longer prove they are intact.
    bool CopyPayloadTo(uint8_t* dst, size_t cap) const {
      return still_valid() && view_.CopyPayloadTo(dst, cap);
    }

    // BF-12: report what Release actually did. Idempotent: a second call
    // returns kAlreadyReleased. Steal detection here also increments
    // lease_invalidated_count() (unless still_valid() already reported it).
    LeaseReleaseResult Release() {
      view_.DisarmWatch();  // defensive: an invalid Lease dtor must leave no watch
      if (reader_ == nullptr || !valid_) {
        reader_ = nullptr;
        valid_ = false;
        return LeaseReleaseResult::kAlreadyReleased;
      }
#if !defined(NDEBUG)
      if (!reader_->IsOpen()) {
        std::fprintf(stderr,
                     "PinnedSlotStore: Lease outlived its reader's Close() "
                     "(frame %llu) — destroy/Reset leases first (P3-8)\n",
                     static_cast<unsigned long long>(index_));
        std::abort();
      }
#endif
      const LeaseReleaseResult r = reader_->ReleaseLeaseStatus(index_, gen_);
      if (r == LeaseReleaseResult::kStolen) NoteInvalidated();
      released_at_ns_ = PinnedSlotReader::MonoNs();
      reader_ = nullptr;
      valid_ = false;
      return r;
    }

    // Legacy void wrapper (destructor too): discards the result.
    void Reset() { (void)Release(); }

   private:
    friend class PinnedSlotReader;
    void NoteInvalidated() const {
      if (invalidated_ || reader_ == nullptr) return;
      invalidated_ = true;
      reader_->lease_invalidated_.fetch_add(1, std::memory_order_relaxed);
    }

    PinnedSlotReader* reader_ = nullptr;
    uint64_t index_ = 0;
    uint32_t gen_ = 0;  // lease ABI v2+: this lease's generation
    uint64_t acquired_at_ns_ = 0;
    uint64_t released_at_ns_ = 0;
    uint64_t steal_timeout_ns_ = 0;
    mutable bool invalidated_ = false;  // counted in lease_invalidated already
    View view_{};
    bool valid_ = false;
  };

  // Acquire a lease on frame_index. Order (roadmap §3.18): pre-check
  // head/window -> pin -> validate (seqlock/footer/crc via ReadFrame) ->
  // stamp epoch -> re-check seq. Pinning BEFORE validation closes the
  // read-before-pin window (a writer that starts after the pin must skip; one
  // that already started makes ReadFrame/seq re-check fail). A failed
  // validation unpins and returns kOverrun/kNotPublished/kTorn; a failure
  // after the stamp also rolls the stamp back so it cannot keep a dead pin's
  // epoch fresh (BF-17). gen_out receives the lease generation (release matches
  // on it, P1-1).
  ReadResult AcquireLease(uint64_t frame_index, View& out, uint32_t* gen_out,
                          bool verify_crc = true) {
    out.DisarmWatch();
    out.ClearLeaseCheck();  // reused View must not keep a stale lease link
    if (lease_read_only_) {
      return ReadResult::kTorn;  // read-only mode: lease unavailable (see Open)
    }
    if (header_ == nullptr) return ReadResult::kNotPublished;
    // Cheap pre-check avoids pinning a future/evicted slot (which would make
    // the writer skip a frame for a lease that cannot succeed). The pin and
    // ReadFrame below still revalidate under the race.
    const uint64_t h = head();
    if (frame_index >= h) return ReadResult::kNotPublished;
    if (h - frame_index > slot_count()) return ReadResult::kOverrun;
    PinnedSlotLeaseCell& cell = LeaseCellOf(frame_index);
    // v4/v5 invariant: pin before validate (closes the read-before-pin
    // window); the epoch is stamped after validation succeeds and before the
    // final seq re-check. Being killed between the pin and the stamp leaves
    // the zombie state pin>0 && epoch==0/stale — StealIfExpired treats
    // epoch==0 as immediately reclaimable, so a pin never lasts forever. The
    // v5 pin CAS preserves copy-active.
    uint32_t gen = 0;
    uint64_t cur = cell.pin_gen.load(std::memory_order_acquire);
    for (;;) {
      const uint32_t pin = LeasePinOf(cur);
      if (pin == kPinnedSlotLeaseMaxPin) {  // pin overflow guard (unreachable in practice)
        torn_reads_ += 1;
        out.DisarmWatch();
        return ReadResult::kTorn;
      }
      gen = LeaseGenOf(cur);
      if (cell.pin_gen.compare_exchange_weak(
              cur, LeasePack(gen, pin + 1, LeaseCopyActiveOf(cur)),
              std::memory_order_acq_rel, std::memory_order_acquire)) {
        break;
      }
    }
    // Pin BEFORE validating. If the writer starts after the pin it must skip
    // this slot; if it already started, seq=0/new makes ReadFrame fail. The
    // old read-then-pin order had a window where the writer completed a newer
    // frame between ReadFrame and the CAS, leaving the reader with stale
    // header/footer but a newer payload (the observed F+2 tear).
    ReadResult r = ReadFrame(frame_index, out, verify_crc);
    if (r != ReadResult::kOk) {
      UnpinGen(cell, gen);
      out.DisarmWatch();
      return r;
    }
    if (verify_crc && !out.crc_ok) {
      // A concurrent writer can leave a mix of old header (crc) and new
      // payload. ReadFrame reports kOk + crc_ok=false; reject it before it
      // is written under the old frame_index.
      UnpinGen(cell, gen);
      out.DisarmWatch();
      torn_reads_ += 1;
      return ReadResult::kTorn;
    }
    // Stamp the epoch only after validation succeeds (v4): from then on a
    // writer seeing pin>0 + a fresh epoch skips; a steal between the stamp and
    // the re-check is caught by the seq re-check below (kTorn). seq_cst is
    // required: the writer stores 0 with seq_cst in Begin and then reads the
    // epoch; under a relaxed store the writer might miss the freshly stamped
    // epoch and steal a live lease as a zombie (BF-15).
    // Stamp with a CAS and remember the exact previous value prev. If the
    // final seq re-check fails, roll back only this attempt's own stamp
    // (CAS stamp -> prev) — a failed attempt must not refresh a dead pin's
    // epoch, otherwise repeated failures would keep the writer waiting for the
    // steal timeout forever. The rollback only applies when no newer stamp
    // exists: a concurrent successful lease's epoch is left untouched; if prev
    // itself is the epoch of an earlier successful acquire, the rollback
    // restores exactly that.
    uint64_t prev = cell.epoch.load(std::memory_order_seq_cst);
    uint64_t stamp = 0;
    for (;;) {
      stamp = MonoNs();
      if (cell.epoch.compare_exchange_weak(prev, stamp,
                                           std::memory_order_seq_cst,
                                           std::memory_order_seq_cst)) {
        break;
      }
    }
    if (epoch_stamp_test_hook_ != nullptr) {
      epoch_stamp_test_hook_(epoch_stamp_test_hook_ctx_);
    }
    const uint8_t* base = SlotBase(frame_index);
    const auto* slot =
        reinterpret_cast<const PinnedSlotHeader*>(base);
    if (slot->seq.load(std::memory_order_seq_cst) != frame_index + 1) {
      (void)cell.epoch.compare_exchange_strong(stamp, prev,
                                               std::memory_order_seq_cst,
                                               std::memory_order_seq_cst);
      UnpinGen(cell, gen);
      torn_reads_ += 1;
      out.DisarmWatch();  // the failure path must also remove ReadFrame's watch (P1-2)
      return ReadResult::kTorn;
    }
    out.DisarmWatch();  // the lease guarantees the lifetime; no watch needed
    out.SetLeaseCheck(&cell, gen, copy_owner_pid_, copy_owner_start_);
    if (gen_out != nullptr) *gen_out = gen;
    return ReadResult::kOk;
  }

  Lease Acquire(uint64_t frame_index, ReadResult* status = nullptr,
                bool verify_crc = true) {
    Lease l;
    uint32_t gen = 0;
    const ReadResult r = AcquireLease(frame_index, l.view_, &gen, verify_crc);
    if (r == ReadResult::kOk) {
      l.reader_ = this;
      l.index_ = frame_index;
      l.gen_ = gen;
      l.acquired_at_ns_ = MonoNs();
      l.steal_timeout_ns_ = steal_timeout_ns();
      l.valid_ = true;
    }
    if (status != nullptr) *status = r;
    return l;
  }

  // BF-12: low-level liveness query for recorder/probe code that owns a raw
  // (index, gen) pair. True while the generation still matches and pin > 0.
  bool LeaseStillValid(uint64_t frame_index, uint32_t gen) const {
    if (lease_ == nullptr || lease_read_only_) return false;
    const uint64_t word =
        LeaseCellOf(frame_index).pin_gen.load(std::memory_order_acquire);
    return LeaseGenOf(word) == gen && LeasePinOf(word) > 0;
  }

  // BF-12: generation-matched unpin with the Release outcome. Low-level
  // ReleaseLease remains void; recorder/probe can use this when it needs to
  // distinguish "still owned" from "stolen".
  LeaseReleaseResult ReleaseLeaseStatus(uint64_t frame_index, uint32_t gen) {
    if (lease_ == nullptr || lease_read_only_) {
      return LeaseReleaseResult::kAlreadyReleased;
    }
    PinnedSlotLeaseCell& cell = LeaseCellOf(frame_index);
    uint64_t cur = cell.pin_gen.load(std::memory_order_acquire);
    for (;;) {
      if (LeaseGenOf(cur) != gen) return LeaseReleaseResult::kStolen;
      const uint32_t pin = LeasePinOf(cur);
      if (pin == 0) return LeaseReleaseResult::kAlreadyReleased;
      if (cell.pin_gen.compare_exchange_weak(
              cur, LeasePack(gen, pin - 1, LeaseCopyActiveOf(cur)),
              std::memory_order_acq_rel, std::memory_order_acquire)) {
        return LeaseReleaseResult::kOk;
      }
    }
  }

  void ReleaseLease(uint64_t frame_index, uint32_t gen) {
    if (lease_ == nullptr || lease_read_only_) return;
    (void)ReleaseLeaseStatus(frame_index, gen);
  }

  // BF-14: high-level facade status. The low-level ReadResult (kNotPublished/
  // kOverrun) is collapsed to the cases an application needs:
  //   kOk    — a valid lease-backed frame was returned.
  //   kEmpty — no frame is available right now (cursor unchanged).
  //   kLost  — the cursor frame is gone (evicted gap or pinned-skip hole);
  //            the cursor advanced and drop_count() counted it.
  //   kTorn  — the slot was torn/corrupt; the cursor advanced past it and
  //            torn_read_count() counted it.
  enum class FrameStatus : uint8_t { kOk, kEmpty, kLost, kTorn };

  // BF-14: move-only lease-backed frame returned by Next()/NextLease().
  // Application code uses data()/size()/mono_ns()/index() and still_valid();
  // the generation and raw release protocol stay hidden. The payload is safe
  // while still_valid(); Release() reports kOk/kStolen/kAlreadyReleased.
  class BorrowedFrame {
   public:
    BorrowedFrame() = default;
    ~BorrowedFrame() = default;
    BorrowedFrame(const BorrowedFrame&) = delete;
    BorrowedFrame& operator=(const BorrowedFrame&) = delete;
    BorrowedFrame(BorrowedFrame&& o) noexcept { *this = std::move(o); }
    BorrowedFrame& operator=(BorrowedFrame&& o) noexcept {
      if (this != &o) {
        lease_ = std::move(o.lease_);
        status_ = o.status_;
        o.status_ = FrameStatus::kEmpty;
      }
      return *this;
    }

    FrameStatus status() const { return status_; }
    bool ok() const { return status_ == FrameStatus::kOk; }
    // Payload is safe only while still_valid(). Returns nullptr for non-kOk
    // results and after Release().
    const uint8_t* data() const {
      return ok() ? lease_.view().payload : nullptr;
    }
    uint32_t size() const { return ok() ? lease_.view().data_len : 0; }
    uint64_t mono_ns() const { return ok() ? lease_.view().mono_ns : 0; }
    uint64_t index() const { return ok() ? lease_.view().frame_index : 0; }
    uint32_t flags() const { return ok() ? lease_.view().flags : 0; }
    bool crc_ok() const { return ok() && lease_.view().crc_ok; }
    // BF-12/BF-14: false after the writer stole the slot. A subsequent
    // Release() reports kStolen and the reader's lease_invalidated_count()
    // records it (once per lease).
    bool still_valid() const { return ok() && lease_.still_valid(); }
    // Copy while the lease is still valid (BF-13 guarded copy).
    bool CopyPayloadTo(uint8_t* dst, size_t cap) const {
      return still_valid() && lease_.view().CopyPayloadTo(dst, cap);
    }
    LeaseReleaseResult Release() {
      const LeaseReleaseResult r = lease_.Release();
      status_ = FrameStatus::kEmpty;
      return r;
    }

   private:
    friend class PinnedSlotReader;
    Lease lease_{};
    FrameStatus status_ = FrameStatus::kEmpty;
  };

  // BF-14: one-step cursor advance. Unlike TryRead + AcquireLease it pins the
  // slot first and validates once under the pin (no double validation), and
  // it never exposes gen/ReleaseLease/ReadResult. In lease_read_only mode the
  // writer-side pin protocol is unavailable, so available frames report
  // kTorn; read-only observers should keep using TryRead().
  BorrowedFrame NextLease(bool verify_crc = true) {
    BorrowedFrame out;
    if (header_ == nullptr) return out;  // kEmpty
    // RT-09 audit: this budget is vestigial — every branch inside the body
    // returns (success, kLost, kTorn or kEmpty), so at most one attempt runs.
    // It is kept so the shape matches TryRead's; retries are therefore never
    // counted here (retry_count() covers ReadFrame/TryRead only).
    for (int attempt = 0; attempt < kPinnedSlotReadRetries; ++attempt) {
      const uint64_t h = head();
      if (next_ >= h) return out;  // kEmpty: cursor unchanged
      if (h - next_ > slot_count()) {
        // Lagging reader: the whole oldest window is gone. Mirror TryRead's
        // gap-skip and surface one kLost result for the jump.
        drops_ += h - slot_count() - next_;
        next_ = h - slot_count();
        out.status_ = FrameStatus::kLost;
        return out;
      }
      const uint64_t idx = next_;
      ReadResult rr = ReadResult::kTorn;
      Lease l = Acquire(idx, &rr, verify_crc);
      if (l.valid()) {
        out.lease_ = std::move(l);
        out.status_ = FrameStatus::kOk;
        next_ += 1;  // advance only after the pin+validation succeeded
        return out;
      }
      switch (rr) {
        case ReadResult::kNotPublished:
          return out;  // raced back to empty; leave the cursor alone
        case ReadResult::kOverrun:
          // Evicted slot or a pinned-skip hole: the frame is gone.
          drops_ += 1;
          next_ += 1;
          out.status_ = FrameStatus::kLost;
          return out;
        case ReadResult::kTorn:
          // Match TryRead: classify once, skip it, keep the stream moving.
          drops_ += 1;
          next_ += 1;
          out.status_ = FrameStatus::kTorn;
          return out;
        case ReadResult::kOk:
          break;  // unreachable while l.valid() is false
      }
    }
    out.status_ = FrameStatus::kTorn;
    return out;
  }

  // Alias: the roadmap/application API name.
  BorrowedFrame Next(bool verify_crc = true) { return NextLease(verify_crc); }

  // BF-14: a tiny cursor wrapper for code that wants "the reader's next
  // frame" without touching the low-level protocol.
  class FrameCursor {
   public:
    explicit FrameCursor(PinnedSlotReader& reader) : reader_(&reader) {}
    BorrowedFrame Next(bool verify_crc = true) {
      return reader_->Next(verify_crc);
    }
    BorrowedFrame NextLease(bool verify_crc = true) {
      return reader_->NextLease(verify_crc);
    }
    uint64_t position() const { return reader_->cursor(); }

   private:
    PinnedSlotReader* reader_;
  };

  // Test seam (BF-17): invoked inside AcquireLease after the epoch stamp and
  // before the final seq re-check. A test installs a hook that lands a
  // concurrent state change (e.g. a writer Begin: seq=0 through its own RW
  // mapping) exactly in that window, then asserts the failed acquire rolled
  // its epoch stamp back. The pointer stays null outside tests (one
  // predictable branch per acquire).
  void set_epoch_stamp_test_hook(void (*hook)(void*), void* ctx) {
    epoch_stamp_test_hook_ = hook;
    epoch_stamp_test_hook_ctx_ = ctx;
  }

  // PERF-15: exercise the no-/proc-start-token fallback without adding a
  // branch to the copy path. Applies to views acquired after this call.
  void set_copy_owner_start_test_override(uint64_t start) {
    copy_owner_start_ = start;
  }

  uint32_t slot_bytes() const {
    return header_->slot_bytes.load(std::memory_order_acquire);
  }
  uint32_t slot_count() const {
    return header_->slot_count.load(std::memory_order_acquire);
  }
  // Total frames ever published (the next frame_index). Triggered-acquisition
  // recorders use it to bound the pre-window lookback (design doc 4.4).
  uint64_t head() const {
    return header_ != nullptr
               ? header_->head.load(std::memory_order_acquire)
               : 0;
  }

  // Random-access read of the frame with absolute frame_index, WITHOUT moving
  // the follow cursor. Validates seqlock/footer (and crc unless verify_crc is
  // false — header-only descriptor harvest passes false to stay cheap on
  // MB-scale slots). Distinguishes kOverrun (slot evicted -> permanent hole)
  // from kTorn (writer mid-commit or killed mid-write; counted in
  // torn_read_count()).
  ReadResult ReadFrame(uint64_t frame_index, View& out,
                       bool verify_crc = true) {
    // A new read supersedes whatever view the caller passed in; drop its
    // debug watch first so an early error return cannot leave a pointer that
    // the writer may rewrite before the view is destroyed (BF-13).
    out.DisarmWatch();
    if (header_ == nullptr) return ReadResult::kNotPublished;
    const uint32_t count = slot_count();
    const uint32_t sbytes = slot_bytes();
    for (int attempt = 0; attempt < kPinnedSlotReadRetries; ++attempt) {
      const uint64_t h = header_->head.load(std::memory_order_acquire);
      if (frame_index >= h) return ReadResult::kNotPublished;
      if (h - frame_index > count) return ReadResult::kOverrun;  // evicted
      const uint8_t* base = SlotBase(frame_index);
      const auto* slot =
          reinterpret_cast<const PinnedSlotHeader*>(base);
      const uint64_t s0 = slot->seq.load(std::memory_order_acquire);
      if (s0 == 0) {  // writer in progress / killed mid-write
        torn_reads_ += 1;
        return ReadResult::kTorn;
      }
      if (s0 != frame_index + 1) return ReadResult::kOverrun;  // slot replaced
      const uint32_t len = slot->data_len.load(std::memory_order_acquire);
      const uint64_t mono = slot->mono_ns.load(std::memory_order_acquire);
      const uint32_t f = slot->flags.load(std::memory_order_acquire);
      const uint32_t crc = slot->crc32.load(std::memory_order_acquire);
      if (len > sbytes) {  // torn header; seq re-check retries
        retries_ += 1;
        continue;
      }
      const uint8_t* payload = base + sizeof(PinnedSlotHeader);
      const auto* footer = reinterpret_cast<const PinnedSlotFooter*>(
          base + sizeof(PinnedSlotHeader) +
          PinnedSlotWriter::RoundUp64(sbytes));
      const uint64_t footer_magic =
          footer->magic.load(std::memory_order_acquire);
      const uint64_t footer_index =
          footer->frame_index.load(std::memory_order_acquire);
      const uint64_t s1 = slot->seq.load(std::memory_order_acquire);
      const bool footer_ok = footer_magic == kPinnedSlotFooterMagic &&
                             footer_index == frame_index;
      if (s1 != s0 || !footer_ok) {
        if (s1 != s0) {  // raced a commit: retry within budget
          retries_ += 1;
          continue;
        }
        torn_reads_ += 1;  // stable but incomplete (killed mid-write)
        return ReadResult::kTorn;
      }
      out.payload = payload;
      out.data_len = len;
      out.mono_ns = mono;
      out.flags = f;
      out.crc32 = crc;
      out.frame_index = frame_index;
      out.slot_seq = s0;
      out.ClearLeaseCheck();  // plain read: no lease protects this view
      const bool crc_on =
          (header_->flags.load(std::memory_order_relaxed) &
           kPinnedSlotFlagCrcEnabled) != 0;
      out.crc_ok = verify_crc &&
                   (!crc_on || (crc != 0 && crc == Crc32(payload, len)));
      out.ArmWatch(&slot->seq);  // RT-11 debug watch (plain random access)
      return ReadResult::kOk;
    }
    torn_reads_ += 1;
    return ReadResult::kTorn;
  }

  // Zero-copy read of the next frame. Validates the seqlock, the footer
  // magic/frame_index (payload-complete marker), and the payload crc; a torn
  // or incomplete frame counts torn_reads and is skipped. Returns false when
  // no new frame is available.
  bool TryRead(View& out) {
    if (header_ == nullptr) return false;
    const uint32_t count = slot_count();
    const uint32_t sbytes = slot_bytes();
    for (int attempt = 0; attempt < kPinnedSlotReadRetries; ++attempt) {
      const uint64_t h = header_->head.load(std::memory_order_acquire);
      if (next_ >= h) return false;
      if (h - next_ > count) {
        drops_ += h - count - next_;
        next_ = h - count;
      }
      const uint64_t idx = next_;
      const uint8_t* base = SlotBase(idx);
      const auto* slot =
          reinterpret_cast<const PinnedSlotHeader*>(base);
      const uint64_t s0 = slot->seq.load(std::memory_order_acquire);
      if (s0 != idx + 1) {
        if (s0 == 0) {  // writer mid-commit: retry within budget
          retries_ += 1;
          continue;
        }
        // s0 < idx+1: a pinned-skip hole (the writer advanced head without
        // publishing). s0 > idx+1: overwritten. Either way our frame is not
        // there — count it and move on instead of spinning on the hole.
        const uint64_t h2 = header_->head.load(std::memory_order_acquire);
        if (h2 - next_ > count) {
          drops_ += h2 - count - next_;
          next_ = h2 - count;
        } else {
          drops_ += 1;
          next_ += 1;
        }
        continue;
      }
      const uint32_t len = slot->data_len.load(std::memory_order_acquire);
      const uint64_t mono = slot->mono_ns.load(std::memory_order_acquire);
      const uint32_t f = slot->flags.load(std::memory_order_acquire);
      const uint32_t crc = slot->crc32.load(std::memory_order_acquire);
      if (len > sbytes) {  // torn read; seq check re-tries
        retries_ += 1;
        continue;
      }
      const uint8_t* payload = base + sizeof(PinnedSlotHeader);
      const auto* footer = reinterpret_cast<const PinnedSlotFooter*>(
          base + sizeof(PinnedSlotHeader) +
          PinnedSlotWriter::RoundUp64(sbytes));
      const uint64_t footer_magic = footer->magic.load(std::memory_order_acquire);
      const uint64_t footer_index =
          footer->frame_index.load(std::memory_order_acquire);
      const uint64_t s1 = slot->seq.load(std::memory_order_acquire);
      const bool footer_ok = footer_magic == kPinnedSlotFooterMagic &&
                             footer_index == idx;
      if (s1 != s0 || !footer_ok) {
        if (s1 != s0) retries_ += 1;  // publish raced the copy
        torn_reads_ += 1;
        drops_ += 1;
        next_ += 1;
        continue;
      }
      out.payload = payload;
      out.data_len = len;
      out.mono_ns = mono;
      out.flags = f;
      out.crc32 = crc;
      out.frame_index = idx;
      out.slot_seq = s0;
      out.ClearLeaseCheck();  // plain cursor read: no lease protection
      const bool crc_on =
          (header_->flags.load(std::memory_order_relaxed) &
           kPinnedSlotFlagCrcEnabled) != 0;
      out.crc_ok = !crc_on || (crc != 0 && crc == Crc32(payload, len));
      next_ += 1;
      return true;
    }
    return false;
  }

  // R1 scratch protocol: copy the frame into caller-owned scratch and only
  // succeed if a second full validation after the copy sees an unchanged slot
  // and the copied bytes still match the stored crc. A producer overwriting
  // the slot mid-copy is caught (kTorn), so a torn frame can never be written
  // to disk as valid. scratch must hold slot_bytes bytes; out.payload points
  // into scratch on kOk.
  ReadResult ReadFrameStable(uint64_t frame_index, uint8_t* scratch,
                             uint32_t scratch_cap, View& out) {
    ReadResult r = ReadFrame(frame_index, out);
    if (r != ReadResult::kOk) {
      // out is only defined on kOk; ReadFrame's error paths leave it
      // untouched. Never leave a stale debug watch armed: the slot may be
      // rewritten before the caller destroys the view (BF-13).
      out.DisarmWatch();
      return r;
    }
    if (!out.crc_ok || out.data_len > scratch_cap) {
      out.DisarmWatch();
      torn_reads_ += 1;  // crc mismatch or no room: treat as unusable
      return ReadResult::kTorn;
    }
    const uint32_t len = out.data_len;
    const uint64_t seq0 = out.slot_seq;
    const uint32_t crc0 = out.crc32;
    const uint64_t mono0 = out.mono_ns;
    detail::AtomicPayloadCopyOut(scratch, out.payload, len);
    View v2;
    v2.DisarmWatch();  // defensive: in case a previous ReadFrame armed a watch
    const ReadResult r2 = ReadFrame(frame_index, v2);
    v2.DisarmWatch();
    if (r2 != ReadResult::kOk || v2.slot_seq != seq0 || v2.crc32 != crc0 ||
        v2.mono_ns != mono0 || v2.data_len != len ||
        (crc0 != 0 && Crc32(scratch, len) != crc0)) {
      out.DisarmWatch();
      torn_reads_ += 1;
      return ReadResult::kTorn;
    }
    out.payload = scratch;
    out.DisarmWatch();  // the stable-copy protocol validated; stop watching
    out.ClearLeaseCheck();  // scratch is caller-owned, unrelated to the lease
    return ReadResult::kOk;
  }

  // Blocking variant: waits on the epoch futex until a frame is available or
  // the absolute monotonic deadline passes, then TryRead()s once.
  bool WaitView(View& out, uint64_t deadline_ns) {
    WaitUntil(deadline_ns);
    return TryRead(out);
  }

  bool empty() const override {
    if (header_ == nullptr) return true;
    return next_ >= header_->head.load(std::memory_order_acquire);
  }

  void Notify() override {}
  void SetWakeWord(std::atomic<uint32_t>* wake_word) { wake_word_ = wake_word; }
  std::atomic<uint32_t>* NotifyWakeWord() const override { return wake_word_; }

  void WaitUntil(uint64_t deadline_ns) override {
    if (header_ == nullptr) return;
    for (;;) {
      if (!empty()) return;
      const uint64_t now = MonoNs();
      if (now >= deadline_ns) return;
      const uint32_t snap = header_->epoch.load(std::memory_order_seq_cst);
      if (!empty()) return;
      const uint64_t rem = deadline_ns - now;
      struct timespec rel {};
      rel.tv_sec = static_cast<time_t>(rem / 1000000000ULL);
      rel.tv_nsec = static_cast<long>(rem % 1000000000ULL);
      FutexWait(const_cast<std::atomic<uint32_t>*>(&header_->epoch), snap,
                &rel);
#ifndef __linux__
      struct timespec req {0, 1000000};
      nanosleep(&req, nullptr);
#endif
    }
  }

  uint64_t drop_count() const { return drops_; }
  // Move the follow cursor to the current head (stream-from-now, used by
  // continuous-mode streaming drains at arm/reattach). Skipped frames are NOT
  // counted as drops — this is an explicit reposition, not a lag.
  void SeekToHead() {
    if (header_ == nullptr) return;
    next_ = header_->head.load(std::memory_order_acquire);
  }
  // Current cursor position (next frame_index TryRead will attempt).
  uint64_t cursor() const { return next_; }
  // Torn/incomplete frames observed by this reader (seq or footer mismatch
  // after the slot looked published). Process-local, like drop_count() —
  // readers typically map the region read-only and cannot count into shm.
  uint64_t torn_read_count() const { return torn_reads_; }
  // RT-09: bounded-retry accounting for the seqlock loops of ReadFrame() and
  // TryRead() (NextLease never loops — see its comment). Counted only when a
  // read has to re-read, so the success path is untouched. Process-local and
  // non-atomic like drop_count(): one reader thread owns the cursor.
  uint64_t retry_count() const { return retries_; }
  // BF-12: leases whose generation no longer matched when still_valid() or
  // Release() checked them (i.e. the writer stole the slot). Counted once per
  // lease object; process-local, atomic so a monitor thread can sample it.
  uint64_t lease_invalidated_count() const {
    return lease_invalidated_.load(std::memory_order_relaxed);
  }

 private:
  // Generation-matched unpin — decrement only when gen matches; on a gen
  // mismatch (the slot was stolen and its generation advanced) skip outright,
  // so it never underflows or decrements a new reader's pin. The epoch is not
  // cleared when the pin reaches zero either (v3 invariant): a concurrent
  // acquire writes the epoch before the pin CAS, so any late clear could erase
  // a live pin's epoch and produce pin>0 && epoch==0.
  static void UnpinGen(PinnedSlotLeaseCell& cell, uint32_t gen) {
    uint64_t cur = cell.pin_gen.load(std::memory_order_acquire);
    for (;;) {
      if (LeaseGenOf(cur) != gen) return;
      const uint32_t pin = LeasePinOf(cur);
      if (pin == 0) return;
      if (cell.pin_gen.compare_exchange_weak(
              cur, LeasePack(gen, pin - 1, LeaseCopyActiveOf(cur)),
              std::memory_order_acq_rel, std::memory_order_acquire)) {
        return;
      }
    }
  }

  PinnedSlotLeaseCell& LeaseCellOf(uint64_t frame_index) const {
    return reinterpret_cast<PinnedSlotLeaseCell*>(lease_ + 1)[
        frame_index % slot_count()];
  }

  const uint8_t* SlotBase(uint64_t index) const {
    const uint32_t count = slot_count();
    const uint32_t stride =
        header_->slot_stride.load(std::memory_order_relaxed);
    const uint32_t meta_blob_bytes =
        header_->meta_blob_bytes.load(std::memory_order_relaxed);
    return reinterpret_cast<const uint8_t*>(header_) +
           PinnedSlotWriter::SlotsOffset(meta_blob_bytes) +
           (index % count) * static_cast<uint64_t>(stride);
  }

  static uint64_t MonoNs() {
    struct timespec ts {};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1000000000ULL +
           static_cast<uint64_t>(ts.tv_nsec);
  }

  ShmRegion region_;
  ShmRegion lease_region_;
  const PinnedSlotPageHeader* header_{nullptr};
  PinnedSlotLeaseHeader* lease_{nullptr};
  bool lease_read_only_{false};
  uint64_t copy_owner_pid_{0};
  uint64_t copy_owner_start_{0};
  uint64_t next_{0};
  std::atomic<uint32_t>* wake_word_{nullptr};
  uint64_t drops_{0};
  uint64_t torn_reads_{0};
  uint64_t retries_{0};  // RT-09: bounded-retry count (see retry_count())
  std::atomic<uint64_t> lease_invalidated_{0};  // BF-12, sampled cross-thread
  void (*epoch_stamp_test_hook_)(void*){nullptr};
  void* epoch_stamp_test_hook_ctx_{nullptr};
};

}  // namespace thyme
