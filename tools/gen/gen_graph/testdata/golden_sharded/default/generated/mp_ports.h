// Code generated for the generated graph. DO NOT EDIT.
// Hand logic belongs in src/nodes/.
// gen_graph 3.11.1 (output format 3)
#pragma once

#include "generated/graph_types.h"
#include "thyme/runtime/health_slot.h"
#include "thyme/runtime/notif_pipe.h"
#include "thyme/runtime/shm_spsc_ring.h"
#include "thyme/runtime/channel.h"
// SHM queue edges report producer backpressure instead of dropping.
inline constexpr uint32_t kShmSemanticsOf(uint32_t semantics) {
  const bool queue = ((semantics >> 8) & 0xFFu) ==
      static_cast<uint32_t>(thyme::ChannelRetention::kQueue);
  const bool one = ((semantics >> 16) & 0xFFu) ==
      static_cast<uint32_t>(thyme::ChannelReaders::kOne);
  const bool drop = ((semantics >> 24) & 0xFFu) ==
      static_cast<uint32_t>(thyme::ChannelOverflow::kDrop);
  return (queue && one && drop)
             ? ((semantics & 0x00FFFFFFu) |
                (static_cast<uint32_t>(thyme::ChannelOverflow::kReject) << 24))
             : semantics;
}

template <typename Local, typename Shm>
struct DualChannelRef : public thyme::Waitable {
  using Write = typename Local::Write;
  using Sample = typename Local::Sample;
  Local* local_p_ = nullptr;
  Shm* shm_p_ = nullptr;
  static Write Unbound() {
    Write out;
    out.status = thyme::ChannelStatus::kUnbound;
    return out;
  }
  Write BeginWrite() {
    if (local_p_ != nullptr) return local_p_->BeginWrite();
    if (shm_p_ != nullptr) return shm_p_->BeginWrite();
    return Unbound();
  }
  Write BeginWriteAt(uint32_t size, uint64_t mono_ns) {
    if (local_p_ != nullptr) return local_p_->BeginWriteAt(size, mono_ns);
    if (shm_p_ != nullptr) return shm_p_->BeginWriteAt(size, mono_ns);
    return Unbound();
  }
  thyme::ChannelStatus CommitWrite(Write* out) {
    if (local_p_ != nullptr) return local_p_->CommitWrite(out);
    if (shm_p_ != nullptr) return shm_p_->CommitWrite(out);
    return thyme::ChannelStatus::kUnbound;
  }
  void AbortWrite(Write* out) {
    if (local_p_ != nullptr) { local_p_->AbortWrite(out); return; }
    if (shm_p_ != nullptr) shm_p_->AbortWrite(out);
  }
  thyme::ChannelStatus TryRead(Sample* out) {
    if (local_p_ != nullptr) return local_p_->TryRead(out);
    if (shm_p_ != nullptr) return shm_p_->TryRead(out);
    return thyme::ChannelStatus::kUnbound;
  }
  bool empty() const override {
    if (local_p_ != nullptr) return local_p_->empty();
    if (shm_p_ != nullptr) return shm_p_->empty();
    return true;
  }
  uint64_t drop_count() const {
    return shm_p_ != nullptr ? shm_p_->drop_count() : 0;
  }
  bool drop_count_observed() const {
    return shm_p_ != nullptr && shm_p_->drop_count_observed();
  }
  bool push_fail_count_observed() const {
    return shm_p_ != nullptr && shm_p_->push_fail_count_observed();
  }
  uint32_t push_fail_count() const {
    return shm_p_ != nullptr ? shm_p_->push_fail_count() : 0;
  }
  void Notify() override {}
  void SetNotify(thyme::Waitable* target) override {
    if (local_p_ != nullptr) { local_p_->SetNotify(target); return; }
    if (shm_p_ != nullptr) shm_p_->SetNotify(target);
  }
  void WaitUntil(uint64_t deadline_ns) override {
    if (local_p_ != nullptr) { local_p_->WaitUntil(deadline_ns); return; }
    if (shm_p_ != nullptr) shm_p_->WaitUntil(deadline_ns);
  }
  std::atomic<uint32_t>* NotifyWakeWord() const override {
    if (local_p_ != nullptr) return local_p_->NotifyWakeWord();
    if (shm_p_ != nullptr) return shm_p_->NotifyWakeWord();
    return nullptr;
  }
  bool uses_local() const { return local_p_ != nullptr; }
};

template <typename T, uint32_t Semantics, std::size_t Cap>
struct FrameChannelRef
    : public DualChannelRef<
          thyme::Channel<T, Semantics, thyme::ChannelBackend::kLocal, Cap, Cap>,
          thyme::Channel<T, kShmSemanticsOf(Semantics),
                         thyme::ChannelBackend::kShm, Cap, Cap>> {
  using Local = thyme::Channel<T, Semantics, thyme::ChannelBackend::kLocal, Cap, Cap>;
  using Shm = thyme::Channel<T, kShmSemanticsOf(Semantics),
                              thyme::ChannelBackend::kShm, Cap, Cap>;
  using Base = DualChannelRef<Local, Shm>;
  using Write = typename Base::Write;
  using Sample = typename Base::Sample;
  Local local_;
  Shm shm_;
  FrameChannelRef() = default;
  // Copies re-point at their own channel storage: Ports are copied into
  // each node, and a shared Channel would share its sequence/reader
  // cursor across threads.
  FrameChannelRef(const FrameChannelRef& other)
      : local_(other.local_), shm_(other.shm_) {
    this->local_p_ = other.local_p_ != nullptr ? &local_ : nullptr;
    this->shm_p_ = other.shm_p_ != nullptr ? &shm_ : nullptr;
  }
  FrameChannelRef& operator=(const FrameChannelRef& other) {
    local_ = other.local_;
    shm_ = other.shm_;
    this->local_p_ = other.local_p_ != nullptr ? &local_ : nullptr;
    this->shm_p_ = other.shm_p_ != nullptr ? &shm_ : nullptr;
    return *this;
  }
  FrameChannelRef(thyme::LatestSlot<thyme::Frame<T>>* endpoint) { Bind(endpoint); }
  FrameChannelRef(thyme::NotifPipe<thyme::Frame<T>, Cap>* endpoint) { Bind(endpoint); }
  FrameChannelRef(thyme::ShmSpscRing<thyme::Frame<T>, Cap>* endpoint) { Bind(endpoint); }
  FrameChannelRef(thyme::ShmSpmcRing<thyme::Frame<T>, Cap>* endpoint) { Bind(endpoint); }
  FrameChannelRef(thyme::SpmcReader<thyme::Frame<T>, Cap>* endpoint) { BindReader(endpoint); }
  void Bind(thyme::LatestSlot<thyme::Frame<T>>* endpoint) {
    local_.Bind(endpoint); this->local_p_ = &local_;
  }
  void Bind(thyme::NotifPipe<thyme::Frame<T>, Cap>* endpoint) {
    local_.Bind(endpoint); this->local_p_ = &local_;
  }
  void Bind(thyme::ShmSpscRing<thyme::Frame<T>, Cap>* endpoint) {
    shm_.Bind(endpoint); this->shm_p_ = &shm_;
  }
  void Bind(thyme::ShmSpmcRing<thyme::Frame<T>, Cap>* endpoint) {
    shm_.Bind(endpoint); this->shm_p_ = &shm_;
  }
  void BindReader(thyme::SpmcReader<thyme::Frame<T>, Cap>* endpoint) {
    shm_.BindReader(endpoint); this->shm_p_ = &shm_;
  }
  bool uses_notif_pipe() const { return this->local_p_ != nullptr; }
};


// Shared per-node construction context: the own health slot plus the
// catalog extra buffers. Constructed once per node at the call site;
// generated bases take (const <type>Ports&, const MpContext&) instead
// of 15-26 flat pool/ring/extra arguments.
struct MpContext {
  thyme::HealthSlot* health;
};

struct ProducerThreadPorts {
  FrameChannelRef<Reading, thyme::ChannelSemanticsPack(thyme::ChannelTransportKind::kFrame, thyme::ChannelRetention::kQueue, thyme::ChannelReaders::kOne, thyme::ChannelOverflow::kDrop), 8> c0_;
  FrameChannelRef<Reading, thyme::ChannelSemanticsPack(thyme::ChannelTransportKind::kFrame, thyme::ChannelRetention::kQueue, thyme::ChannelReaders::kOne, thyme::ChannelOverflow::kDrop), 8> c1_;
  FrameChannelRef<Reading, thyme::ChannelSemanticsPack(thyme::ChannelTransportKind::kFrame, thyme::ChannelRetention::kQueue, thyme::ChannelReaders::kOne, thyme::ChannelOverflow::kDrop), 8> c2_;
  FrameChannelRef<Reading, thyme::ChannelSemanticsPack(thyme::ChannelTransportKind::kFrame, thyme::ChannelRetention::kQueue, thyme::ChannelReaders::kOne, thyme::ChannelOverflow::kDrop), 8> c3_;
};
struct SinkS0Ports {
  FrameChannelRef<Reading, thyme::ChannelSemanticsPack(thyme::ChannelTransportKind::kFrame, thyme::ChannelRetention::kQueue, thyme::ChannelReaders::kOne, thyme::ChannelOverflow::kDrop), 8> c0_;
  FrameChannelRef<Reading, thyme::ChannelSemanticsPack(thyme::ChannelTransportKind::kFrame, thyme::ChannelRetention::kQueue, thyme::ChannelReaders::kOne, thyme::ChannelOverflow::kDrop), 8> c2_;
};
struct SinkS1Ports {
  FrameChannelRef<Reading, thyme::ChannelSemanticsPack(thyme::ChannelTransportKind::kFrame, thyme::ChannelRetention::kQueue, thyme::ChannelReaders::kOne, thyme::ChannelOverflow::kDrop), 8> c1_;
  FrameChannelRef<Reading, thyme::ChannelSemanticsPack(thyme::ChannelTransportKind::kFrame, thyme::ChannelRetention::kQueue, thyme::ChannelReaders::kOne, thyme::ChannelOverflow::kDrop), 8> c3_;
};
