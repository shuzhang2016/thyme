#include "capability.h"

#include "thyme/contracts/abi.h"
#include "thyme/contracts/frame_header.h"

#include <string>

namespace {

std::string EdgeLabel(const Edge& edge) {
  return "edge " + edge.from + "->" + edge.to + " (" + edge.channel + ")";
}

bool Fail(const Graph& g, const Edge& edge, const std::string& message,
          std::string* error) {
  if (error != nullptr) {
    *error = GraphLoc(g, edge.line) + EdgeLabel(edge) + ": " + message;
  }
  return false;
}

}  // namespace

bool DeriveChannelCapability(const Graph& g, const TemplateCatalog& cat,
                             const Edge& edge,
                             thyme::ChannelCapabilityInfo* out,
                             std::string* error) {
  if (out == nullptr) return Fail(g, edge, "null capability output", error);
  const ContractSizeCat* size = FindContractSize(cat, edge.cpp_type);
  const bool bulk = EdgeRecordsBulk(edge);
  const uint64_t framed_bytes =
      size != nullptr ? sizeof(thyme::FrameHeader) + size->bytes : 0;
  const bool oversized = size != nullptr && framed_bytes > thyme::kAbiMaxTransportBytes;

  thyme::ChannelCapabilityInfo info;
  info.transport_kind = (bulk || oversized) ? thyme::ChannelTransportKind::kPinnedSlot
                                            : thyme::ChannelTransportKind::kFrame;
  info.retention = edge.delivery == "latest" ? thyme::ChannelRetention::kLatest
                                             : thyme::ChannelRetention::kQueue;
  info.readers = edge.delivery == "broadcast" ? thyme::ChannelReaders::kMany
                                              : thyme::ChannelReaders::kOne;
  info.recordable = edge.record;
  info.replay_gate = info.transport_kind == thyme::ChannelTransportKind::kPinnedSlot;
  info.lease_required = info.replay_gate;
  info.slot_count = info.lease_required
                        ? static_cast<uint32_t>(edge.depth > 0 ? edge.depth : g.ring_cap)
                        : 0;

  if (info.transport_kind == thyme::ChannelTransportKind::kPinnedSlot) {
    info.overflow = thyme::ChannelOverflow::kDrop;
    info.max_wire_bytes = edge.max_frame_bytes != 0
                              ? edge.max_frame_bytes
                              : (size != nullptr ? size->bytes : 0);
    if (edge.max_frame_bytes != 0 && size != nullptr &&
        edge.max_frame_bytes < size->bytes) {
      return Fail(g, edge, "max_frame_bytes is smaller than the fixed contract size",
                  error);
    }
    if (edge.lossless) {
      return Fail(g, edge,
                  "lossless delivery is incompatible with PinnedSlot lease overwrite/pinned-skip",
                  error);
    }
    if (edge.record && edge.record_kind != "h264" && edge.avg_frame_bytes == 0) {
      return Fail(g, edge,
                  "recorded PinnedSlot requires avg_frame_bytes for rate planning",
                  error);
    }
  } else {
    if (info.retention == thyme::ChannelRetention::kLatest ||
        edge.delivery == "broadcast") {
      info.overflow = thyme::ChannelOverflow::kOverwrite;
    } else {
      const Node* from = nullptr;
      const Node* to = nullptr;
      for (const auto& node : g.nodes) {
        if (node.id == edge.from) from = &node;
        if (node.id == edge.to) to = &node;
      }
      const bool local = from != nullptr && to != nullptr && from->process == to->process;
      info.overflow = local ? thyme::ChannelOverflow::kDrop
                            : thyme::ChannelOverflow::kReject;
    }
    info.max_wire_bytes = framed_bytes;
    if (edge.max_frame_bytes != 0) {
      return Fail(g, edge, "max_frame_bytes is only valid on a PinnedSlot edge", error);
    }
    if (edge.avg_frame_bytes != 0 || !edge.record_shm.empty()) {
      return Fail(g, edge,
                  "avg_frame_bytes/record_shm are only valid on a PinnedSlot edge",
                  error);
    }
  }

  *out = info;
  return true;
}
