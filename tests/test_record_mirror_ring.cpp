#include "thyme/check.h"
#include "thyme/record/record_mirror_ring_client.h"
#include "thyme/record/record_table.h"
#include "thyme/runtime/record_mirror_ring.h"
#include "thyme/runtime/shm_region.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

using namespace thyme;

namespace {

uint32_t TestPayloadSize(uint16_t sid) { return sid == 0 ? 8 : 16; }
ThymeRecordPriority TestPriority(uint16_t) { return ThymeRecordPriority::kP1; }
void TestFillDescs(ThymeStructDesc* out, std::size_t cap) {
  if (cap < 2) return;
  out[0].struct_id = 0;
  out[0].contract_version = 1;
  out[0].payload_size = 8;
  std::snprintf(out[0].name, sizeof(out[0].name), "Alpha");
  out[1].struct_id = 1;
  out[1].contract_version = 2;
  out[1].payload_size = 16;
  std::snprintf(out[1].name, sizeof(out[1].name), "Beta");
}

ThymeStructDesc MakeDesc(uint16_t id, uint32_t payload, const char* name) {
  ThymeStructDesc d{};
  d.struct_id = id;
  d.contract_version = 1;
  d.payload_size = payload;
  std::snprintf(d.name, sizeof(d.name), "%s", name);
  return d;
}

void TestGateAppearsDuringWrite() {
  char name[64];
  std::snprintf(name, sizeof(name), "/c9ring_gate_%d",
                static_cast<int>(getpid()));
  ShmRegion::UnlinkName(name);
  ThymeStructDesc desc = MakeDesc(0, kRecordMirrorMaxPayload, "Gated");
  const RecordMirrorRingSpec spec{kRecordMirrorMaxPayload, 1};
  ShmRegion region;
  CHECK(region.Create(name, RecordMirrorRingPage::RequiredBytes(&desc, &spec, 1)));
  auto* page = static_cast<RecordMirrorRingPage*>(region.data());
  CHECK(page->Init(1, &desc, &spec, 1));
  std::vector<uint8_t> payload(kRecordMirrorMaxPayload, 0x5A);
  auto* slot = reinterpret_cast<RecordMirrorRingSlotHeader*>(page->ArenaBase());

  const pid_t writer = fork();
  CHECK(writer >= 0);
  if (writer == 0) {
    const bool written = page->TryWriteRing(0, payload.data(),
                                            kRecordMirrorMaxPayload, 100, 1);
    _exit(written ? 1 : 0);
  }

  const uint64_t deadline = ProducerGateNowNs() + 2000000000ULL;
  while ((slot->seq.load(std::memory_order_acquire) & 1u) == 0) {
    CHECK(ProducerGateNowNs() < deadline);
  }
  constexpr uint64_t kToken = 0x12345678ABCDEF01ULL;
  ProducerGate* gate = page->producer_gate(0);
  CHECK(gate != nullptr);
  CHECK(ProducerGateAcquire(*gate, kToken, ProducerGateNowNs(),
                            kProducerGateDefaultTimeoutNs) == GateResult::kOk);
  int status = 0;
  CHECK(waitpid(writer, &status, 0) == writer);
  CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
  CHECK(page->RingHead(0) == 0);
  CHECK(page->write_generation() == 0);
  CHECK((slot->seq.load(std::memory_order_acquire) & 1u) == 0);
  CHECK(slot->seq.load(std::memory_order_acquire) != 0);
  CHECK(gate->rejects.load(std::memory_order_acquire) == 1);

  CHECK(page->TryWriteRing(0, payload.data(), kRecordMirrorMaxPayload, 101, 2,
                           kToken));
  CHECK(page->RingHead(0) == 1);
  CHECK(page->write_generation() == 1);
  std::vector<uint8_t> out(kRecordMirrorMaxPayload);
  uint32_t out_size = 0;
  uint64_t mono_ns = 0;
  uint32_t source_sequence = 0;
  CHECK(page->ReadRingSample(0, 0, out.data(), kRecordMirrorMaxPayload, out_size,
                             mono_ns, source_sequence));
  CHECK(out_size == kRecordMirrorMaxPayload);
  CHECK(mono_ns == 101);
  CHECK(source_sequence == 2);
  CHECK(std::memcmp(out.data(), payload.data(), kRecordMirrorMaxPayload) == 0);
  CHECK(ProducerGateRelease(*gate, kToken) == GateResult::kOk);
  region.Unmap();
  ShmRegion::UnlinkName(name);
}

}  // namespace

int main() {
  // SHM seqlock contract: lock-free atomics are asserted at compile time in
  // record_mirror_ring.h; keep a runtime witness too.
  CHECK(std::atomic<uint32_t>::is_always_lock_free);
  CHECK(std::atomic<uint64_t>::is_always_lock_free);

  // --- Sizing: arena is computed from the ring spec, not a fixed budget. ---
  {
    ThymeStructDesc descs[2] = {MakeDesc(0, 100, "A"), MakeDesc(1, 200, "B")};
    RecordMirrorRingSpec specs[2] = {{100, 4}, {200, 8}};
    const uint64_t arena =
        RecordMirrorRingPage::ComputeArenaBytes(descs, specs, 2);
    // struct0: (64 + round_up(100,64)=128) * 4 = 768
    // struct1: (64 + round_up(200,64)=256) * 8 = 2560
    CHECK(arena == 768 + 2560);
    CHECK(RecordMirrorRingPage::RequiredBytes(descs, specs, 2) ==
          sizeof(RecordMirrorRingPage) + arena);
    // Invalid specs are rejected: zero payload, zero depth, oversize payload.
    RecordMirrorRingSpec bad0[1] = {{0, 4}};
    ThymeStructDesc one[1] = {MakeDesc(0, 100, "A")};
    CHECK(RecordMirrorRingPage::RequiredBytes(one, bad0, 1) == 0);
    RecordMirrorRingSpec bad1[1] = {{100, 0}};
    CHECK(RecordMirrorRingPage::RequiredBytes(one, bad1, 1) == 0);
    RecordMirrorRingSpec bad2[1] = {{kRecordMirrorMaxPayload + 1, 1}};
    CHECK(RecordMirrorRingPage::RequiredBytes(one, bad2, 1) == 0);
  }

  // --- Config hash: same config -> same hash; any change flips it. ---
  {
    ThymeStructDesc descs[2] = {MakeDesc(0, 100, "A"), MakeDesc(1, 200, "B")};
    RecordMirrorRingSpec specs[2] = {{100, 4}, {200, 8}};
    const uint64_t h0 = RecordMirrorRingPage::HashConfig(descs, specs, 2);
    CHECK(h0 == RecordMirrorRingPage::HashConfig(descs, specs, 2));
    RecordMirrorRingSpec deeper[2] = {{100, 5}, {200, 8}};
    CHECK(RecordMirrorRingPage::HashConfig(descs, deeper, 2) != h0);
    RecordMirrorRingSpec bigger[2] = {{128, 4}, {200, 8}};
    CHECK(RecordMirrorRingPage::HashConfig(descs, bigger, 2) != h0);
  }

  // --- Ring write/read, wrap-around eviction, window harvest. ---
  char name[64];
  std::snprintf(name, sizeof(name), "/c9ring_%d", static_cast<int>(getpid()));
  ShmRegion::UnlinkName(name);

  ThymeStructDesc descs[2] = {MakeDesc(0, 8, "Alpha"), MakeDesc(1, 16, "Beta")};
  RecordMirrorRingSpec specs[2] = {{8, 4}, {16, 2}};
  const uint64_t bytes = RecordMirrorRingPage::RequiredBytes(descs, specs, 2);
  CHECK(bytes > 0);

  record_mirror::RingClient client;
  CHECK(client.Create(name, descs, specs, 2, 7));
  RecordMirrorRingPage* page = client.Page();
  CHECK(page != nullptr);
  CHECK(page->Validate());
  // OPS-02: a second live producer cannot unlink the mapped ring.
  record_mirror::RingClient duplicate;
  CHECK(!duplicate.Create(name, descs, specs, 2, 7));
  CHECK(client.Page()->Validate());
  CHECK(page->struct_count() == 2);
  CHECK(page->RingDepth(0) == 4);
  CHECK(page->RingDepth(1) == 2);
  CHECK(page->ValidateConfig(page->config_hash()));
  CHECK(!page->ValidateConfig(page->config_hash() + 1));

  // Push 6 samples on struct 0 (depth 4) -> indexes 0,1 evicted.
  for (uint64_t i = 0; i < 6; ++i) {
    const uint64_t v = 1000 + i;
    CHECK(page->TryWriteRing(0, &v, sizeof(v), 100 + i * 10,
                             static_cast<uint32_t>(i)));
  }
  CHECK(page->RingHead(0) == 6);
  CHECK(page->write_generation() == 6);

  uint64_t out = 0;
  uint32_t sz = 0;
  uint64_t mono = 0;
  uint32_t sseq = 0;
  uint32_t stable = 0;
  // Evicted indexes read as overrun holes (false, no torn-drop counted).
  uint64_t torn = 0;
  CHECK(!page->ReadRingSample(0, 0, reinterpret_cast<uint8_t*>(&out),
                              sizeof(out), sz, mono, sseq, nullptr, &torn));
  CHECK(!page->ReadRingSample(0, 1, reinterpret_cast<uint8_t*>(&out),
                              sizeof(out), sz, mono, sseq, nullptr, &torn));
  CHECK(torn == 0);
  // Retained window [2, 6) reads back with exact metadata.
  for (uint64_t i = 2; i < 6; ++i) {
    CHECK(page->ReadRingSample(0, i, reinterpret_cast<uint8_t*>(&out),
                               sizeof(out), sz, mono, sseq, &stable));
    CHECK(out == 1000 + i);
    CHECK(sz == 8);
    CHECK(mono == 100 + i * 10);
    CHECK(sseq == static_cast<uint32_t>(i));
    CHECK(stable != 0 && (stable & 1u) == 0);
  }

  // Harvest [t_begin, t_end] from the retained window.
  RecordMirrorRingSampleDesc harvest[8];
  uint32_t n = page->HarvestWindow(0, 120, 150, harvest, 8);
  CHECK(n == 4);  // mono 120,130,140,150 -> publish indexes 2..5
  for (uint32_t i = 0; i < n; ++i) {
    CHECK(harvest[i].publish_index == 2 + i);
    CHECK(harvest[i].mono_ns == 120 + i * 10);
    CHECK(harvest[i].payload_size == 8);
  }
  // Narrower window crops both ends.
  n = page->HarvestWindow(0, 121, 149, harvest, 8);
  CHECK(n == 2);
  CHECK(harvest[0].publish_index == 3);
  CHECK(harvest[1].publish_index == 4);
  // Output cap truncates.
  n = page->HarvestWindow(0, 120, 150, harvest, 3);
  CHECK(n == 3);

  // struct 1 untouched so far.
  CHECK(page->RingHead(1) == 0);
  RecordMirrorRingSampleDesc none[2];
  CHECK(page->HarvestWindow(1, 0, ~0ULL, none, 2) == 0);

  // --- depth=1 degenerates to latest-value semantics. ---
  {
    ThymeStructDesc d1[1] = {MakeDesc(0, 8, "Latest")};
    RecordMirrorRingSpec s1[1] = {{8, 1}};
    const uint64_t b1 = RecordMirrorRingPage::RequiredBytes(d1, s1, 1);
    CHECK(b1 > 0);
    auto region = std::make_unique<ShmRegion>();
    char n1[64];
    std::snprintf(n1, sizeof(n1), "/c9ring1_%d", static_cast<int>(getpid()));
    ShmRegion::UnlinkName(n1);
    CHECK(region->Create(n1, b1));
    auto* p1 = static_cast<RecordMirrorRingPage*>(region->data());
    CHECK(p1->Init(1, d1, s1, 1));
    for (uint64_t i = 0; i < 5; ++i) {
      CHECK(p1->TryWriteRing(0, &i, sizeof(i), i, static_cast<uint32_t>(i)));
    }
    CHECK(p1->RingHead(0) == 5);
    uint64_t v = 0;
    uint32_t z = 0;
    uint64_t m = 0;
    uint32_t sq = 0;
    // Only the latest survives; every older index is an overrun hole.
    for (uint64_t i = 0; i < 4; ++i) {
      CHECK(!p1->ReadRingSample(0, i, reinterpret_cast<uint8_t*>(&v),
                                sizeof(v), z, m, sq));
    }
    CHECK(p1->ReadRingSample(0, 4, reinterpret_cast<uint8_t*>(&v), sizeof(v),
                             z, m, sq));
    CHECK(v == 4);
    region->Unmap();
    ShmRegion::UnlinkName(n1);
  }

  // --- source_sequence wraps without affecting absolute sample identity. ---
  {
    ThymeStructDesc wrap_desc[1] = {MakeDesc(0, 8, "SequenceWrap")};
    RecordMirrorRingSpec wrap_spec[1] = {{8, 2}};
    const uint64_t wrap_bytes =
        RecordMirrorRingPage::RequiredBytes(wrap_desc, wrap_spec, 1);
    CHECK(wrap_bytes > 0);
    auto wrap_region = std::make_unique<ShmRegion>();
    char wrap_name[64];
    std::snprintf(wrap_name, sizeof(wrap_name), "/c9ring_wrap_%d",
                  static_cast<int>(getpid()));
    ShmRegion::UnlinkName(wrap_name);
    CHECK(wrap_region->Create(wrap_name, wrap_bytes));
    auto* wrap_page = static_cast<RecordMirrorRingPage*>(wrap_region->data());
    CHECK(wrap_page->Init(1, wrap_desc, wrap_spec, 1));
    ProducerGate* wrap_gate = wrap_page->producer_gate(0);
    CHECK(wrap_gate != nullptr);
    CHECK(wrap_gate->owner_token.load(std::memory_order_acquire) ==
          kProducerGateUnheld);

    const uint64_t payloads[3] = {0x1111111111111111ULL,
                                  0x2222222222222222ULL,
                                  0x3333333333333333ULL};
    const uint64_t monos[3] = {7000, 7010, 7020};
    const uint32_t source_sequences[3] = {UINT32_MAX, 0, 1};
    uint64_t previous_head = 0;
    uint64_t wrap_out = 0;
    uint32_t wrap_size = 0;
    uint64_t wrap_mono = 0;
    uint32_t wrap_source_sequence = 0;
    for (uint64_t i = 0; i < 3; ++i) {
      CHECK(wrap_page->TryWriteRing(0, &payloads[i], sizeof(payloads[i]),
                                    monos[i], source_sequences[i]));
      const uint64_t head = wrap_page->RingHead(0);
      CHECK(head == i + 1);
      CHECK(head > previous_head);
      previous_head = head;

      RecordMirrorRingSampleDesc peek{};
      CHECK(wrap_page->PeekRingSample(0, i, peek));
      CHECK(peek.publish_index == i);
      CHECK(peek.publish_index + 1 == head);
      CHECK(peek.mono_ns == monos[i]);
      CHECK(peek.payload_size == sizeof(payloads[i]));
      CHECK(wrap_page->ReadRingSample(
          0, i, reinterpret_cast<uint8_t*>(&wrap_out), sizeof(wrap_out),
          wrap_size, wrap_mono, wrap_source_sequence));
      CHECK(wrap_out == payloads[i]);
      CHECK(wrap_size == sizeof(payloads[i]));
      CHECK(wrap_mono == monos[i]);
      CHECK(wrap_source_sequence == source_sequences[i]);
    }

    // Depth 2 evicts publish_index 0 even though its source_sequence was the
    // numerically largest value. The wrapped 0 and 1 samples remain independent
    // new samples addressed only by their absolute publish indexes.
    RecordMirrorRingSampleDesc evicted{};
    CHECK(!wrap_page->PeekRingSample(0, 0, evicted));
    CHECK(!wrap_page->ReadRingSample(
        0, 0, reinterpret_cast<uint8_t*>(&wrap_out), sizeof(wrap_out), wrap_size,
        wrap_mono, wrap_source_sequence));
    for (uint64_t i = 1; i < 3; ++i) {
      RecordMirrorRingSampleDesc peek{};
      CHECK(wrap_page->PeekRingSample(0, i, peek));
      CHECK(peek.publish_index == i);
      CHECK(peek.mono_ns == monos[i]);
      CHECK(wrap_page->ReadRingSample(
          0, i, reinterpret_cast<uint8_t*>(&wrap_out), sizeof(wrap_out),
          wrap_size, wrap_mono, wrap_source_sequence));
      CHECK(wrap_out == payloads[i]);
      CHECK(wrap_mono == monos[i]);
      CHECK(wrap_source_sequence == source_sequences[i]);
    }
    CHECK(wrap_page->RingHead(0) == 3);
    wrap_region->Unmap();
    ShmRegion::UnlinkName(wrap_name);
  }

  // --- Oversize: payload over slot capacity drops and counts. ---
  {
    // struct 0 capacity = RoundUpCapacity(8) = 64.
    std::vector<uint8_t> big(128, 0xAB);
    CHECK(!page->TryWriteRing(0, big.data(), 128, 1, 1));
    CHECK(page->oversize_drop_count() == 1);
  }

  // --- Torn read: a slot stuck mid-write (odd seq) fails the read after the
  // retry budget and counts a ring drop without blocking. ---
  {
    // Force-publish one good sample on struct 1, then jam its slot seq odd.
    const uint64_t v = 0x1122334455667788ULL;
    CHECK(page->TryWriteRing(1, &v, sizeof(v), 500, 9));
    RecordMirrorRingDesc& desc = page->ring_descs[1];
    const uint64_t head = desc.ring_head.load(std::memory_order_acquire);
    auto* slot = reinterpret_cast<RecordMirrorRingSlotHeader*>(
        page->ArenaBase() + desc.slots_offset +
        ((head - 1) % desc.ring_depth) * desc.slot_stride);
    const uint32_t saved = slot->seq.load(std::memory_order_acquire);
    slot->seq.store(saved | 1u, std::memory_order_release);  // odd = writing
    uint8_t buf[16];
    uint32_t z = 0;
    uint64_t m = 0;
    uint32_t sq = 0;
    uint64_t torn2 = 0;
    CHECK(!page->ReadRingSample(1, head - 1, buf, sizeof(buf), z, m, sq,
                                nullptr, &torn2));
    CHECK(torn2 == 1);  // counted into the caller-owned counter, not shm
    slot->seq.store(saved, std::memory_order_release);  // restore
    CHECK(page->ReadRingSample(1, head - 1, buf, sizeof(buf), z, m, sq));
    uint64_t got = 0;
    std::memcpy(&got, buf, sizeof(got));
    CHECK(got == v);
  }

  // --- Cross-process read-only attach + config hash enforcement. ---
  pid_t pid = fork();
  CHECK(pid >= 0);
  if (pid == 0) {
    record_mirror::RingClient ro;
    CHECK(ro.AttachReadOnly(name, 1000000000ULL, page->config_hash()));
    CHECK(ro.Page()->Validate());
    uint64_t v = 0;
    uint32_t z = 0;
    uint64_t m = 0;
    uint32_t sq = 0;
    CHECK(ro.Page()->ReadRingSample(0, 5, reinterpret_cast<uint8_t*>(&v),
                                    sizeof(v), z, m, sq));
    CHECK(v == 1005);
    // Wrong config hash must be rejected.
    record_mirror::RingClient bad;
    CHECK(!bad.AttachReadOnly(name, 100000000ULL, page->config_hash() + 1));
    _exit(0);
  }
  int st = 0;
  CHECK(waitpid(pid, &st, 0) == pid);
  CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0);

  // --- Per-struct replay gate: only the gated struct blocks real writes. ---
  {
    ProducerGate* gate_a = page->producer_gate(0);
    ProducerGate* gate_b = page->producer_gate(1);
    CHECK(gate_a != nullptr && gate_b != nullptr);
    CHECK(page->producer_gate(2) == nullptr);
    constexpr uint64_t kToken = 0x0FEDCBA987654321ULL;
    CHECK(ProducerGateAcquire(*gate_a, kToken, ProducerGateNowNs(),
                              kProducerGateDefaultTimeoutNs) == GateResult::kOk);
    const uint64_t value = 0xA5A5A5A5A5A5A5A5ULL;
    CHECK(!page->TryWriteRing(0, &value, sizeof(value), 600, 10));
    CHECK(page->RingHead(0) == 6);
    CHECK(gate_a->rejects.load(std::memory_order_acquire) == 1);
    CHECK(page->TryWriteRing(1, &value, sizeof(value), 600, 10));
    CHECK(page->RingHead(1) == 2);
    CHECK(gate_b->rejects.load(std::memory_order_acquire) == 0);
    CHECK(page->TryWriteRing(0, &value, sizeof(value), 601, 11, kToken));
    CHECK(page->RingHead(0) == 7);
    CHECK(ProducerGateRelease(*gate_a, kToken) == GateResult::kOk);

    // An expired lease is reclaimed by a plain writer and counted as a steal.
    constexpr uint64_t kExpiredToken = 0x0123456789ABCDEFULL;
    CHECK(ProducerGateAcquire(
              *gate_a, kExpiredToken,
              ProducerGateNowNs() - kProducerGateDefaultTimeoutNs - 1,
              kProducerGateDefaultTimeoutNs) == GateResult::kOk);
    CHECK(page->TryWriteRing(0, &value, sizeof(value), 602, 12));
    CHECK(page->RingHead(0) == 8);
    CHECK(gate_a->steals.load(std::memory_order_acquire) == 1);
  }

  // --- Table-driven CreateRing via the process default name. ---
  {
    const ThymeRecordTable table{2, TestPayloadSize, TestPriority,
                                 TestFillDescs};
    RecordMirrorRingSpec tspecs[2] = {{8, 3}, {16, 5}};
    const std::string expected = record_mirror::DefaultRingShmName();
    ShmRegion::UnlinkName(expected.c_str());
    record_mirror::RingClient tc;
    CHECK(tc.CreateRing(record_mirror::DefaultRingShmName().c_str(), table,
                        tspecs, 3));
    CHECK(tc.Page() != nullptr);
    CHECK(tc.Page()->Validate());
    CHECK(tc.Page()->RingDepth(0) == 3);
    CHECK(tc.Page()->RingDepth(1) == 5);
    CHECK(tc.Page()->header.session_id.load(std::memory_order_acquire) == 3);
    tc.Shutdown(true);
  }

  client.Shutdown(true);
  TestGateAppearsDuringWrite();
  return 0;
}
