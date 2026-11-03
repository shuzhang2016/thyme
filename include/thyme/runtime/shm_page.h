#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace thyme {

struct ShmPageSpec {
  uint64_t magic = 0;
  uint32_t abi_version = 0;
  uint64_t layout_hash = 0;
  uint64_t page_bytes = 0;
  uint64_t feature_bits = 0;
};

// Stable FNV-1a layout identity. Page descriptions pass every ABI-relevant
// value (tag, version, sizeof/alignof, field offsets/sizes, strides, nested
// layout hashes); runtime config and feature values do not belong in it.
// `ShmLayoutHash(parts...)` mixes the parts in declaration order, so two page
// layouts that differ in any of them get different hashes. Feature bits are
// deliberately excluded: they describe optional behaviour, not wire layout,
// and are validated separately.
inline constexpr uint64_t kShmLayoutHashSeed = 14695981039346656037ULL;
constexpr uint64_t ShmLayoutMix(uint64_t hash, uint64_t value) {
  constexpr uint64_t kPrime = 1099511628211ULL;
  for (uint32_t i = 0; i < 8; ++i) {
    hash ^= (value >> (8u * i)) & 0xFFu;
    hash *= kPrime;
  }
  return hash;
}

template <typename... Parts>
constexpr uint64_t ShmLayoutHash(Parts... parts) {
  uint64_t hash = kShmLayoutHashSeed;
  ((hash = ShmLayoutMix(hash, static_cast<uint64_t>(parts))), ...);
  return hash;
}

struct alignas(64) ShmSuperHeader {
  std::atomic<uint64_t> magic{0};
  std::atomic<uint32_t> abi_version{0};
  uint32_t reserved0 = 0;
  std::atomic<uint64_t> layout_hash{0};
  std::atomic<uint64_t> page_bytes{0};
  std::atomic<uint64_t> feature_bits{0};
  uint64_t reserved[3] = {};
};

static_assert(sizeof(ShmSuperHeader) == 64, "SHM super-header is 64 bytes");
static_assert(offsetof(ShmSuperHeader, magic) == 0, "magic leads the page");
static_assert(offsetof(ShmSuperHeader, abi_version) == 8,
              "version follows magic");
static_assert(offsetof(ShmSuperHeader, layout_hash) == 16,
              "layout hash offset is ABI");
static_assert(offsetof(ShmSuperHeader, page_bytes) == 24,
              "page size offset is ABI");
static_assert(offsetof(ShmSuperHeader, feature_bits) == 32,
              "feature bits offset is ABI");

enum class ShmPageErrc : uint8_t {
  kOk = 0,
  kNotReady,
  kTooSmall,
  kBadMagic,
  kBadAbiVersion,
  kBadLayoutHash,
  kBadPageBytes,
  kBadFeatureBits,
  kBadReserved,
  kBadGeometry,
  kInvalidSpec,
};

struct ShmPageError {
  ShmPageErrc code = ShmPageErrc::kOk;
  uint64_t expected = 0;
  uint64_t actual = 0;
};

const char* ShmPageErrorName(ShmPageErrc code);
// Human-readable diagnostic (e.g. "bad_page_bytes expected=4224 actual=4160")
// written to `buffer` and NUL-terminated when capacity > 0. Returns the number
// of characters written, excluding the terminator.
std::size_t FormatShmPageError(const ShmPageError& error, char* buffer,
                               std::size_t capacity);
bool IsShmPageSpecValid(const ShmPageSpec& spec);
void InitShmSuperHeader(ShmSuperHeader* header, const ShmPageSpec& spec);
ShmPageErrc ValidateShmSuperHeader(const void* base, std::size_t mapped_bytes,
                                   const ShmPageSpec& expected,
                                   ShmPageError* error = nullptr);

}  // namespace thyme
