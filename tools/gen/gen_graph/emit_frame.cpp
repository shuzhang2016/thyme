#include "emit_internal.h"
#include "capability.h"

namespace gen_emit {

std::string FrameSemantics(const Graph& g, const TemplateCatalog& cat,
                           const Edge& e) {
  thyme::ChannelCapabilityInfo info;
  std::string error;
  if (!DeriveChannelCapability(g, cat, e, &info, &error)) return {};
  const char* retention = info.retention == thyme::ChannelRetention::kQueue
                              ? "thyme::ChannelRetention::kQueue"
                              : "thyme::ChannelRetention::kLatest";
  const char* readers = info.readers == thyme::ChannelReaders::kMany
                            ? "thyme::ChannelReaders::kMany"
                            : "thyme::ChannelReaders::kOne";
  const char* overflow = "thyme::ChannelOverflow::kOverwrite";
  if (info.overflow == thyme::ChannelOverflow::kDrop) {
    overflow = "thyme::ChannelOverflow::kDrop";
  } else if (info.overflow == thyme::ChannelOverflow::kReject) {
    overflow = "thyme::ChannelOverflow::kReject";
  }
  return "thyme::ChannelSemanticsPack(thyme::ChannelTransportKind::kFrame, " +
         std::string(retention) + ", " + readers + ", " + overflow + ")";
}

std::string FrameRefType(const Graph& g, const TemplateCatalog& cat,
                         const Edge& e) {
  return "FrameChannelRef<" + e.cpp_type + ", " + FrameSemantics(g, cat, e) +
         ", " + std::to_string(EventEdgeCap(g, e)) + ">";
}

// One dispatch body for both generated refs. SP and MP still need two channel
// backends in the same Ports struct (that is why a ref exists at all), but the
// null-checked forwarding is written once instead of once per ref.
void AppendDualChannelRefDefinition(std::ostringstream& os) {
  os << "// SHM queue edges report producer backpressure instead of dropping.\n";
  os << "inline constexpr uint32_t kShmSemanticsOf(uint32_t semantics) {\n";
  os << "  const bool queue = ((semantics >> 8) & 0xFFu) ==\n";
  os << "      static_cast<uint32_t>(thyme::ChannelRetention::kQueue);\n";
  os << "  const bool one = ((semantics >> 16) & 0xFFu) ==\n";
  os << "      static_cast<uint32_t>(thyme::ChannelReaders::kOne);\n";
  os << "  const bool drop = ((semantics >> 24) & 0xFFu) ==\n";
  os << "      static_cast<uint32_t>(thyme::ChannelOverflow::kDrop);\n";
  os << "  return (queue && one && drop)\n";
  os << "             ? ((semantics & 0x00FFFFFFu) |\n";
  os << "                (static_cast<uint32_t>(thyme::ChannelOverflow::kReject) << 24))\n";
  os << "             : semantics;\n";
  os << "}\n\n";
  os << "template <typename Local, typename Shm>\n";
  os << "struct DualChannelRef : public thyme::Waitable {\n";
  os << "  using Write = typename Local::Write;\n";
  os << "  using Sample = typename Local::Sample;\n";
  os << "  Local* local_p_ = nullptr;\n";
  os << "  Shm* shm_p_ = nullptr;\n";
  os << "  static Write Unbound() {\n";
  os << "    Write out;\n";
  os << "    out.status = thyme::ChannelStatus::kUnbound;\n";
  os << "    return out;\n  }\n";
  os << "  Write BeginWrite() {\n";
  os << "    if (local_p_ != nullptr) return local_p_->BeginWrite();\n";
  os << "    if (shm_p_ != nullptr) return shm_p_->BeginWrite();\n";
  os << "    return Unbound();\n  }\n";
  os << "  Write BeginWriteAt(uint32_t size, uint64_t mono_ns) {\n";
  os << "    if (local_p_ != nullptr) return local_p_->BeginWriteAt(size, mono_ns);\n";
  os << "    if (shm_p_ != nullptr) return shm_p_->BeginWriteAt(size, mono_ns);\n";
  os << "    return Unbound();\n  }\n";
  os << "  thyme::ChannelStatus CommitWrite(Write* out) {\n";
  os << "    if (local_p_ != nullptr) return local_p_->CommitWrite(out);\n";
  os << "    if (shm_p_ != nullptr) return shm_p_->CommitWrite(out);\n";
  os << "    return thyme::ChannelStatus::kUnbound;\n  }\n";
  os << "  void AbortWrite(Write* out) {\n";
  os << "    if (local_p_ != nullptr) { local_p_->AbortWrite(out); return; }\n";
  os << "    if (shm_p_ != nullptr) shm_p_->AbortWrite(out);\n  }\n";
  os << "  thyme::ChannelStatus TryRead(Sample* out) {\n";
  os << "    if (local_p_ != nullptr) return local_p_->TryRead(out);\n";
  os << "    if (shm_p_ != nullptr) return shm_p_->TryRead(out);\n";
  os << "    return thyme::ChannelStatus::kUnbound;\n  }\n";
  os << "  bool empty() const override {\n";
  os << "    if (local_p_ != nullptr) return local_p_->empty();\n";
  os << "    if (shm_p_ != nullptr) return shm_p_->empty();\n";
  os << "    return true;\n  }\n";
  os << "  uint64_t drop_count() const {\n";
  os << "    return shm_p_ != nullptr ? shm_p_->drop_count() : 0;\n  }\n";
  os << "  bool drop_count_observed() const {\n";
  os << "    return shm_p_ != nullptr && shm_p_->drop_count_observed();\n  }\n";
  os << "  bool push_fail_count_observed() const {\n";
  os << "    return shm_p_ != nullptr && shm_p_->push_fail_count_observed();\n  }\n";
  os << "  uint32_t push_fail_count() const {\n";
  os << "    return shm_p_ != nullptr ? shm_p_->push_fail_count() : 0;\n  }\n";
  os << "  void Notify() override {}\n";
  os << "  void SetNotify(thyme::Waitable* target) override {\n";
  os << "    if (local_p_ != nullptr) { local_p_->SetNotify(target); return; }\n";
  os << "    if (shm_p_ != nullptr) shm_p_->SetNotify(target);\n  }\n";
  os << "  void WaitUntil(uint64_t deadline_ns) override {\n";
  os << "    if (local_p_ != nullptr) { local_p_->WaitUntil(deadline_ns); return; }\n";
  os << "    if (shm_p_ != nullptr) shm_p_->WaitUntil(deadline_ns);\n  }\n";
  os << "  std::atomic<uint32_t>* NotifyWakeWord() const override {\n";
  os << "    if (local_p_ != nullptr) return local_p_->NotifyWakeWord();\n";
  os << "    if (shm_p_ != nullptr) return shm_p_->NotifyWakeWord();\n";
  os << "    return nullptr;\n  }\n";
  os << "  bool uses_local() const { return local_p_ != nullptr; }\n";
  os << "};\n\n";
}

void AppendFrameRefDefinition(std::ostringstream& os) {
  os << "template <typename T, uint32_t Semantics, std::size_t Cap>\n";
  os << "struct FrameChannelRef\n";
  os << "    : public DualChannelRef<\n";
  os << "          thyme::Channel<T, Semantics, thyme::ChannelBackend::kLocal, Cap, Cap>,\n";
  os << "          thyme::Channel<T, kShmSemanticsOf(Semantics),\n";
  os << "                         thyme::ChannelBackend::kShm, Cap, Cap>> {\n";
  os << "  using Local = thyme::Channel<T, Semantics, thyme::ChannelBackend::kLocal, Cap, Cap>;\n";
  os << "  using Shm = thyme::Channel<T, kShmSemanticsOf(Semantics),\n";
  os << "                              thyme::ChannelBackend::kShm, Cap, Cap>;\n";
  os << "  using Base = DualChannelRef<Local, Shm>;\n";
  os << "  using Write = typename Base::Write;\n";
  os << "  using Sample = typename Base::Sample;\n";
  os << "  Local local_;\n  Shm shm_;\n";
  os << "  FrameChannelRef() = default;\n";
  os << "  // Copies re-point at their own channel storage: Ports are copied into\n";
  os << "  // each node, and a shared Channel would share its sequence/reader\n";
  os << "  // cursor across threads.\n";
  os << "  FrameChannelRef(const FrameChannelRef& other)\n";
  os << "      : local_(other.local_), shm_(other.shm_) {\n";
  os << "    this->local_p_ = other.local_p_ != nullptr ? &local_ : nullptr;\n";
  os << "    this->shm_p_ = other.shm_p_ != nullptr ? &shm_ : nullptr;\n";
  os << "  }\n";
  os << "  FrameChannelRef& operator=(const FrameChannelRef& other) {\n";
  os << "    local_ = other.local_;\n";
  os << "    shm_ = other.shm_;\n";
  os << "    this->local_p_ = other.local_p_ != nullptr ? &local_ : nullptr;\n";
  os << "    this->shm_p_ = other.shm_p_ != nullptr ? &shm_ : nullptr;\n";
  os << "    return *this;\n";
  os << "  }\n";
  os << "  FrameChannelRef(thyme::LatestSlot<thyme::Frame<T>>* endpoint) { Bind(endpoint); }\n";
  os << "  FrameChannelRef(thyme::NotifPipe<thyme::Frame<T>, Cap>* endpoint) { Bind(endpoint); }\n";
  os << "  FrameChannelRef(thyme::ShmSpscRing<thyme::Frame<T>, Cap>* endpoint) { Bind(endpoint); }\n";
  os << "  FrameChannelRef(thyme::ShmSpmcRing<thyme::Frame<T>, Cap>* endpoint) { Bind(endpoint); }\n";
  os << "  FrameChannelRef(thyme::SpmcReader<thyme::Frame<T>, Cap>* endpoint) { BindReader(endpoint); }\n";
  os << "  void Bind(thyme::LatestSlot<thyme::Frame<T>>* endpoint) {\n";
  os << "    local_.Bind(endpoint); this->local_p_ = &local_;\n  }\n";
  os << "  void Bind(thyme::NotifPipe<thyme::Frame<T>, Cap>* endpoint) {\n";
  os << "    local_.Bind(endpoint); this->local_p_ = &local_;\n  }\n";
  os << "  void Bind(thyme::ShmSpscRing<thyme::Frame<T>, Cap>* endpoint) {\n";
  os << "    shm_.Bind(endpoint); this->shm_p_ = &shm_;\n  }\n";
  os << "  void Bind(thyme::ShmSpmcRing<thyme::Frame<T>, Cap>* endpoint) {\n";
  os << "    shm_.Bind(endpoint); this->shm_p_ = &shm_;\n  }\n";
  os << "  void BindReader(thyme::SpmcReader<thyme::Frame<T>, Cap>* endpoint) {\n";
  os << "    shm_.BindReader(endpoint); this->shm_p_ = &shm_;\n  }\n";
  os << "  bool uses_notif_pipe() const { return this->local_p_ != nullptr; }\n";
  os << "};\n\n";
}

}  // namespace gen_emit
