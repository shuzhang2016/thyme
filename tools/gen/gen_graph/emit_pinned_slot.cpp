#include "emit_internal.h"
#include "capability.h"

#include <set>

namespace gen_emit {

bool IsPinnedSlotDelivery(const Graph& g, const TemplateCatalog& cat, const Edge& e) {
  thyme::ChannelCapabilityInfo info;
  std::string error;
  return DeriveChannelCapability(g, cat, e, &info, &error) &&
         info.transport_kind == thyme::ChannelTransportKind::kPinnedSlot;
}

bool HasPinnedSlotEdge(const Graph& g, const TemplateCatalog& cat) {
  for (const auto& e : g.edges) {
    if (IsPinnedSlotDelivery(g, cat, e)) return true;
  }
  return false;
}

bool HasPinnedSlotShmEdge(const Graph& g, const TemplateCatalog& cat) {
  for (const auto& e : g.edges) {
    if (!e.shm.empty() && IsPinnedSlotDelivery(g, cat, e)) return true;
  }
  return false;
}

std::string PinnedSlotSemantics(const Graph& g, const TemplateCatalog& cat,
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
  return "thyme::ChannelSemanticsPack(thyme::ChannelTransportKind::kPinnedSlot, " +
         std::string(retention) + ", " + readers + ", thyme::ChannelOverflow::kDrop)";
}

std::string PinnedSlotRefType(const Graph& g, const TemplateCatalog& cat,
                            const Edge& e) {
  return "PinnedSlotRef<" + e.cpp_type + ", " + PinnedSlotSemantics(g, cat, e) +
         ", " + std::to_string(EventEdgeCap(g, e)) + ">";
}

std::string PinnedSlotLocalChannelType(const Graph& g, const TemplateCatalog& cat,
                                     const Edge& e) {
  return "thyme::Channel<" + e.cpp_type + ", " + PinnedSlotSemantics(g, cat, e) +
         ", thyme::ChannelBackend::kLocal, " + std::to_string(EventEdgeCap(g, e)) +
         ", " + std::to_string(EventEdgeCap(g, e)) + ">";
}

std::string PinnedSlotShmChannelType(const Graph& g, const TemplateCatalog& cat,
                                   const Edge& e) {
  return "thyme::Channel<" + e.cpp_type + ", " + PinnedSlotSemantics(g, cat, e) +
         ", thyme::ChannelBackend::kShm, " + std::to_string(EventEdgeCap(g, e)) +
         ", " + std::to_string(EventEdgeCap(g, e)) + ">";
}

std::string PinnedSlotWriterMember(const Edge& e) { return e.channel + "_w_"; }
std::string PinnedSlotReaderMember(const Edge& e) {
  return e.to + "_" + e.channel + "_r_";
}
std::string PinnedSlotStoreMember(const Edge& e, bool as_out) {
  return (as_out ? e.from : e.to) + "_" + e.channel + "_ch_";
}

void AppendPinnedSlotRefDefinition(std::ostringstream& os) {
  os << "template <typename T, uint32_t Semantics, std::size_t Cap>\n";
  os << "struct PinnedSlotRef\n";
  os << "    : public DualChannelRef<\n";
  os << "          thyme::Channel<T, Semantics, thyme::ChannelBackend::kLocal, Cap, Cap>,\n";
  os << "          thyme::Channel<T, Semantics, thyme::ChannelBackend::kShm, Cap, Cap>> {\n";
  os << "  using Local = thyme::Channel<T, Semantics, thyme::ChannelBackend::kLocal, Cap, Cap>;\n";
  os << "  using Shm = thyme::Channel<T, Semantics, thyme::ChannelBackend::kShm, Cap, Cap>;\n";
  os << "  using Base = DualChannelRef<Local, Shm>;\n";
  os << "  PinnedSlotRef() = default;\n";
  os << "  PinnedSlotRef(Local* channel) { this->local_p_ = channel; }\n";
  os << "  PinnedSlotRef(Shm* channel) { this->shm_p_ = channel; }\n";
  os << "};\n\n";
}

void AppendSpPinnedSlotFields(std::ostringstream& os, const Graph& g,
                            const TemplateCatalog& cat) {
  if (!HasPinnedSlotEdge(g, cat)) return;
  os << "  struct SpPinnedSlotTransports {\n";
  std::set<std::string> producers;
  for (const auto& e : g.edges) {
    if (!IsPinnedSlotDelivery(g, cat, e)) continue;
    const std::string channel = PinnedSlotLocalChannelType(g, cat, e);
    if (producers.insert(e.channel).second) {
      if (e.delivery == "latest") {
        os << "    " << channel << "::PinnedLatest " << e.channel << "_store_;\n";
      } else {
        os << "    " << channel << "::PinnedRing " << e.channel << "_store_;\n";
      }
      os << "    " << channel << " " << PinnedSlotStoreMember(e, true) << ";\n";
    }
    os << "    " << channel << " " << PinnedSlotStoreMember(e, false) << ";\n";
    if (e.delivery != "latest") {
      os << "    " << channel << "::PinnedRing::Reader " << PinnedSlotReaderMember(e)
         << ";\n";
    }
  }
  os << "  };\n  SpPinnedSlotTransports pinned_slot_transports_;\n\n";
}

void AppendSpPinnedSlotCreate(std::ostringstream& os, const Graph& g,
                            const TemplateCatalog& cat) {
  std::set<std::string> producers;
  for (const auto& e : g.edges) {
    if (!IsPinnedSlotDelivery(g, cat, e)) continue;
    if (producers.insert(e.channel).second) {
      os << "  pipeline->pinned_slot_transports_." << PinnedSlotStoreMember(e, true)
         << ".Bind(&pipeline->pinned_slot_transports_." << e.channel << "_store_);\n";
    }
    os << "  pipeline->pinned_slot_transports_." << PinnedSlotStoreMember(e, false)
       << ".Bind(&pipeline->pinned_slot_transports_." << e.channel << "_store_);\n";
    if (e.delivery != "latest") {
      os << "  if (!pipeline->pinned_slot_transports_." << PinnedSlotReaderMember(e)
         << ".Bind(&pipeline->pinned_slot_transports_." << e.channel
         << "_store_)) return nullptr;\n";
      os << "  pipeline->pinned_slot_transports_." << PinnedSlotStoreMember(e, false)
         << ".BindReader(&pipeline->pinned_slot_transports_." << PinnedSlotReaderMember(e)
         << ");\n";
    }
  }
}

void AppendPinnedSlotShmHelpers(std::ostringstream& os, const Graph& g,
                              const TemplateCatalog& cat) {
  if (!HasPinnedSlotShmEdge(g, cat)) return;
  os << "\nstruct EventTransports;\n";
  os << "struct PinnedSlotTransports {\n";
  std::set<std::string> regions;
  for (const auto& e : g.edges) {
    if (e.shm.empty() || !IsPinnedSlotDelivery(g, cat, e)) continue;
    if (regions.insert(e.shm).second) {
      os << "  thyme::PinnedSlotWriter " << PinnedSlotWriterMember(e) << ";\n";
      os << "  " << PinnedSlotShmChannelType(g, cat, e) << " "
         << PinnedSlotStoreMember(e, true) << ";\n";
    }
    os << "  thyme::PinnedSlotReader " << PinnedSlotReaderMember(e) << ";\n";
    os << "  " << PinnedSlotShmChannelType(g, cat, e) << " "
       << PinnedSlotStoreMember(e, false) << ";\n";
  }
  os << "};\n\n";

  os << "inline void UnlinkPinnedSlotShmChannels(const char* prefix) {\n";
  os << "  char nm[64];\n  char lease_nm[72];\n";
  regions.clear();
  for (const auto& e : g.edges) {
    if (e.shm.empty() || !IsPinnedSlotDelivery(g, cat, e)) continue;
    if (!regions.insert(e.shm).second) continue;
    os << "  thyme::app::MpName(nm, sizeof(nm), prefix, \"" << e.shm << "\");\n";
    os << "  std::snprintf(lease_nm, sizeof(lease_nm), \"%s.lease\", nm);\n";
    os << "  thyme::ShmRegion::UnlinkName(nm);\n";
    os << "  thyme::ShmRegion::UnlinkName(lease_nm);\n";
  }
  os << "}\n\n";

  os << "inline bool PreparePinnedSlotShmChannels(const char* prefix) {\n";
  os << "  char nm[64];\n";
  regions.clear();
  for (const auto& e : g.edges) {
    if (e.shm.empty() || !IsPinnedSlotDelivery(g, cat, e)) continue;
    if (!regions.insert(e.shm).second) continue;
    os << "  thyme::app::MpName(nm, sizeof(nm), prefix, \"" << e.shm << "\");\n";
    os << "  {\n    thyme::PinnedSlotWriter writer;\n";
    os << "    if (!writer.Create(nm, "
       << (e.max_frame_bytes != 0 ? std::to_string(e.max_frame_bytes) + "u"
                                  : "sizeof(" + e.cpp_type + ")")
       << ", " << EventEdgeCap(g, e) << ", 0)) return false;\n";
    os << "  }\n";
  }
  os << "  return true;\n}\n\n";

  os << "inline bool OpenPinnedSlotShmChannels(PinnedSlotTransports& l, const char* prefix,\n"
        "                                      const char* role, uint64_t timeout_ns,\n"
        "                                      const EventTransports* ev = nullptr) {\n";
  os << "  char nm[64];\n";
  regions.clear();
  for (const auto& e : g.edges) {
    if (e.shm.empty() || !IsPinnedSlotDelivery(g, cat, e)) continue;
    const Node* from = FindNodeById(g, e.from);
    const Node* to = FindNodeById(g, e.to);
    const std::string from_process = from != nullptr ? from->process : "";
    const std::string to_process = to != nullptr ? to->process : "";
    os << "  thyme::app::MpName(nm, sizeof(nm), prefix, \"" << e.shm << "\");\n";
    if (regions.insert(e.shm).second) {
      os << "  if (std::strcmp(role, \"" << from_process << "\") == 0) {\n";
      os << "    if (!l." << PinnedSlotWriterMember(e)
         << ".Open(nm, timeout_ns)) return false;\n";
      os << "    if (ev != nullptr && !l." << PinnedSlotWriterMember(e)
         << ".AddWakeWord(ev->" << e.to << "_wake)) return false;\n";
      os << "    l." << PinnedSlotStoreMember(e, true) << ".Bind(&l."
         << PinnedSlotWriterMember(e) << ");\n";
      os << "  }\n";
    }
    os << "  if (std::strcmp(role, \"" << to_process << "\") == 0) {\n";
    os << "    if (!l." << PinnedSlotReaderMember(e)
       << ".Open(nm, timeout_ns)) return false;\n";
    os << "    if (ev != nullptr) l." << PinnedSlotReaderMember(e)
       << ".SetWakeWord(ev->" << e.to << "_wake);\n";
    os << "    l." << PinnedSlotStoreMember(e, false) << ".Bind(&l."
       << PinnedSlotReaderMember(e) << ");\n";
    os << "  }\n";
  }
  os << "  return true;\n}\n";
}

}  // namespace gen_emit
