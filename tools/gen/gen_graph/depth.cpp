#include "depth.h"

#include <cstdio>

namespace {

// Scratch buffer for one DEPTH_WARN line (snprintf-truncated).
constexpr std::size_t kWarnLineBufSize = 384;

const Node* FindNode(const Graph& g, const std::string& id) {
  for (const Node& n : g.nodes) {
    if (n.id == id) return &n;
  }
  return nullptr;
}

std::string EdgeKey(const Edge& e) {
  return e.from + "->" + e.to + ":" + e.channel;
}

}  // namespace

int NextPow2(int n) {
  if (n <= 1) return 1;
  int p = 1;
  while (p < n) p <<= 1;
  return p;
}

// Depth floor: even a 1:1 rate edge gets one spare slot so the consumer never
// waits on the producer's in-flight frame.
constexpr int kMinSuggestedDepth = 2;

int SuggestDepth(int producer_hz, int consumer_hz, int k) {
  if (producer_hz <= 0 || consumer_hz <= 0 || k <= 0) return kMinSuggestedDepth;
  const int num = k * producer_hz;
  const int raw = (num + consumer_hz - 1) / consumer_hz;
  const int clamped = raw < kMinSuggestedDepth ? kMinSuggestedDepth : raw;
  return NextPow2(clamped);
}

// Buffering horizon in consumer periods: queued cross-process delivery
// tolerates more in-flight latency than same-process notify.
constexpr int kQueueBufferPeriods = 3;
constexpr int kNotifyBufferPeriods = 2;

void WarnDepthBelowSuggest(const Graph& g, const RateInfo& info,
                           std::vector<std::string>* warnings) {
  if (warnings == nullptr) return;
  for (const Edge& e : g.edges) {
    if (e.depth <= 0) continue;
    if (e.delivery != "notify" && e.delivery != "queue") continue;
    const Node* to = FindNode(g, e.to);
    if (to == nullptr || to->hz <= 0) continue;
    const auto pit = info.edge_producer_rate.find(EdgeKey(e));
    if (pit == info.edge_producer_rate.end()) continue;
    const int k =
        e.delivery == "queue" ? kQueueBufferPeriods : kNotifyBufferPeriods;
    const int suggest = SuggestDepth(pit->second, to->hz, k);
    if (e.depth >= suggest) continue;
    char buf[kWarnLineBufSize];
    std::snprintf(buf, sizeof(buf),
                  "DEPTH_WARN edge %s->%s (%s): depth=%d suggested=%d "
                  "(producer=%d consumer_hz=%d k=%d)",
                  e.from.c_str(), e.to.c_str(), e.channel.c_str(), e.depth,
                  suggest, pit->second, to->hz, k);
    warnings->push_back(buf);
  }
}
