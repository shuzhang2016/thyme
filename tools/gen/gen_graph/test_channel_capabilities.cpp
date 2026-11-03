#include "capability.h"

#include <cstdio>
#include <cstdlib>
#include <string>

#define CHECK(c) do { if (!(c)) { \
  std::fprintf(stderr, "CHECK failed at line %d: %s\n", __LINE__, #c); \
  std::abort(); \
} } while (0)

namespace {

Graph MakeGraph() {
  Graph g;
  g.ring_cap = 8;
  Node producer;
  producer.id = "producer";
  producer.process = "p1";
  Node consumer;
  consumer.id = "consumer";
  consumer.process = "p2";
  consumer.exec = "event";
  g.nodes = {producer, consumer};
  return g;
}

Edge MakeEdge() {
  Edge e;
  e.from = "producer";
  e.to = "consumer";
  e.channel = "samples";
  e.cpp_type = "Sample";
  e.delivery = "queue";
  e.record = true;
  return e;
}

}  // namespace

int main() {
  const Graph g = MakeGraph();
  TemplateCatalog cat;
  cat.contract_sizes.push_back(ContractSizeCat{"Sample", 128});
  Edge edge = MakeEdge();
  thyme::ChannelCapabilityInfo info;
  std::string error;

  CHECK(DeriveChannelCapability(g, cat, edge, &info, &error));
  CHECK(info.transport_kind == thyme::ChannelTransportKind::kFrame);
  CHECK(info.retention == thyme::ChannelRetention::kQueue);
  CHECK(info.readers == thyme::ChannelReaders::kOne);
  CHECK(info.overflow == thyme::ChannelOverflow::kReject);
  CHECK(info.recordable && !info.replay_gate && !info.lease_required);
  CHECK(info.max_wire_bytes == 168);

  edge.delivery = "broadcast";
  CHECK(DeriveChannelCapability(g, cat, edge, &info, &error));
  CHECK(info.retention == thyme::ChannelRetention::kQueue);
  CHECK(info.readers == thyme::ChannelReaders::kMany);
  CHECK(info.overflow == thyme::ChannelOverflow::kOverwrite);

  cat.contract_sizes[0].bytes = 70000;
  edge.avg_frame_bytes = 60000;
  CHECK(DeriveChannelCapability(g, cat, edge, &info, &error));
  CHECK(info.transport_kind == thyme::ChannelTransportKind::kPinnedSlot);
  CHECK(info.overflow == thyme::ChannelOverflow::kDrop);
  CHECK(info.replay_gate && info.lease_required);
  CHECK(info.max_wire_bytes == 70000);
  CHECK(info.slot_count == 8);

  edge.lossless = true;
  CHECK(!DeriveChannelCapability(g, cat, edge, &info, &error));
  CHECK(error.find("incompatible with PinnedSlot") != std::string::npos);
  edge.lossless = false;

  edge.max_frame_bytes = 1024;
  CHECK(!DeriveChannelCapability(g, cat, edge, &info, &error));
  CHECK(error.find("smaller than the fixed contract") != std::string::npos);
  edge.max_frame_bytes = 80000;
  CHECK(DeriveChannelCapability(g, cat, edge, &info, &error));
  CHECK(info.max_wire_bytes == 80000);

  edge.avg_frame_bytes = 0;
  CHECK(!DeriveChannelCapability(g, cat, edge, &info, &error));
  CHECK(error.find("requires avg_frame_bytes") != std::string::npos);

  cat.contract_sizes[0].bytes = 128;
  edge.avg_frame_bytes = 0;
  edge.max_frame_bytes = 4096;
  CHECK(!DeriveChannelCapability(g, cat, edge, &info, &error));
  CHECK(error.find("only valid on a PinnedSlot") != std::string::npos);

  edge.max_frame_bytes = 0;
  CHECK(DeriveChannelCapability(g, cat, edge, &info, &error));
  CHECK(info.transport_kind == thyme::ChannelTransportKind::kFrame);
  CHECK(info.max_wire_bytes == 168);

  std::printf("test_channel_capabilities ok\n");
  return 0;
}
