#include "thyme/runtime/channel.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <type_traits>

#define CHECK(c) do { if (!(c)) { \
  std::fprintf(stderr, "CHECK failed at line %d: %s\n", __LINE__, #c); \
  std::abort(); \
} } while (0)

namespace {

constexpr uint32_t kMagic = 0x54455354u;
constexpr uint16_t kVersion = 1;
constexpr uint32_t kQueueSemantics = thyme::ChannelSemanticsPack(
    thyme::ChannelTransportKind::kFrame, thyme::ChannelRetention::kQueue,
    thyme::ChannelReaders::kOne, thyme::ChannelOverflow::kDrop);
constexpr uint32_t kLatestBroadcastSemantics = thyme::ChannelSemanticsPack(
    thyme::ChannelTransportKind::kFrame, thyme::ChannelRetention::kLatest,
    thyme::ChannelReaders::kMany, thyme::ChannelOverflow::kOverwrite);
constexpr uint32_t kQueueBroadcastSemantics = thyme::ChannelSemanticsPack(
    thyme::ChannelTransportKind::kFrame, thyme::ChannelRetention::kQueue,
    thyme::ChannelReaders::kMany, thyme::ChannelOverflow::kOverwrite);
constexpr uint32_t kLatestBigSemantics = thyme::ChannelSemanticsPack(
    thyme::ChannelTransportKind::kPinnedSlot, thyme::ChannelRetention::kLatest,
    thyme::ChannelReaders::kOne, thyme::ChannelOverflow::kDrop);
constexpr uint32_t kQueueBigSemantics = thyme::ChannelSemanticsPack(
    thyme::ChannelTransportKind::kPinnedSlot, thyme::ChannelRetention::kQueue,
    thyme::ChannelReaders::kMany, thyme::ChannelOverflow::kDrop);

struct Small {
  uint32_t value;
  uint32_t mirror;
};

struct Large {
  uint64_t sequence;
  uint8_t bytes[70000 - sizeof(uint64_t)];
};

}  // namespace

int main() {
  using SmallChannel = thyme::Channel<Small, kQueueSemantics, thyme::ChannelBackend::kLocal>;
  static_assert(!std::is_polymorphic<SmallChannel>::value, "no vtable");
  static_assert(std::is_same<SmallChannel::Slot, thyme::Frame<Small>>::value,
                "framed channel slot is Frame<T>");
  thyme::NotifPipe<thyme::Frame<Small>, 4> pipe;
  SmallChannel small({kMagic, kVersion, 7});
  small.Bind(&pipe);
  auto write = small.BeginWrite();
  CHECK(write.status == thyme::ChannelStatus::kOk);
  CHECK(write.frame_ptr() != nullptr);
  CHECK(&write.frame_ptr()->payload == write.payload);
  write.payload->value = 42;
  write.payload->mirror = 42;
  CHECK(small.CommitWrite(&write) == thyme::ChannelStatus::kOk);
  CHECK(write.frame_ptr() != nullptr && write.frame_ptr()->payload.value == 42);
  SmallChannel::Sample sample;
  CHECK(small.TryRead(&sample) == thyme::ChannelStatus::kOk);
  CHECK(sample.payload->value == 42 && sample.payload->mirror == 42);
  CHECK(sample.mono_ns != 0 && sample.sequence == 1);
  Small copied{};
  CHECK(sample.CopyTo(&copied));
  CHECK(copied.value == 42 && copied.mirror == 42);
  // A moved copy-mode sample must re-point at its own storage, not the source.
  SmallChannel::Sample moved(std::move(sample));
  CHECK(moved.payload != nullptr && moved.payload->value == 42);
  CHECK(moved.CopyTo(&copied) && copied.value == 42);
  CHECK(!sample.still_valid());
  moved.Reset();

  using LatestChannel =
      thyme::Channel<Large, kLatestBigSemantics, thyme::ChannelBackend::kLocal>;
  static_assert(sizeof(LatestChannel::Sample) < sizeof(Large) / 16,
                "lease samples must not contain a full payload copy");
  static_assert(sizeof(LatestChannel::Write) < sizeof(Large) / 16,
                "lease writes must not clear a full default payload");
  LatestChannel::PinnedLatest store;
  LatestChannel latest;
  latest.Bind(&store);
  auto large_write = latest.BeginWrite();
  CHECK(large_write.status == thyme::ChannelStatus::kOk);
  large_write.payload->sequence = 9;
  std::memset(large_write.payload->bytes, 0x5a, sizeof(large_write.payload->bytes));
  CHECK(latest.CommitWrite(&large_write) == thyme::ChannelStatus::kOk);
  LatestChannel::Sample latest_sample;
  CHECK(latest.TryRead(&latest_sample) == thyme::ChannelStatus::kOk);
  CHECK(reinterpret_cast<const Large*>(latest_sample.data)->sequence == 9);
  CHECK(latest_sample.sequence == 1 && latest_sample.mono_ns != 0);
  CHECK(latest_sample.still_valid());
  latest_sample.Reset();

  using RingChannel =
      thyme::Channel<Large, kQueueBigSemantics, thyme::ChannelBackend::kLocal>;
  RingChannel::PinnedRing ring;
  RingChannel::PinnedRing::Reader reader;
  CHECK(reader.Bind(&ring));
  RingChannel queued;
  queued.Bind(&ring);
  queued.BindReader(&reader);
  for (uint64_t i = 1; i <= 2; ++i) {
    auto item = queued.BeginWrite();
    CHECK(item.status == thyme::ChannelStatus::kOk);
    item.payload->sequence = i;
    std::memset(item.payload->bytes, static_cast<int>(i), sizeof(item.payload->bytes));
    CHECK(queued.CommitWrite(&item) == thyme::ChannelStatus::kOk);
  }
  RingChannel::Sample queued_sample;
  CHECK(queued.TryRead(&queued_sample) == thyme::ChannelStatus::kOk);
  CHECK(reinterpret_cast<const Large*>(queued_sample.data)->sequence == 1);
  queued_sample.Reset();
  CHECK(queued.TryRead(&queued_sample) == thyme::ChannelStatus::kOk);
  CHECK(reinterpret_cast<const Large*>(queued_sample.data)->sequence == 2);
  queued_sample.Reset();

  using LatestBroadcast =
      thyme::Channel<Small, kLatestBroadcastSemantics, thyme::ChannelBackend::kShm>;
  using QueueBroadcast =
      thyme::Channel<Small, kQueueBroadcastSemantics, thyme::ChannelBackend::kShm>;
  const char* broadcast_name = "/thyme_channel_broadcast_test";
  thyme::ShmRegion::UnlinkName(broadcast_name);
  thyme::ShmSpmcRing<thyme::Frame<Small>, 4> broadcast_ring;
  thyme::SpmcReader<thyme::Frame<Small>, 4> latest_endpoint;
  thyme::SpmcReader<thyme::Frame<Small>, 4> follow_endpoint;
  CHECK(broadcast_ring.Create(broadcast_name));
  CHECK(latest_endpoint.Bind(&broadcast_ring, thyme::SpmcRead::Latest));
  CHECK(follow_endpoint.Bind(&broadcast_ring, thyme::SpmcRead::Follow));
  LatestBroadcast broadcast({kMagic, kVersion, 7});
  LatestBroadcast latest_reader({kMagic, kVersion, 7});
  QueueBroadcast follow_reader({kMagic, kVersion, 7});
  broadcast.Bind(&broadcast_ring);
  latest_reader.BindReader(&latest_endpoint);
  follow_reader.BindReader(&follow_endpoint);
  auto publish_small = [&](uint32_t value) {
    auto out = broadcast.BeginWrite();
    CHECK(out.status == thyme::ChannelStatus::kOk);
    out.payload->value = value;
    out.payload->mirror = value;
    CHECK(broadcast.CommitWrite(&out) == thyme::ChannelStatus::kOk);
  };
  publish_small(43);
  publish_small(44);
  LatestBroadcast::Sample latest_broadcast_sample;
  CHECK(latest_reader.TryRead(&latest_broadcast_sample) == thyme::ChannelStatus::kOk);
  CHECK(latest_broadcast_sample.payload->value == 44);
  CHECK(latest_reader.empty());
  CHECK(follow_reader.drop_count_observed());
  CHECK(!follow_reader.push_fail_count_observed());
  QueueBroadcast::Sample follow_sample;
  CHECK(follow_reader.TryRead(&follow_sample) == thyme::ChannelStatus::kOk);
  CHECK(follow_sample.payload->value == 43);
  follow_sample.Reset();
  CHECK(follow_reader.TryRead(&follow_sample) == thyme::ChannelStatus::kOk);
  CHECK(follow_sample.payload->value == 44);
  follow_sample.Reset();
  auto aborted = broadcast.BeginWrite();
  CHECK(aborted.status == thyme::ChannelStatus::kOk);
  aborted.payload->value = 45;
  broadcast.AbortWrite(&aborted);
  publish_small(46);
  CHECK(latest_reader.TryRead(&latest_broadcast_sample) == thyme::ChannelStatus::kOk);
  CHECK(latest_broadcast_sample.payload->value == 46);
  CHECK(follow_reader.TryRead(&follow_sample) == thyme::ChannelStatus::kOk);
  CHECK(follow_sample.payload->value == 46);
  follow_sample.Reset();
  CHECK(follow_reader.empty());
  broadcast_ring.Close();
  thyme::ShmRegion::UnlinkName(broadcast_name);

  using BigChannel =
      thyme::Channel<Large, kQueueBigSemantics, thyme::ChannelBackend::kShm>;
  const char* name = "/thyme_channel_facade_test";
  thyme::ShmRegion::UnlinkName(name);
  thyme::ShmRegion::UnlinkName((std::string(name) + ".lease").c_str());
  thyme::PinnedSlotWriter writer;
  thyme::PinnedSlotReader big_reader;
  CHECK(writer.Create(name, sizeof(Large), 4, 0));
  CHECK(big_reader.Open(name, 1000000000ULL));
  BigChannel big;
  big.Bind(&writer);
  big.Bind(&big_reader);
  auto big_write = big.BeginWrite();
  CHECK(big_write.status == thyme::ChannelStatus::kOk);
  big_write.payload->sequence = 11;
  std::memset(big_write.payload->bytes, 0x33, sizeof(big_write.payload->bytes));
  CHECK(big.CommitWrite(&big_write) == thyme::ChannelStatus::kOk);
  BigChannel::Sample big_sample;
  CHECK(big.TryRead(&big_sample) == thyme::ChannelStatus::kOk);
  CHECK(reinterpret_cast<const Large*>(big_sample.data)->sequence == 11);
  CHECK(reinterpret_cast<const Large*>(big_sample.data)->bytes[0] == 0x33);
  CHECK(big_sample.still_valid());
  Large big_copy{};
  CHECK(big_sample.CopyTo(&big_copy));
  CHECK(big_copy.sequence == 11 && big_copy.bytes[0] == 0x33);
  big_sample.Reset();
  auto variable = big.BeginWriteAt(123, 123456);
  CHECK(variable.status == thyme::ChannelStatus::kOk);
  CHECK(variable.sequence == 2 && variable.mono_ns == 123456);
  CHECK(variable.payload == nullptr && variable.data != nullptr && variable.size == 123);
  std::memset(variable.data, 0x7b, variable.size);
  CHECK(big.CommitWrite(&variable) == thyme::ChannelStatus::kOk);
  CHECK(big.TryRead(&big_sample) == thyme::ChannelStatus::kOk);
  CHECK(big_sample.data != nullptr);
  CHECK(big_sample.size == 123 && big_sample.mono_ns == 123456);
  CHECK(big_sample.data[0] == 0x7b && big_sample.data[122] == 0x7b);
  CHECK(!big_sample.CopyTo(&big_copy));
  big_sample.Reset();
  big_reader.Close();
  writer.Close();
  thyme::ShmRegion::UnlinkName(name);
  thyme::ShmRegion::UnlinkName((std::string(name) + ".lease").c_str());

  BigChannel unbound;
  auto invalid = unbound.BeginWrite();
  CHECK(invalid.status == thyme::ChannelStatus::kUnbound);
  CHECK(!invalid.bound());
  CHECK(invalid.payload == nullptr && invalid.data == nullptr);
  CHECK(invalid.frame_ptr() == nullptr);
  CHECK(invalid.sequence == 0 && invalid.mono_ns == 0);
  CHECK(!unbound.wired());
  CHECK(!unbound.reader_wired());
  CHECK(invalid.Commit() == thyme::ChannelStatus::kUnbound);
  CHECK(unbound.CommitWrite(&invalid) == thyme::ChannelStatus::kInvalid);
  BigChannel::Sample unbound_sample;
  CHECK(unbound.TryRead(&unbound_sample) == thyme::ChannelStatus::kUnbound);

  using RawChannel = thyme::Channel<Small, kQueueBroadcastSemantics,
                                    thyme::ChannelBackend::kShm, 4, 4, 8,
                                    thyme::ChannelWireFormat::kRaw>;
  static_assert(std::is_same<RawChannel::Slot, Small>::value,
                "raw channel slot is the bare payload, not Frame<T>");
  static_assert(sizeof(RawChannel) == sizeof(SmallChannel),
                "raw and framed channels share one backend pointer set");
  const char* raw_name = "/thyme_channel_raw_test";
  thyme::ShmRegion::UnlinkName(raw_name);
  thyme::ShmSpmcRing<Small, 4> raw_ring;
  thyme::SpmcReader<Small, 4> raw_endpoint;
  CHECK(raw_ring.Create(raw_name));
  CHECK(raw_endpoint.Bind(&raw_ring, thyme::SpmcRead::Follow));
  RawChannel raw;
  raw.Bind(&raw_ring);
  raw.BindReader(&raw_endpoint);
  auto raw_write = raw.BeginWriteAt(sizeof(Small), 777);
  CHECK(raw_write.status == thyme::ChannelStatus::kOk);
  CHECK(raw_write.frame_ptr() == nullptr);
  CHECK(raw_write.sequence == 1 && raw_write.mono_ns == 777);
  raw_write.payload->value = 91;
  raw_write.payload->mirror = 91;
  CHECK(raw.CommitWrite(&raw_write) == thyme::ChannelStatus::kOk);
  RawChannel::Sample raw_sample;
  CHECK(raw.TryRead(&raw_sample) == thyme::ChannelStatus::kOk);
  CHECK(raw_sample.payload != nullptr);
  CHECK(raw.drop_count_observed());
  CHECK(raw_sample.payload->value == 91 && raw_sample.payload->mirror == 91);
  raw_sample.Reset();

  // A write is owned by the channel that produced it: handing it to another
  // channel is rejected instead of publishing into a slot it never reserved.
  RawChannel same_endpoint;
  same_endpoint.Bind(&raw_ring);
  auto owned = raw.BeginWriteAt(sizeof(Small), 888);
  CHECK(owned.status == thyme::ChannelStatus::kOk);
  CHECK(owned.bound());
  owned.payload->value = 17;
  CHECK(same_endpoint.CommitWrite(&owned) == thyme::ChannelStatus::kInvalid);
  CHECK(owned.Commit() == thyme::ChannelStatus::kOk);
  CHECK(raw.TryRead(&raw_sample) == thyme::ChannelStatus::kOk);
  CHECK(raw_sample.payload->value == 17);
  CHECK(owned.Commit() == thyme::ChannelStatus::kInvalid);
  raw_sample.Reset();

  raw_ring.Close();
  thyme::ShmRegion::UnlinkName(raw_name);

  std::printf("test_channel_facade ok\n");
  return 0;
}
