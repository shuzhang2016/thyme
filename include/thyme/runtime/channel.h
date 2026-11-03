#pragma once

#include "thyme/contracts/frame.h"
#include "thyme/runtime/pinned_slot_store.h"
#include "thyme/runtime/channel_capability.h"
#include "thyme/runtime/latest_slot.h"
#include "thyme/runtime/notif_pipe.h"
#include "thyme/runtime/pinned_latest_store.h"
#include "thyme/runtime/pinned_spmc_ring.h"
#include "thyme/runtime/shm_spsc_ring.h"
#include "thyme/runtime/shm_spmc_ring.h"

#include <cstddef>
#include <cstdint>
#include <ctime>
#include <type_traits>
#include <utility>

namespace thyme {

struct ChannelSemantics {
  ChannelTransportKind transport_kind = ChannelTransportKind::kFrame;
  ChannelRetention retention = ChannelRetention::kLatest;
  ChannelReaders readers = ChannelReaders::kOne;
  ChannelOverflow overflow = ChannelOverflow::kOverwrite;
};

constexpr uint32_t ChannelSemanticsPack(ChannelTransportKind transport,
                                        ChannelRetention retention,
                                        ChannelReaders readers,
                                        ChannelOverflow overflow) {
  return static_cast<uint32_t>(transport) |
         (static_cast<uint32_t>(retention) << 8) |
         (static_cast<uint32_t>(readers) << 16) |
         (static_cast<uint32_t>(overflow) << 24);
}

enum class ChannelBackend : uint8_t {
  kLocal = 0,
  kShm = 1,
};

enum class ChannelWireFormat : uint8_t {
  kFrame = 0,
  kRaw = 1,
};

enum class ChannelStatus : uint8_t {
  kOk = 0,
  kEmpty,
  kFull,
  kOverwritten,
  kPinnedSkip,
  kGated,
  kOversize,
  kTorn,
  kBusy,
  kBackpressure,
  kBadLayout,
  kInvalid,
  // No endpoint was bound. Distinct from kInvalid (wrong object, bad argument)
  // so a caller can tell "this channel is not wired" from "this call is wrong".
  kUnbound,
};

struct ChannelConfig {
  uint32_t magic = 0;
  uint16_t version = 0;
  uint64_t session_id = 0;
};

template <typename T>
struct ChannelEnvelope {
  uint64_t mono_ns = 0;
  uint64_t sequence = 0;
  T payload{};
};

template <typename T, bool OwnPayload>
struct ChannelSampleStorage {};

template <typename T>
struct ChannelSampleStorage<T, true> {
  Frame<T> frame{};
  T raw{};
};

// Fixed-size sample (frame transport): the read copies the payload into the
// sample's own storage. It exposes payload/frame() only — data/size and borrow
// handles belong to the blob sample used by pinned-slot channels, so using the
// wrong accessor for a transport is a compile error instead of a null check.
template <typename T>
class ChannelFixedSample {
  static_assert(std::is_trivially_copyable<T>::value,
                "ChannelFixedSample payload must be trivially copyable");
  static_assert(std::is_standard_layout<T>::value,
                "ChannelFixedSample payload must be standard layout");

 public:
  ChannelFixedSample() = default;
  ~ChannelFixedSample() { Reset(); }
  ChannelFixedSample(const ChannelFixedSample&) = delete;
  ChannelFixedSample& operator=(const ChannelFixedSample&) = delete;
  ChannelFixedSample(ChannelFixedSample&& other) noexcept { MoveFrom(other); }
  ChannelFixedSample& operator=(ChannelFixedSample&& other) noexcept {
    if (this != &other) {
      Reset();
      MoveFrom(other);
    }
    return *this;
  }

  const T* payload = nullptr;
  uint64_t mono_ns = 0;
  uint64_t sequence = 0;

  bool valid() const { return kind_ != Kind::kNone; }
  bool still_valid() const { return kind_ == Kind::kCopy; }

  bool CopyTo(T* out) const {
    if (out == nullptr || payload == nullptr || !still_valid()) return false;
    *out = *payload;
    return true;
  }

  const Frame<T>* frame() const {
    return kind_ == Kind::kCopy ? &storage_.frame : nullptr;
  }

  void Reset() {
    kind_ = Kind::kNone;
    payload = nullptr;
    mono_ns = 0;
    sequence = 0;
  }

 private:
  template <typename U, uint32_t Semantics, ChannelBackend Backend,
            std::size_t Cap, std::size_t LocalSlots, std::size_t Readers,
            ChannelWireFormat Wire>
  friend class Channel;

  enum class Kind : uint8_t { kNone, kCopy };

  void MoveFrom(ChannelFixedSample& other) {
    Reset();
    kind_ = other.kind_;
    mono_ns = other.mono_ns;
    sequence = other.sequence;
    if (kind_ == Kind::kCopy) {
      storage_ = other.storage_;
      if (other.payload == &other.storage_.raw) {
        payload = &storage_.raw;
      } else if (other.payload == &other.storage_.frame.payload) {
        payload = &storage_.frame.payload;
      }
    }
    other.kind_ = Kind::kNone;
    other.payload = nullptr;
    other.mono_ns = 0;
    other.sequence = 0;
  }

  // Direct read target: the seqlock/ring copies straight into the sample's own
  // storage, so a copy-mode read costs one memcpy instead of two.
  T* raw_target() { return &storage_.raw; }
  Frame<T>* frame_target() { return &storage_.frame; }

  void AdoptRawCopy(uint64_t timestamp, uint64_t index) {
    Reset();
    kind_ = Kind::kCopy;
    mono_ns = timestamp;
    sequence = index;
    payload = &storage_.raw;
  }

  void AdoptFrameCopy() {
    Reset();
    kind_ = Kind::kCopy;
    mono_ns = storage_.frame.hdr.mono_ns;
    sequence = storage_.frame.hdr.sequence;
    payload = &storage_.frame.payload;
  }

  Kind kind_ = Kind::kNone;
  ChannelSampleStorage<T, true> storage_{};
};

template <typename T, std::size_t Slots = 4, std::size_t MaxReaders = 8,
          bool OwnPayload = true>
class ChannelSample {
  static_assert(std::is_trivially_copyable<T>::value,
                "ChannelSample payload must be trivially copyable");
  static_assert(std::is_standard_layout<T>::value,
                "ChannelSample payload must be standard layout");

 public:
  ChannelSample() = default;
  ~ChannelSample() { Reset(); }
  ChannelSample(const ChannelSample&) = delete;
  ChannelSample& operator=(const ChannelSample&) = delete;
  ChannelSample(ChannelSample&& other) noexcept { MoveFrom(other); }
  ChannelSample& operator=(ChannelSample&& other) noexcept {
    if (this != &other) {
      Reset();
      MoveFrom(other);
    }
    return *this;
  }

  const uint8_t* data = nullptr;
  uint32_t size = 0;
  uint64_t mono_ns = 0;
  uint64_t sequence = 0;

  bool valid() const { return kind_ != Kind::kNone; }
  bool still_valid() const {
    switch (kind_) {
      case Kind::kNone: return false;
      case Kind::kCopy: return true;
      case Kind::kPinnedLatest: return static_cast<bool>(latest_);
      case Kind::kPinnedRing: return ring_.still_valid();
      case Kind::kPinnedSlot: return big_.still_valid();
    }
    return false;
  }

  bool CopyTo(T* out) const {
    if (out == nullptr || data == nullptr || size != sizeof(T) || !still_valid()) {
      return false;
    }
    *out = *reinterpret_cast<const T*>(data);
    return true;
  }

  void Reset() {
    latest_.Reset();
    ring_.Reset();
    (void)big_.Release();
    kind_ = Kind::kNone;
    data = nullptr;
    size = 0;
    mono_ns = 0;
    sequence = 0;
  }

 private:
  template <typename U, uint32_t Semantics, ChannelBackend Backend,
            std::size_t Cap, std::size_t LocalSlots, std::size_t Readers,
            ChannelWireFormat Wire>
  friend class Channel;

  enum class Kind : uint8_t { kNone, kCopy, kPinnedLatest, kPinnedRing, kPinnedSlot };

  void SetBytes(const T* value, uint32_t bytes) {
    data = reinterpret_cast<const uint8_t*>(value);
    size = bytes;
  }

  // Moving a sample must not leave `payload` pointing into the source's
  // storage: copy-mode samples are re-pointed at this object's own buffer.
  void MoveFrom(ChannelSample& other) {
    Reset();
    kind_ = other.kind_;
    mono_ns = other.mono_ns;
    sequence = other.sequence;
    latest_ = std::move(other.latest_);
    ring_ = std::move(other.ring_);
    big_ = std::move(other.big_);
    size = other.size;
    if (kind_ == Kind::kCopy) {
      if constexpr (OwnPayload) {
        storage_ = other.storage_;
        if (other.data == reinterpret_cast<const uint8_t*>(&other.storage_.raw)) {
          data = reinterpret_cast<const uint8_t*>(&storage_.raw);
        } else if (other.data ==
                   reinterpret_cast<const uint8_t*>(&other.storage_.frame.payload)) {
          data = reinterpret_cast<const uint8_t*>(&storage_.frame.payload);
        } else {
          data = nullptr;
        }
      } else {
        data = nullptr;
      }
    } else {
      data = other.data;
    }
    other.kind_ = Kind::kNone;
    other.data = nullptr;
    other.size = 0;
    other.mono_ns = 0;
    other.sequence = 0;
  }

  // Direct read target: the seqlock/ring copies straight into the sample's own
  // storage, so a copy-mode read costs one memcpy instead of two.
  T* raw_target() {
    static_assert(OwnPayload, "lease samples do not own a copy target");
    return &storage_.raw;
  }

  Frame<T>* frame_target() {
    static_assert(OwnPayload, "lease samples do not own a copy target");
    return &storage_.frame;
  }

  void AdoptRawCopy(uint64_t timestamp, uint64_t index) {
    static_assert(OwnPayload, "lease samples do not own a raw copy");
    Reset();
    kind_ = Kind::kCopy;
    mono_ns = timestamp;
    sequence = index;
    SetBytes(&storage_.raw, sizeof(T));
  }

  void AdoptFrameCopy() {
    static_assert(OwnPayload, "lease samples do not own a Frame copy");
    Reset();
    kind_ = Kind::kCopy;
    mono_ns = storage_.frame.hdr.mono_ns;
    sequence = storage_.frame.hdr.sequence;
    SetBytes(&storage_.frame.payload, sizeof(T));
  }

  void SetLatest(typename PinnedLatestStore<ChannelEnvelope<T>, Slots>::Borrowed&& value) {
    Reset();
    mono_ns = value->mono_ns;
    sequence = value->sequence;
    SetBytes(&value->payload, sizeof(T));
    latest_ = std::move(value);
    kind_ = Kind::kPinnedLatest;
  }

  void SetRing(typename PinnedSpmcRing<ChannelEnvelope<T>, Slots, MaxReaders>::Borrowed&& value) {
    Reset();
    mono_ns = value->mono_ns;
    sequence = value->sequence;
    SetBytes(&value->payload, sizeof(T));
    ring_ = std::move(value);
    kind_ = Kind::kPinnedRing;
  }

  void SetBig(PinnedSlotReader::BorrowedFrame&& value) {
    Reset();
    mono_ns = value.mono_ns();
    sequence = value.index();
    SetBytes(reinterpret_cast<const T*>(value.data()), value.size());
    big_ = std::move(value);
    kind_ = Kind::kPinnedSlot;
  }

  Kind kind_ = Kind::kNone;
  ChannelSampleStorage<T, OwnPayload> storage_{};
  typename PinnedLatestStore<ChannelEnvelope<T>, Slots>::Borrowed latest_;
  typename PinnedSpmcRing<ChannelEnvelope<T>, Slots, MaxReaders>::Borrowed ring_;
  PinnedSlotReader::BorrowedFrame big_;
};

template <typename T>
class ChannelWrite {
 public:
  ChannelWrite() = default;
  ChannelWrite(const ChannelWrite&) = delete;
  ChannelWrite& operator=(const ChannelWrite&) = delete;
  ChannelWrite(ChannelWrite&& other) noexcept { MoveFrom(other); }
  ChannelWrite& operator=(ChannelWrite&& other) noexcept {
    if (this != &other) MoveFrom(other);
    return *this;
  }

  T* payload = nullptr;
  uint8_t* data = nullptr;
  uint32_t size = 0;
  uint64_t mono_ns = 0;
  uint64_t sequence = 0;
  ChannelStatus status = ChannelStatus::kInvalid;

  // A write belongs to the channel that produced it. Committing it through a
  // different channel is rejected instead of silently publishing into a slot
  // that the other channel never reserved. bound() means "this handle still
  // holds a live slot", so a failed Begin and a committed handle are both false.
  bool bound() const { return owner_ != nullptr && status == ChannelStatus::kOk; }

  const Frame<T>* frame_ptr() const { return kind == Kind::kFrame ? frame : nullptr; }

  ChannelStatus Commit() {
    if (owner_ == nullptr) return ChannelStatus::kUnbound;
    if (status != ChannelStatus::kOk) return status;
    return commit_(owner_, this);
  }

  void Abort() {
    if (owner_ != nullptr) abort_(owner_, this);
  }

 private:
  template <typename U, uint32_t Semantics, ChannelBackend Backend,
            std::size_t Cap, std::size_t LocalSlots, std::size_t Readers,
            ChannelWireFormat Wire>
  friend class Channel;

  void MoveFrom(ChannelWrite& other) {
    kind = other.kind;
    frame = other.frame;
    raw = other.raw;
    envelope = other.envelope;
    big = other.big;
    payload = other.payload;
    data = other.data;
    size = other.size;
    mono_ns = other.mono_ns;
    sequence = other.sequence;
    status = other.status;
    owner_ = other.owner_;
    commit_ = other.commit_;
    abort_ = other.abort_;
    other.kind = Kind::kNone;
    other.payload = nullptr;
    other.data = nullptr;
    other.size = 0;
    other.owner_ = nullptr;
    other.commit_ = nullptr;
    other.abort_ = nullptr;
  }

  enum class Kind : uint8_t { kNone, kFrame, kRaw, kPinnedSlot, kEnvelope };
  Kind kind = Kind::kNone;
  Frame<T>* frame = nullptr;
  T* raw = nullptr;
  ChannelEnvelope<T>* envelope = nullptr;
  PinnedSlotWriter::BeginView big{};
  void* owner_ = nullptr;
  ChannelStatus (*commit_)(void*, ChannelWrite*) = nullptr;
  void (*abort_)(void*, ChannelWrite*) = nullptr;
};

template <typename T, uint32_t Semantics, ChannelBackend Backend,
          std::size_t Cap = 4, std::size_t LocalSlots = 4,
          std::size_t MaxReaders = 8,
          ChannelWireFormat Wire = ChannelWireFormat::kFrame>
class Channel {
  static_assert(Cap >= 2, "Channel queue capacity must be at least two");
  static_assert(LocalSlots >= 2, "Channel pin capacity must be at least two");
  static_assert(std::is_trivially_copyable<T>::value,
                "Channel payload must be trivially copyable");
  static_assert(std::is_standard_layout<T>::value,
                "Channel payload must be standard layout");

 public:
  static constexpr ChannelTransportKind kDeclaredTransport =
      static_cast<ChannelTransportKind>(Semantics & 0xFFu);
  static constexpr ChannelRetention kRetention =
      static_cast<ChannelRetention>((Semantics >> 8) & 0xFFu);
  static constexpr ChannelReaders kReaders =
      static_cast<ChannelReaders>((Semantics >> 16) & 0xFFu);
  static constexpr ChannelOverflow kOverflow =
      static_cast<ChannelOverflow>((Semantics >> 24) & 0xFFu);
  static constexpr ChannelTransportKind kTransport =
      (kDeclaredTransport == ChannelTransportKind::kPinnedSlot ||
       (Wire == ChannelWireFormat::kFrame && FramePodWireBytes<Frame<T>> > 0xFFFFu))
          ? ChannelTransportKind::kPinnedSlot
          : ChannelTransportKind::kFrame;
  static constexpr bool kShmBroadcast =
      kTransport == ChannelTransportKind::kFrame &&
      Backend == ChannelBackend::kShm && kReaders == ChannelReaders::kMany;
  static constexpr bool kRaw = Wire == ChannelWireFormat::kRaw;
  // The slot carries either the framed image or the bare payload. Everything
  // below is written once against Slot instead of once per wire format.
  using Slot = std::conditional_t<kRaw, T, Frame<T>>;
  static_assert(!kShmBroadcast || kOverflow == ChannelOverflow::kOverwrite,
                "SHM broadcast always overwrites");
  static_assert(kTransport == ChannelTransportKind::kPinnedSlot ||
                    kRetention != ChannelRetention::kLatest ||
                    kOverflow == ChannelOverflow::kOverwrite,
                "latest channels always overwrite");
  static_assert(kTransport == ChannelTransportKind::kPinnedSlot ||
                    Backend == ChannelBackend::kShm ||
                    kRetention != ChannelRetention::kQueue ||
                    kOverflow == ChannelOverflow::kDrop ||
                    kOverflow == ChannelOverflow::kReject ||
                    (kReaders == ChannelReaders::kMany &&
                     kOverflow == ChannelOverflow::kOverwrite),
                "local queue overflow must match its backend adapter");
  static_assert(kTransport == ChannelTransportKind::kPinnedSlot ||
                    Backend == ChannelBackend::kLocal ||
                    kShmBroadcast || kRetention != ChannelRetention::kQueue ||
                    kOverflow == ChannelOverflow::kReject,
                "SHM queues report producer backpressure");
  static_assert(kTransport != ChannelTransportKind::kPinnedSlot ||
                    kOverflow == ChannelOverflow::kDrop,
                "PinnedSlot drops a publish when all slots are pinned");

  using Sample = std::conditional_t<
      kTransport == ChannelTransportKind::kFrame, ChannelFixedSample<T>,
      ChannelSample<T, LocalSlots, MaxReaders, false>>;
  using Write = ChannelWrite<T>;
  using Envelope = ChannelEnvelope<T>;
  using PinnedLatest = PinnedLatestStore<Envelope, LocalSlots>;
  using PinnedRing = PinnedSpmcRing<Envelope, LocalSlots, MaxReaders>;

  explicit Channel(ChannelConfig config = {}) : config_(config) {}
  Channel(const Channel&) = default;
  Channel& operator=(const Channel&) = default;

  // Bind overloads are type-parameterised on the slot: the same overload
  // accepts Frame<T> for a framed channel and T for a raw one, and the
  // static_assert names the semantics instead of duplicating the tree.
  template <typename S>
  void Bind(LatestSlot<S>* endpoint) {
    static_assert(std::is_same<S, Slot>::value,
                  "LatestSlot slot type must match the channel wire format");
    static_assert(kTransport == ChannelTransportKind::kFrame &&
                      kRetention == ChannelRetention::kLatest,
                  "LatestSlot requires latest frame semantics");
    ep_.latest = endpoint;
  }
  template <typename S>
  void Bind(NotifPipe<S, Cap>* endpoint) {
    static_assert(std::is_same<S, Slot>::value,
                  "NotifPipe slot type must match the channel wire format");
    static_assert(kTransport == ChannelTransportKind::kFrame &&
                      Backend == ChannelBackend::kLocal &&
                      kRetention == ChannelRetention::kQueue,
                  "NotifPipe requires local queue semantics");
    ep_.notif = endpoint;
  }
  template <typename S>
  void Bind(ShmSpscRing<S, Cap>* endpoint) {
    static_assert(std::is_same<S, Slot>::value,
                  "ShmSpscRing slot type must match the channel wire format");
    static_assert(kTransport == ChannelTransportKind::kFrame &&
                      Backend == ChannelBackend::kShm &&
                      kReaders == ChannelReaders::kOne &&
                      kRetention == ChannelRetention::kQueue,
                  "ShmSpscRing requires SHM queue semantics");
    ep_.spsc = endpoint;
  }
  template <typename S>
  void Bind(ShmSpmcRing<S, Cap>* endpoint) {
    static_assert(std::is_same<S, Slot>::value,
                  "ShmSpmcRing slot type must match the channel wire format");
    static_assert(kShmBroadcast, "ShmSpmcRing requires SHM broadcast semantics");
    ep_.spmc = endpoint;
  }
  template <typename S>
  void BindReader(SpmcReader<S, Cap>* endpoint) {
    static_assert(std::is_same<S, Slot>::value,
                  "SpmcReader slot type must match the channel wire format");
    static_assert(kShmBroadcast, "SpmcReader requires SHM broadcast semantics");
    rp_.spmc_reader = endpoint;
  }

  void Bind(PinnedLatest* endpoint) {
    static_assert(kTransport == ChannelTransportKind::kPinnedSlot &&
                      Backend == ChannelBackend::kLocal &&
                      kRetention == ChannelRetention::kLatest,
                  "PinnedLatest requires local PinnedSlot latest semantics");
    ep_.pinned_latest = endpoint;
  }
  void Bind(PinnedRing* endpoint) {
    static_assert(kTransport == ChannelTransportKind::kPinnedSlot &&
                      Backend == ChannelBackend::kLocal &&
                      kRetention == ChannelRetention::kQueue,
                  "PinnedRing requires local PinnedSlot queue semantics");
    ep_.pinned_ring = endpoint;
  }
  void BindReader(typename PinnedRing::Reader* endpoint) {
    static_assert(kTransport == ChannelTransportKind::kPinnedSlot &&
                      Backend == ChannelBackend::kLocal &&
                      kRetention == ChannelRetention::kQueue,
                  "PinnedRing::Reader requires local PinnedSlot queue semantics");
    rp_.pinned_reader = endpoint;
  }
  void Bind(PinnedSlotWriter* endpoint) {
    static_assert(kTransport == ChannelTransportKind::kPinnedSlot &&
                      Backend == ChannelBackend::kShm,
                  "PinnedSlotWriter requires SHM PinnedSlot semantics");
    ep_.big_writer = endpoint;
  }
  void Bind(PinnedSlotReader* endpoint) {
    static_assert(kTransport == ChannelTransportKind::kPinnedSlot &&
                      Backend == ChannelBackend::kShm,
                  "PinnedSlotReader requires SHM PinnedSlot semantics");
    rp_.big_reader = endpoint;
  }

  // True when the endpoint this channel's compile-time semantics select has
  // been bound. Distinguishes "not wired" from "call rejected".
  bool wired() const {
    if constexpr (kTransport == ChannelTransportKind::kPinnedSlot) {
      if constexpr (Backend == ChannelBackend::kShm) return ep_.big_writer != nullptr;
      else if constexpr (kRetention == ChannelRetention::kLatest)
        return ep_.pinned_latest != nullptr;
      else return ep_.pinned_ring != nullptr;
    } else if constexpr (kShmBroadcast) {
      return ep_.spmc != nullptr;
    } else if constexpr (kRetention == ChannelRetention::kLatest) {
      return ep_.latest != nullptr;
    } else if constexpr (Backend == ChannelBackend::kLocal) {
      return ep_.notif != nullptr;
    } else {
      return ep_.spsc != nullptr;
    }
  }

  bool reader_wired() const {
    if constexpr (kTransport == ChannelTransportKind::kPinnedSlot) {
      if constexpr (Backend == ChannelBackend::kShm) return rp_.big_reader != nullptr;
      else if constexpr (kRetention == ChannelRetention::kLatest)
        return ep_.pinned_latest != nullptr;
      else return rp_.pinned_reader != nullptr;
    } else if constexpr (kShmBroadcast) {
      return rp_.spmc_reader != nullptr;
    } else if constexpr (kRetention == ChannelRetention::kLatest) {
      return ep_.latest != nullptr;
    } else if constexpr (Backend == ChannelBackend::kLocal) {
      return ep_.notif != nullptr;
    } else {
      return ep_.spsc != nullptr;
    }
  }

  Write BeginWrite() { return BeginWriteAt(sizeof(T), 0); }

  Write BeginWriteAt(uint32_t data_len, uint64_t mono_ns) {
    Write out;
    out.size = data_len;
    out.owner_ = this;
    out.commit_ = &Channel::CommitThunk;
    out.abort_ = &Channel::AbortThunk;
    if (kTransport != ChannelTransportKind::kPinnedSlot && data_len != sizeof(T)) {
      out.status = ChannelStatus::kOversize;
      return out;
    }
    if (kTransport == ChannelTransportKind::kPinnedSlot &&
        Backend == ChannelBackend::kLocal && data_len != sizeof(T)) {
      out.status = ChannelStatus::kOversize;
      return out;
    }
    if (!wired()) {
      out.status = ChannelStatus::kUnbound;
      return out;
    }

    if constexpr (kTransport == ChannelTransportKind::kPinnedSlot) {
      if constexpr (Backend == ChannelBackend::kShm) {
        out.big = ep_.big_writer->Begin(data_len);
        using Status = PinnedSlotWriter::BeginStatus;
        if (out.big.status != Status::kOk) {
          out.status = out.big.status == Status::kOversize
                           ? ChannelStatus::kOversize
                           : out.big.status == Status::kPinnedSkip
                                 ? ChannelStatus::kPinnedSkip
                                 : ChannelStatus::kGated;
          return out;
        }
        out.kind = Write::Kind::kPinnedSlot;
        out.data = out.big.payload;
        out.payload = data_len == sizeof(T) ? reinterpret_cast<T*>(out.data) : nullptr;
      } else if constexpr (kRetention == ChannelRetention::kLatest) {
        out.envelope = ep_.pinned_latest->BeginWrite();
        if (out.envelope == nullptr) {
          out.status = ChannelStatus::kPinnedSkip;
          return out;
        }
        out.kind = Write::Kind::kEnvelope;
        out.payload = &out.envelope->payload;
        out.data = reinterpret_cast<uint8_t*>(out.payload);
      } else {
        out.envelope = ep_.pinned_ring->BeginWrite();
        if (out.envelope == nullptr) {
          out.status = ChannelStatus::kPinnedSkip;
          return out;
        }
        out.kind = Write::Kind::kEnvelope;
        out.payload = &out.envelope->payload;
        out.data = reinterpret_cast<uint8_t*>(out.payload);
      }
    } else {
      Slot* slot = nullptr;
      if constexpr (kShmBroadcast) {
        slot = ep_.spmc->BeginWrite();
      } else if constexpr (kRetention == ChannelRetention::kLatest) {
        slot = ep_.latest->BeginWrite();
      } else if constexpr (Backend == ChannelBackend::kLocal) {
        slot = ep_.notif->BeginWrite();
      } else {
        slot = ep_.spsc->BeginWrite();
      }
      if (slot == nullptr) {
        out.status = ChannelStatus::kBackpressure;
        return out;
      }
      if constexpr (kRaw) {
        out.kind = Write::Kind::kRaw;
        out.raw = slot;
        out.payload = slot;
        out.data = reinterpret_cast<uint8_t*>(slot);
      } else {
        out.kind = Write::Kind::kFrame;
        out.frame = slot;
        out.payload = &slot->payload;
        out.data = reinterpret_cast<uint8_t*>(out.payload);
      }
    }

    out.sequence = ++sequence_;
    out.mono_ns = mono_ns != 0 ? mono_ns : NowNs();
    if (out.kind == Write::Kind::kEnvelope) {
      out.envelope->mono_ns = out.mono_ns;
      out.envelope->sequence = out.sequence;
    }
    if constexpr (Wire == ChannelWireFormat::kFrame &&
                  kTransport == ChannelTransportKind::kFrame) {
      if (out.kind == Write::Kind::kFrame) {
        FrameInit(*out.frame, static_cast<uint32_t>(out.sequence), out.mono_ns,
                  config_.session_id);
      }
    }
    out.status = ChannelStatus::kOk;
    return out;
  }

  ChannelStatus CommitWrite(Write* out) {
    if (out == nullptr) return ChannelStatus::kInvalid;
    if (out->owner_ != this) return ChannelStatus::kInvalid;
    if (out->status != ChannelStatus::kOk || out->data == nullptr) {
      return ChannelStatus::kInvalid;
    }
    switch (out->kind) {
      case Write::Kind::kFrame:
        FrameSeal(*out->frame, config_.magic, config_.version);
        CommitSlot();
        break;
      case Write::Kind::kRaw:
        CommitSlot();
        break;
      case Write::Kind::kPinnedSlot:
        ep_.big_writer->Commit(out->mono_ns);
        break;
      case Write::Kind::kEnvelope:
        if constexpr (kRetention == ChannelRetention::kLatest) {
          ep_.pinned_latest->CommitWrite();
        } else {
          ep_.pinned_ring->CommitWrite();
        }
        break;
      case Write::Kind::kNone:
        return ChannelStatus::kInvalid;
    }
    out->status = ChannelStatus::kInvalid;
    out->payload = nullptr;
    out->data = nullptr;
    return ChannelStatus::kOk;
  }

  void AbortWrite(Write* out) {
    if (out == nullptr) return;
    if (out->owner_ != this) return;
    if (out->status != ChannelStatus::kOk) return;
    switch (out->kind) {
      case Write::Kind::kFrame:
      case Write::Kind::kRaw:
        AbortSlot();
        break;
      case Write::Kind::kPinnedSlot:
        break;
      case Write::Kind::kEnvelope:
        if constexpr (kRetention == ChannelRetention::kLatest) {
          ep_.pinned_latest->AbortWrite();
        } else {
          ep_.pinned_ring->AbortWrite();
        }
        break;
      case Write::Kind::kNone:
        break;
    }
    out->status = ChannelStatus::kInvalid;
    out->payload = nullptr;
    out->data = nullptr;
  }

  ChannelStatus TryRead(Sample* out) {
    if (out == nullptr) return ChannelStatus::kInvalid;
    if (!reader_wired()) return ChannelStatus::kUnbound;
    if constexpr (kTransport == ChannelTransportKind::kPinnedSlot) {
      if constexpr (Backend == ChannelBackend::kShm) {
        auto frame = rp_.big_reader->NextLease();
        using Status = PinnedSlotReader::FrameStatus;
        if (frame.status() == Status::kEmpty) return ChannelStatus::kEmpty;
        if (frame.status() == Status::kLost) return ChannelStatus::kOverwritten;
        if (frame.status() == Status::kTorn) return ChannelStatus::kTorn;
        out->SetBig(std::move(frame));
      } else if constexpr (kRetention == ChannelRetention::kLatest) {
        typename PinnedLatest::Borrowed value;
        if (!ep_.pinned_latest->TryRead(&value)) return ChannelStatus::kEmpty;
        out->SetLatest(std::move(value));
      } else {
        typename PinnedRing::Borrowed value;
        if (!rp_.pinned_reader->TryRead(&value, PinnedReadMode::kFollow)) {
          return ChannelStatus::kEmpty;
        }
        out->SetRing(std::move(value));
      }
    } else {
      Slot* value;
      if constexpr (kRaw) {
        value = out->raw_target();
      } else {
        value = out->frame_target();
      }
      bool ok = false;
      if constexpr (kShmBroadcast) {
        if constexpr (kRetention == ChannelRetention::kLatest) {
          ok = rp_.spmc_reader->ReadLatest(*value);
        } else {
          ok = rp_.spmc_reader->TryPop(*value);
        }
      } else if constexpr (kRetention == ChannelRetention::kLatest) {
        ok = ep_.latest->ReadIfNew(seen_, *value);
      } else if constexpr (Backend == ChannelBackend::kLocal) {
        ok = ep_.notif->TryPop(*value);
      } else {
        ok = ep_.spsc->TryPop(*value);
      }
      if (!ok) return ChannelStatus::kEmpty;
      if constexpr (kRaw) {
        out->AdoptRawCopy(0, 0);
      } else {
        if (!FrameVerify(*value, config_.magic, config_.version)) {
          return ChannelStatus::kTorn;
        }
        out->AdoptFrameCopy();
      }
    }
    return ChannelStatus::kOk;
  }

  bool empty() const {
    if constexpr (kTransport == ChannelTransportKind::kPinnedSlot) {
      if constexpr (Backend == ChannelBackend::kShm) {
        return rp_.big_reader == nullptr || rp_.big_reader->empty();
      } else if constexpr (kRetention == ChannelRetention::kLatest) {
        return ep_.pinned_latest == nullptr || ep_.pinned_latest->empty();
      } else {
        return rp_.pinned_reader == nullptr || rp_.pinned_reader->empty();
      }
    } else if constexpr (kShmBroadcast) {
      return rp_.spmc_reader == nullptr || rp_.spmc_reader->empty();
    } else if constexpr (kRetention == ChannelRetention::kLatest) {
      return ep_.latest == nullptr || !ep_.latest->HasNew(seen_);
    } else if constexpr (Backend == ChannelBackend::kLocal) {
      return ep_.notif == nullptr || ep_.notif->empty();
    } else {
      return ep_.spsc == nullptr || ep_.spsc->empty();
    }
  }

  uint64_t drop_count() const {
    if constexpr (kShmBroadcast) {
      return rp_.spmc_reader != nullptr ? rp_.spmc_reader->drop_count() : 0;
    }
    return 0;
  }

  // Counters are only meaningful on the shapes that own one. Returning 0 from
  // drop_count()/push_fail_count() on other channels must not be read as "no
  // frames lost" — check the corresponding flag first.
  bool drop_count_observed() const {
    if constexpr (kShmBroadcast) return rp_.spmc_reader != nullptr;
    return false;
  }

  bool push_fail_count_observed() const {
    if constexpr (kTransport == ChannelTransportKind::kFrame &&
                  Backend == ChannelBackend::kShm &&
                  kReaders == ChannelReaders::kOne &&
                  kRetention == ChannelRetention::kQueue) {
      return ep_.spsc != nullptr;
    }
    return false;
  }

  uint32_t push_fail_count() const {
    if constexpr (kTransport == ChannelTransportKind::kFrame &&
                  Backend == ChannelBackend::kShm &&
                  kReaders == ChannelReaders::kOne &&
                  kRetention == ChannelRetention::kQueue) {
      return ep_.spsc != nullptr ? ep_.spsc->push_fail_count() : 0;
    }
    return 0;
  }

  void SetNotify(Waitable* target) {
    if constexpr (kTransport == ChannelTransportKind::kPinnedSlot) {
      if constexpr (Backend == ChannelBackend::kShm) {
        if (rp_.big_reader != nullptr) rp_.big_reader->SetNotify(target);
      } else if constexpr (kRetention == ChannelRetention::kLatest) {
        if (ep_.pinned_latest != nullptr) ep_.pinned_latest->SetNotify(target);
      } else {
        if (ep_.pinned_ring != nullptr) ep_.pinned_ring->SetNotify(target);
      }
    } else if constexpr (kShmBroadcast) {
      if (rp_.spmc_reader != nullptr) rp_.spmc_reader->SetNotify(target);
    } else if constexpr (Backend == ChannelBackend::kLocal &&
                         kRetention != ChannelRetention::kLatest) {
      if (ep_.notif != nullptr) ep_.notif->SetNotify(target);
    } else if constexpr (Backend == ChannelBackend::kShm &&
                         kRetention != ChannelRetention::kLatest) {
      if (ep_.spsc != nullptr) ep_.spsc->SetNotify(target);
    }
  }

  void WaitUntil(uint64_t deadline_ns) {
    if constexpr (kTransport == ChannelTransportKind::kPinnedSlot) {
      if constexpr (Backend == ChannelBackend::kShm) {
        if (rp_.big_reader != nullptr) rp_.big_reader->WaitUntil(deadline_ns);
      } else if constexpr (kRetention == ChannelRetention::kLatest) {
        if (ep_.pinned_latest != nullptr) ep_.pinned_latest->WaitUntil(deadline_ns);
      } else {
        if (rp_.pinned_reader != nullptr) rp_.pinned_reader->WaitUntil(deadline_ns);
      }
    } else if constexpr (kShmBroadcast) {
      if (rp_.spmc_reader != nullptr) rp_.spmc_reader->WaitUntil(deadline_ns);
    } else if constexpr (Backend == ChannelBackend::kLocal &&
                         kRetention != ChannelRetention::kLatest) {
      if (ep_.notif != nullptr) ep_.notif->WaitUntil(deadline_ns);
    } else if constexpr (Backend == ChannelBackend::kShm &&
                         kRetention != ChannelRetention::kLatest) {
      if (ep_.spsc != nullptr) ep_.spsc->WaitUntil(deadline_ns);
    }
  }

  std::atomic<uint32_t>* NotifyWakeWord() const {
    if constexpr (kTransport == ChannelTransportKind::kPinnedSlot) {
      if constexpr (Backend == ChannelBackend::kShm) {
        return rp_.big_reader != nullptr ? rp_.big_reader->NotifyWakeWord() : nullptr;
      } else if constexpr (kRetention == ChannelRetention::kLatest) {
        return ep_.pinned_latest != nullptr ? ep_.pinned_latest->NotifyWakeWord() : nullptr;
      } else {
        return rp_.pinned_reader != nullptr ? rp_.pinned_reader->NotifyWakeWord() : nullptr;
      }
    } else if constexpr (kShmBroadcast) {
      return rp_.spmc_reader != nullptr ? rp_.spmc_reader->NotifyWakeWord() : nullptr;
    } else if constexpr (Backend == ChannelBackend::kLocal &&
                         kRetention != ChannelRetention::kLatest) {
      return ep_.notif != nullptr ? ep_.notif->NotifyWakeWord() : nullptr;
    } else if constexpr (Backend == ChannelBackend::kShm &&
                         kRetention != ChannelRetention::kLatest) {
      return ep_.spsc != nullptr ? ep_.spsc->NotifyWakeWord() : nullptr;
    }
    return nullptr;
  }

 private:
  // One commit/abort body for every frame-path transport. The slot type is
  // already fixed by `Slot`, so only the delivery semantics branch here.
  void CommitSlot() {
    if constexpr (kShmBroadcast) {
      ep_.spmc->CommitWrite();
    } else if constexpr (kRetention == ChannelRetention::kLatest) {
      ep_.latest->CommitWrite();
    } else if constexpr (Backend == ChannelBackend::kLocal) {
      ep_.notif->CommitWrite();
    } else {
      ep_.spsc->CommitWrite();
    }
  }

  void AbortSlot() {
    if constexpr (kShmBroadcast) {
      ep_.spmc->AbortWrite();
    } else if constexpr (kRetention == ChannelRetention::kLatest) {
      ep_.latest->AbortWrite();
    } else if constexpr (Backend == ChannelBackend::kLocal) {
      ep_.notif->AbortWrite();
    } else {
      ep_.spsc->AbortWrite();
    }
  }

  static ChannelStatus CommitThunk(void* owner, Write* write) {
    return static_cast<Channel*>(owner)->CommitWrite(write);
  }

  static void AbortThunk(void* owner, Write* write) {
    static_cast<Channel*>(owner)->AbortWrite(write);
  }

  static uint64_t NowNs() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1000000000ULL +
           static_cast<uint64_t>(ts.tv_nsec);
  }

  ChannelConfig config_;
  uint64_t sequence_ = 0;
  uint64_t seen_ = 0;
  // One write endpoint and one read endpoint, regardless of delivery shape.
  // Only the member selected by the compile-time semantics is ever read, so the
  // unions are safe and a Channel costs two pointers instead of ten.
  union Endpoints {
    LatestSlot<Slot>* latest;
    NotifPipe<Slot, Cap>* notif;
    ShmSpscRing<Slot, Cap>* spsc;
    ShmSpmcRing<Slot, Cap>* spmc;
    PinnedLatest* pinned_latest;
    PinnedRing* pinned_ring;
    PinnedSlotWriter* big_writer;
  };
  union Readers {
    SpmcReader<Slot, Cap>* spmc_reader;
    typename PinnedRing::Reader* pinned_reader;
    PinnedSlotReader* big_reader;
  };
  Endpoints ep_{};
  Readers rp_{};
};

}  // namespace thyme
