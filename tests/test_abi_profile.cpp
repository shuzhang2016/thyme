// ABI-13: build-time capacity profile.
//
// The profile knob only exists so a deployment can raise three table sizes
// (SPMC readers, record-mirror structs, health workers) without forking the
// headers. Two builds with different values must never interoperate, and that
// is enforced by ShmLayoutHash(): every page that embeds one of these ceilings
// mixes it into the hash, so an attach across profiles fails on layout_hash
// instead of reading a table with the wrong stride.
//
// This test pins what a single build can prove:
//   1. the configured values are inside the range the tables can represent,
//      and the SPMC reader ceiling is still the fixed 8 (its payload offset is
//      exactly filled by the eight drop slots, so it is not a profile knob);
//   2. the default profile still has the reviewed literals;
//   3. the three page identities are non-zero and distinct — a copy-paste that
//      made two pages share an identity would let a wrong page attach;
//   4. ShmLayoutHash is sensitive to each ceiling, so changing one cannot
//      silently keep the old identity.
//
// Cross-profile rejection itself is covered by the ARCH-11 attach matrix
// (test_abi_cross_version) with a deliberately wrong layout_hash; reproducing
// it here would need a second build tree.
#include "thyme/check.h"
#include "thyme/limits.h"
#include "thyme/runtime/record_mirror_ring.h"
#include "thyme/runtime/shared_health.h"
#include "thyme/runtime/shm_page.h"
#include "thyme/runtime/shm_spmc_ring.h"

#include <cstdint>
#include <cstdio>

namespace {

constexpr uint64_t Mix(uint64_t a, uint64_t b, uint64_t c) {
  return thyme::ShmLayoutHash(a, b, c);
}

}  // namespace

int main() {
  using namespace thyme;

  // 1. Range: the tables must be able to represent the configured size.
  //    Readers are not a profile knob: the payload offset is exactly filled by
  //    the eight drop slots, so 8 is the only supported value.
  CHECK(kAbiMaxSpmcReaders == 8);
  CHECK(kAbiMaxStructCount >= 1);
  CHECK(kAbiMaxStructCount <= 1024);  // contract kinds per package/mirror page
  CHECK(kAbiMaxThreads >= 1);
  CHECK(kAbiMaxThreads <= 1024);  // per-process worker ceiling

#if !defined(THYME_ABI_PROFILE_CUSTOM)
  // 2. Default profile: the reviewed literals, unchanged.
  CHECK(kAbiMaxStructCount == 64);
  CHECK(kAbiMaxThreads == 64);
#endif

  // 3. Distinct, non-zero page identities.
  const uint64_t health = SharedHealthPageLayoutHash();
  const uint64_t mirror = RecordMirrorRingPage::LayoutHash();
  const uint64_t spmc =
      ShmSpmcRing<uint32_t, 8>::Layout::LayoutHash();
  CHECK(health != 0);
  CHECK(mirror != 0);
  CHECK(spmc != 0);
  CHECK(health != mirror);
  CHECK(health != spmc);
  CHECK(mirror != spmc);

  // 4. Every ceiling participates in the hash. A page that forgot one would
  //    attach across profiles with a stale identity.
  CHECK(Mix(8, 64, 64) != Mix(16, 64, 64));
  CHECK(Mix(8, 64, 64) != Mix(8, 128, 64));
  CHECK(Mix(8, 64, 64) != Mix(8, 64, 128));

  std::printf(
      "abi-profile: readers=%zu structs=%u threads=%zu "
      "health=0x%016llx mirror=0x%016llx spmc=0x%016llx\n",
      kAbiMaxSpmcReaders, kAbiMaxStructCount, kAbiMaxThreads,
      static_cast<unsigned long long>(health),
      static_cast<unsigned long long>(mirror),
      static_cast<unsigned long long>(spmc));
  return 0;
}
