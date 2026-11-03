// ARCH-11: unit tests for the v3 described LatestSlot<T> page.
//
// Pure/compile-time coverage of the layout model (64 B header, offsets,
// sizeof), the combined PageSpec identity, and runtime rejection paths for
// mismatched layout/page_bytes/feature/version, short mappings and legacy v2
// (header-less) pages. The SHM cases exercise the described Create/Open path
// end to end, including a write through the hot path and a read-only peer.

#include "thyme/check.h"
#include "thyme/runtime/latest_slot.h"
#include "thyme/runtime/latest_slot_page.h"
#include "thyme/runtime/shm_region.h"

#include "test_shm.h"

#include <cstddef>
#include <cstdint>
#include <new>
#include <string>

using namespace thyme;

namespace {

struct Payload24 {
  uint8_t bytes[24];
};

struct Payload100 {
  uint8_t bytes[100];
};

// The formula the generator used for the (v2) body: 2 cells of
// RoundUp64(payload) plus the 64 B control tail. The model must agree on the
// stride and on the body half of the region; the v3 page adds the 64 B header
// and the 64 B geometry block.
constexpr std::size_t RoundUp64(std::size_t n) {
  return ((n + 63u) / 64u) * 64u;
}

constexpr std::size_t GeneratedLatestSlotBodyBytes(std::size_t payload_bytes) {
  return 2u * RoundUp64(payload_bytes) + 64u;
}

using Model24 = LatestSlotLayoutModel<Payload24>;
using Model100 = LatestSlotLayoutModel<Payload100>;

// ---- 64 B header + 64 B geometry / offsets / sizeof ---------------------
static_assert(sizeof(ShmSuperHeader) == 64, "super-header is one cache line");
static_assert(sizeof(LatestSlotPageGeometry) == 64,
              "geometry block is one cache line");
static_assert(Model24::kMagic == kLatestSlotPageMagic, "model magic");
static_assert(Model24::kAbiVersion == kLatestSlotPageAbiVersion, "model ABI");
static_assert(Model24::kAbiVersion == 3, "page version is v3");
static_assert(Model24::kHeaderBytes == 64, "header leads the page");
static_assert(Model24::kGeometryOffset == 64, "geometry follows the header");
static_assert(Model24::kGeometryBytes == 64, "geometry is one line");
static_assert(Model24::kCellStride == 64, "24 B payload -> one line cell");
static_assert(Model24::kSlotsOffset == 128,
              "payload cells follow header + geometry");
static_assert(Model24::kPublishedIdxOffset == 256,
              "published_idx_ = 128 + 2 * 64");
static_assert(Model24::kSeqOffset == 264, "seq_ follows published_idx_");
static_assert(Model24::kRegionBytes == 320, "24 B payload page is 320 B");
static_assert(sizeof(LatestSlotPage<Payload24>) == Model24::kRegionBytes,
              "wrapper size is the model region size");
static_assert(alignof(LatestSlotPage<Payload24>) == 64, "page is line-aligned");

static_assert(Model100::kCellStride == 128, "100 B payload -> two lines");
static_assert(Model100::kPublishedIdxOffset == 384,
              "published_idx_ = 128 + 2 * 128");
static_assert(Model100::kSeqOffset == 392, "seq_ follows published_idx_");
static_assert(Model100::kRegionBytes == 448, "100 B payload page is 448 B");
static_assert(sizeof(LatestSlotPage<Payload100>) == Model100::kRegionBytes,
              "wrapper size is the model region size");

// ---- generated-style stride agreement -----------------------------------
static_assert(Model24::kCellStride == RoundUp64(sizeof(Payload24)),
              "model stride == generated RoundUp64(sizeof(T))");
static_assert(Model100::kCellStride == RoundUp64(sizeof(Payload100)),
              "model stride == generated RoundUp64(sizeof(T))");
static_assert(Model24::kRegionBytes ==
                  Model24::kHeaderBytes + Model24::kGeometryBytes +
                      GeneratedLatestSlotBodyBytes(sizeof(Payload24)),
              "region = header + geometry + generated v2 body");
static_assert(Model100::kRegionBytes ==
                  Model100::kHeaderBytes + Model100::kGeometryBytes +
                      GeneratedLatestSlotBodyBytes(sizeof(Payload100)),
              "region = header + geometry + generated v2 body");
static_assert(sizeof(LatestSlot<Payload24>) ==
                  GeneratedLatestSlotBodyBytes(sizeof(Payload24)),
              "body size is the generated v2 page size");
static_assert(sizeof(LatestSlot<Payload24>) < Model24::kRegionBytes,
              "v2 body cannot satisfy the v3 page size");

constexpr uint64_t kPayloadHashA = 0x1122334455667788ULL;
constexpr uint64_t kPayloadHashB = 0x1122334455667789ULL;

// The spec builder and validator are constexpr, so generated code can pin the
// identity at compile time.
static_assert(Model24::IsSpecValid(Model24::PageSpec(kPayloadHashA)),
              "PageSpec must be valid at compile time");
static_assert(Model24::PageSpec(kPayloadHashA).shm.layout_hash ==
                  LatestSlotPageLayoutHash<Payload24>(kPayloadHashA),
              "spec hash is the combined identity");
static_assert(!Model24::IsSpecValid(Model24::PageSpec(0)),
              "zero payload identity is rejected");
static_assert(LatestSlotPageLayoutHash<Payload24>(0) != 0,
              "zero payload identity never yields a zero combination");

void TestHeaderAndOffsetsAtRuntime() {
  LatestSlotPage<Payload24> page{};
  const auto* base = reinterpret_cast<const unsigned char*>(&page);
  const auto* geometry =
      reinterpret_cast<const unsigned char*>(&page.geometry);
  const auto* body = reinterpret_cast<const unsigned char*>(&page.slot);
  CHECK(static_cast<std::size_t>(geometry - base) == Model24::kGeometryOffset);
  CHECK(static_cast<std::size_t>(body - base) == Model24::kSlotsOffset);
  CHECK(sizeof(LatestSlotPage<Payload24>) == 320);
}

// The in-process capability is unchanged: a bare LatestSlot<T> (or the body
// of an uninitialized page) still publishes and reads without any header.
void TestInProcessBody() {
  LatestSlotPage<Payload24> page{};
  CHECK(page.header.magic.load(std::memory_order_relaxed) == 0);
  Payload24 value{};
  value.bytes[0] = 3;
  Payload24* cell = page.slot.BeginWrite();
  *cell = value;
  page.slot.CommitWrite();
  Payload24 out{};
  CHECK(page.slot.Read(out));
  CHECK(out.bytes[0] == 3);
  // No described header was published: the page is intentionally not a v3
  // SHM page until CreateLatestSlotPage initializes it.
  CHECK(ValidateLatestSlotPage<Payload24>(
            &page, sizeof page, Model24::PageSpec(kPayloadHashA), nullptr) ==
        ShmPageErrc::kNotReady);
}

void TestPageSpecIdentity() {
  const auto spec = Model24::PageSpec(kPayloadHashA);
  CHECK(Model24::IsSpecValid(spec));
  CHECK(spec.payload_layout_hash == kPayloadHashA);
  CHECK(spec.shm.magic == kLatestSlotPageMagic);
  CHECK(spec.shm.abi_version == 3);
  CHECK(spec.shm.page_bytes == Model24::kRegionBytes);
  CHECK(spec.shm.feature_bits == 0);
  CHECK(spec.shm.layout_hash != 0);
  CHECK(spec.shm.layout_hash ==
        LatestSlotPageLayoutHash<Payload24>(kPayloadHashA));
  CHECK(IsShmPageSpecValid(spec.shm));

  // A zero payload identity is rejected outright: it would let any same-size
  // layout validate against this page.
  const auto zero = Model24::PageSpec(0);
  CHECK(zero.payload_layout_hash == 0);
  CHECK(zero.shm.layout_hash == 0);
  CHECK(!Model24::IsSpecValid(zero));
  CHECK(!IsShmPageSpecValid(zero.shm));
  CHECK(LatestSlotPageLayoutHash<Payload24>(0) != 0);

  // Same slot geometry, different payload layout: different combined hash.
  const auto other_payload = Model24::PageSpec(kPayloadHashB);
  CHECK(other_payload.shm.page_bytes == spec.shm.page_bytes);
  CHECK(other_payload.shm.layout_hash != spec.shm.layout_hash);

  // A spec cannot be reassembled from a different payload's hash.
  auto spliced = Model24::PageSpec(kPayloadHashA);
  spliced.shm.layout_hash = other_payload.shm.layout_hash;
  CHECK(!Model24::IsSpecValid(spliced));

  // Geometry fields are part of the identity, not just a free-form hash slot.
  auto wrong_bytes = Model24::PageSpec(kPayloadHashA);
  wrong_bytes.shm.page_bytes += 64;
  CHECK(!Model24::IsSpecValid(wrong_bytes));
  auto wrong_version = Model24::PageSpec(kPayloadHashA);
  wrong_version.shm.abi_version = 2;
  CHECK(!Model24::IsSpecValid(wrong_version));
  auto wrong_magic = Model24::PageSpec(kPayloadHashA);
  wrong_magic.shm.magic ^= 1;
  CHECK(!Model24::IsSpecValid(wrong_magic));
}

void TestValidateRejections() {
  alignas(64) unsigned char storage[Model24::kRegionBytes] = {};
  auto* page = new (storage) LatestSlotPage<Payload24>();
  const auto spec = Model24::PageSpec(kPayloadHashA);
  InitLatestSlotPageGeometry(&page->geometry, sizeof(Payload24),
                             Model24::kCellStride, Model24::kSlotCount);
  InitShmSuperHeader(&page->header, spec.shm);

  ShmPageError error;
  CHECK(ValidateLatestSlotPage<Payload24>(storage, sizeof storage, spec,
                                          &error) == ShmPageErrc::kOk);
  CHECK(error.code == ShmPageErrc::kOk);

  // Short mapping: rejected before the body is ever touched.
  CHECK(ValidateLatestSlotPage<Payload24>(storage, Model24::kRegionBytes - 1,
                                          spec, &error) ==
        ShmPageErrc::kTooSmall);
  CHECK(error.expected == Model24::kRegionBytes);
  CHECK(error.actual == Model24::kRegionBytes - 1);
  CHECK(ValidateLatestSlotPage<Payload24>(nullptr, 0, spec, &error) ==
        ShmPageErrc::kTooSmall);

  // Inconsistent / zero-identity specs are rejected even when the header is
  // otherwise intact.
  auto bad_spec = spec;
  bad_spec.shm.page_bytes += 64;
  CHECK(ValidateLatestSlotPage<Payload24>(storage, sizeof storage, bad_spec,
                                          &error) ==
        ShmPageErrc::kInvalidSpec);
  CHECK(ValidateLatestSlotPage<Payload24>(storage, sizeof storage,
                                          Model24::PageSpec(0), &error) ==
        ShmPageErrc::kInvalidSpec);

  page->header.abi_version.store(2, std::memory_order_relaxed);
  CHECK(ValidateLatestSlotPage<Payload24>(storage, sizeof storage, spec,
                                          &error) ==
        ShmPageErrc::kBadAbiVersion);
  CHECK(error.expected == 3 && error.actual == 2);
  InitShmSuperHeader(&page->header, spec.shm);

  page->header.page_bytes.store(Model24::kRegionBytes + 64,
                                std::memory_order_relaxed);
  CHECK(ValidateLatestSlotPage<Payload24>(storage, sizeof storage, spec,
                                          &error) ==
        ShmPageErrc::kBadPageBytes);
  CHECK(error.expected == Model24::kRegionBytes);
  InitShmSuperHeader(&page->header, spec.shm);

  page->header.feature_bits.store(1, std::memory_order_relaxed);
  CHECK(ValidateLatestSlotPage<Payload24>(storage, sizeof storage, spec,
                                          &error) ==
        ShmPageErrc::kBadFeatureBits);
  CHECK(error.expected == 0 && error.actual == 1);
  InitShmSuperHeader(&page->header, spec.shm);

  page->header.layout_hash.store(spec.shm.layout_hash ^ 1u,
                                 std::memory_order_relaxed);
  CHECK(ValidateLatestSlotPage<Payload24>(storage, sizeof storage, spec,
                                          &error) ==
        ShmPageErrc::kBadLayoutHash);
  InitShmSuperHeader(&page->header, spec.shm);

  page->header.magic.store(kLatestSlotPageMagic ^ 1u,
                           std::memory_order_relaxed);
  CHECK(ValidateLatestSlotPage<Payload24>(storage, sizeof storage, spec,
                                          &error) == ShmPageErrc::kBadMagic);
  InitShmSuperHeader(&page->header, spec.shm);

  page->header.reserved0 = 1;
  CHECK(ValidateLatestSlotPage<Payload24>(storage, sizeof storage, spec,
                                          &error) == ShmPageErrc::kBadReserved);
  page->header.reserved0 = 0;

  // Geometry corruption is rejected by the type-erased validator too (the probe
  // uses it): a stride that is not RoundUp64(payload_bytes), a wrong slot
  // count, and an absent payload identity.
  page->geometry.cell_stride.store(Model24::kCellStride + 64,
                                   std::memory_order_relaxed);
  CHECK(ValidateLatestSlotPageGeometry(storage, sizeof storage, &error) ==
        ShmPageErrc::kBadGeometry);
  InitLatestSlotPageGeometry(&page->geometry, sizeof(Payload24),
                             Model24::kCellStride, Model24::kSlotCount);

  page->geometry.slot_count.store(3, std::memory_order_relaxed);
  CHECK(ValidateLatestSlotPageGeometry(storage, sizeof storage, &error) ==
        ShmPageErrc::kBadGeometry);
  InitLatestSlotPageGeometry(&page->geometry, sizeof(Payload24),
                             Model24::kCellStride, Model24::kSlotCount);

  page->geometry.payload_bytes.store(0, std::memory_order_relaxed);
  CHECK(ValidateLatestSlotPageGeometry(storage, sizeof storage, &error) ==
        ShmPageErrc::kBadGeometry);
  InitLatestSlotPageGeometry(&page->geometry, sizeof(Payload24),
                             Model24::kCellStride, Model24::kSlotCount);

  // A page that describes Payload100's geometry cannot attach as Payload24.
  page->geometry.payload_bytes.store(sizeof(Payload100),
                                     std::memory_order_relaxed);
  page->geometry.cell_stride.store(Model100::kCellStride,
                                   std::memory_order_relaxed);
  page->header.page_bytes.store(Model100::kRegionBytes,
                                std::memory_order_relaxed);
  CHECK(ValidateLatestSlotPage<Payload24>(storage, sizeof storage, spec,
                                          &error) ==
        ShmPageErrc::kBadPageBytes);
  InitLatestSlotPageGeometry(&page->geometry, sizeof(Payload24),
                             Model24::kCellStride, Model24::kSlotCount);
  InitShmSuperHeader(&page->header, spec.shm);
}

void TestLegacyV2LayoutRejected() {
  const auto spec = Model24::PageSpec(kPayloadHashA);
  ShmPageError error;

  // v2 page = bare LatestSlot<T> at offset 0. Publish twice so the first
  // payload cell (offset 0) is filled: its first bytes read as a bogus magic.
  alignas(64) unsigned char legacy[4096] = {};
  auto* old = new (legacy) LatestSlot<Payload24>();
  Payload24 value{};
  value.bytes[0] = 0xAA;
  old->Write(value);
  old->Write(value);
  CHECK(ValidateLatestSlotPage<Payload24>(legacy, sizeof legacy, spec,
                                          &error) ==
        ShmPageErrc::kBadMagic);
  CHECK(error.expected == kLatestSlotPageMagic);
  CHECK(error.actual != kLatestSlotPageMagic);

  // A never-written v2 page has a zero leading word: "not ready", never a
  // valid v3 page.
  alignas(64) unsigned char blank[4096] = {};
  CHECK(ValidateLatestSlotPage<Payload24>(blank, sizeof blank, spec, &error) ==
        ShmPageErrc::kNotReady);
}

void TestShmCreateWriteRead() {
  const std::string name = test::ShmTestName("latest_page_rw");
  ShmRegion::UnlinkName(name.c_str());

  ShmPageError error;
  ShmRegion writer;
  auto* page = CreateLatestSlotPage<Payload24>(writer, name.c_str(),
                                               kPayloadHashA, 0, &error);
  CHECK(page != nullptr);
  CHECK(error.code == ShmPageErrc::kOk);
  CHECK(writer.size() >= Model24::kRegionBytes);
  const auto* header = static_cast<const ShmSuperHeader*>(writer.data());
  CHECK(header->magic.load(std::memory_order_acquire) == kLatestSlotPageMagic);
  CHECK(header->abi_version.load(std::memory_order_relaxed) == 3);
  CHECK(header->page_bytes.load(std::memory_order_relaxed) ==
        Model24::kRegionBytes);
  const auto* geometry = reinterpret_cast<const LatestSlotPageGeometry*>(
      static_cast<const unsigned char*>(writer.data()) + Model24::kGeometryOffset);
  CHECK(geometry->payload_bytes.load(std::memory_order_relaxed) ==
        sizeof(Payload24));
  CHECK(geometry->cell_stride.load(std::memory_order_relaxed) ==
        Model24::kCellStride);
  CHECK(geometry->slot_count.load(std::memory_order_relaxed) == 2);

  Payload24 value{};
  for (std::size_t i = 0; i < sizeof(value.bytes); ++i) {
    value.bytes[i] = static_cast<uint8_t>(i + 1u);
  }
  Payload24* cell = page->slot.BeginWrite();
  *cell = value;
  page->slot.CommitWrite();

  ShmRegion reader;
  const auto* read_page = OpenLatestSlotPageReadOnly<Payload24>(
      reader, name.c_str(), kPayloadHashA, 0, 0, &error);
  CHECK(read_page != nullptr);
  CHECK(error.code == ShmPageErrc::kOk);
  CHECK(reader.size() >= Model24::kRegionBytes);

  Payload24 out{};
  CHECK(read_page->slot.Read(out));
  CHECK(out.bytes[0] == value.bytes[0]);
  CHECK(out.bytes[23] == value.bytes[23]);
  uint64_t seen = 0;
  CHECK(read_page->slot.ReadIfNew(seen, out));
  CHECK(out.bytes[23] == value.bytes[23]);
  CHECK(!read_page->slot.ReadIfNew(seen, out));

  ShmRegion second_writer;
  auto* writable = OpenLatestSlotPage<Payload24>(
      second_writer, name.c_str(), kPayloadHashA, 0, 0, &error);
  CHECK(writable != nullptr);
  writable->slot.BeginWrite()->bytes[0] = 0x77;
  writable->slot.CommitWrite();
  CHECK(read_page->slot.Read(out));
  CHECK(out.bytes[0] == 0x77);

  const auto* base = reinterpret_cast<const unsigned char*>(read_page);
  const auto* body = reinterpret_cast<const unsigned char*>(&read_page->slot);
  CHECK(static_cast<std::size_t>(body - base) == Model24::kSlotsOffset);

  ShmRegion::UnlinkName(name.c_str());
}

void TestShmOpenRejections() {
  const std::string name = test::ShmTestName("latest_page_reject");
  ShmRegion::UnlinkName(name.c_str());

  ShmPageError error;
  ShmRegion writer;
  auto* page = CreateLatestSlotPage<Payload24>(writer, name.c_str(),
                                               kPayloadHashA, 0, &error);
  CHECK(page != nullptr);

  ShmRegion zero_hash;
  CHECK(CreateLatestSlotPage<Payload24>(zero_hash, name.c_str(), 0, 0,
                                        &error) == nullptr);
  CHECK(error.code == ShmPageErrc::kInvalidSpec);

  // Wrong payload layout identity (same size, different payload hash).
  ShmRegion wrong_layout;
  CHECK(OpenLatestSlotPageReadOnly<Payload24>(wrong_layout, name.c_str(),
                                              kPayloadHashB, 0, 0,
                                              &error) == nullptr);
  CHECK(error.code == ShmPageErrc::kBadLayoutHash);

  // Wrong feature bits.
  ShmRegion wrong_feature;
  CHECK(OpenLatestSlotPageReadOnly<Payload24>(wrong_feature, name.c_str(),
                                              kPayloadHashA, 0, 1,
                                              &error) == nullptr);
  CHECK(error.code == ShmPageErrc::kBadFeatureBits);

  // Wrong version word (a v2-style peer page with a v3 geometry).
  page->header.abi_version.store(2, std::memory_order_relaxed);
  ShmRegion wrong_version;
  CHECK(OpenLatestSlotPageReadOnly<Payload24>(wrong_version, name.c_str(),
                                              kPayloadHashA, 0, 0,
                                              &error) == nullptr);
  CHECK(error.code == ShmPageErrc::kBadAbiVersion);
  InitShmSuperHeader(&page->header, Model24::PageSpec(kPayloadHashA).shm);

  // Wrong recorded page size.
  page->header.page_bytes.store(Model24::kRegionBytes + 64,
                                std::memory_order_relaxed);
  ShmRegion wrong_bytes;
  CHECK(OpenLatestSlotPageReadOnly<Payload24>(wrong_bytes, name.c_str(),
                                              kPayloadHashA, 0, 0,
                                              &error) == nullptr);
  CHECK(error.code == ShmPageErrc::kBadPageBytes);

  ShmRegion::UnlinkName(name.c_str());
}

void TestLegacyShmPageRejected() {
  const std::string name = test::ShmTestName("latest_page_v2");
  ShmRegion::UnlinkName(name.c_str());

  ShmRegion legacy;
  auto* old = legacy.Create<LatestSlot<Payload24>>(name.c_str(), 4096);
  CHECK(old != nullptr);
  Payload24 value{};
  value.bytes[0] = 0xAA;
  old->Write(value);
  old->Write(value);

  ShmPageError error;
  ShmRegion reader;
  CHECK(OpenLatestSlotPageReadOnly<Payload24>(reader, name.c_str(),
                                              kPayloadHashA, 0, 0,
                                              &error) == nullptr);
  CHECK(error.code == ShmPageErrc::kBadMagic);

  ShmRegion::UnlinkName(name.c_str());
}

void TestGeneratedStyleStride() {
  CHECK(Model24::kCellStride == RoundUp64(sizeof(Payload24)));
  CHECK(Model24::kRegionBytes ==
        Model24::kHeaderBytes + Model24::kGeometryBytes +
            GeneratedLatestSlotBodyBytes(sizeof(Payload24)));
  CHECK(Model100::kCellStride == RoundUp64(sizeof(Payload100)));
  CHECK(Model100::kRegionBytes ==
        Model100::kHeaderBytes + Model100::kGeometryBytes +
            GeneratedLatestSlotBodyBytes(sizeof(Payload100)));
}

}  // namespace

int main() {
  TestHeaderAndOffsetsAtRuntime();
  TestInProcessBody();
  TestPageSpecIdentity();
  TestValidateRejections();
  TestLegacyV2LayoutRejected();
  TestShmCreateWriteRead();
  TestShmOpenRejections();
  TestLegacyShmPageRejected();
  TestGeneratedStyleStride();
  return 0;
}
