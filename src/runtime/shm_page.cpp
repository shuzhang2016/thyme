#include "thyme/runtime/shm_page.h"

#include <cstdio>

namespace thyme {
namespace {

ShmPageErrc Fail(ShmPageErrc code, uint64_t expected, uint64_t actual,
                 ShmPageError* error) {
  if (error != nullptr) *error = ShmPageError{code, expected, actual};
  return code;
}

}  // namespace

const char* ShmPageErrorName(ShmPageErrc code) {
  switch (code) {
    case ShmPageErrc::kOk: return "ok";
    case ShmPageErrc::kNotReady: return "not_ready";
    case ShmPageErrc::kTooSmall: return "too_small";
    case ShmPageErrc::kBadMagic: return "bad_magic";
    case ShmPageErrc::kBadAbiVersion: return "bad_abi_version";
    case ShmPageErrc::kBadLayoutHash: return "bad_layout_hash";
    case ShmPageErrc::kBadPageBytes: return "bad_page_bytes";
    case ShmPageErrc::kBadFeatureBits: return "bad_feature_bits";
    case ShmPageErrc::kBadReserved: return "bad_reserved";
    case ShmPageErrc::kBadGeometry: return "bad_geometry";
    case ShmPageErrc::kInvalidSpec: return "invalid_spec";
  }
  return "invalid_spec";
}

std::size_t FormatShmPageError(const ShmPageError& error, char* buffer,
                               std::size_t capacity) {
  if (buffer == nullptr || capacity == 0) return 0;
  const int written = std::snprintf(
      buffer, capacity, "%s expected=%llu actual=%llu",
      ShmPageErrorName(error.code),
      static_cast<unsigned long long>(error.expected),
      static_cast<unsigned long long>(error.actual));
  if (written <= 0) return 0;
  const auto size = static_cast<std::size_t>(written);
  return size < capacity ? size : capacity - 1;
}

bool IsShmPageSpecValid(const ShmPageSpec& spec) {
  return spec.magic != 0 && spec.abi_version != 0 && spec.layout_hash != 0 &&
         spec.page_bytes >= sizeof(ShmSuperHeader);
}

void InitShmSuperHeader(ShmSuperHeader* header, const ShmPageSpec& spec) {
  if (header == nullptr) return;
  header->magic.store(0, std::memory_order_relaxed);
  header->reserved0 = 0;
  for (uint64_t& value : header->reserved) value = 0;
  if (!IsShmPageSpecValid(spec)) {
    header->abi_version.store(0, std::memory_order_relaxed);
    header->layout_hash.store(0, std::memory_order_relaxed);
    header->page_bytes.store(0, std::memory_order_relaxed);
    header->feature_bits.store(0, std::memory_order_relaxed);
    return;
  }
  header->abi_version.store(spec.abi_version, std::memory_order_relaxed);
  header->layout_hash.store(spec.layout_hash, std::memory_order_relaxed);
  header->page_bytes.store(spec.page_bytes, std::memory_order_relaxed);
  header->feature_bits.store(spec.feature_bits, std::memory_order_relaxed);
  header->magic.store(spec.magic, std::memory_order_release);
}

ShmPageErrc ValidateShmSuperHeader(const void* base, std::size_t mapped_bytes,
                                   const ShmPageSpec& expected,
                                   ShmPageError* error) {
  if (error != nullptr) *error = ShmPageError{};
  if (!IsShmPageSpecValid(expected)) {
    return Fail(ShmPageErrc::kInvalidSpec, 0, 0, error);
  }
  if (base == nullptr || mapped_bytes < sizeof(ShmSuperHeader)) {
    return Fail(ShmPageErrc::kTooSmall, sizeof(ShmSuperHeader), mapped_bytes,
                error);
  }
  const auto* header = static_cast<const ShmSuperHeader*>(base);
  const uint64_t magic = header->magic.load(std::memory_order_acquire);
  if (magic == 0) {
    return Fail(ShmPageErrc::kNotReady, expected.magic, magic, error);
  }
  if (magic != expected.magic) {
    return Fail(ShmPageErrc::kBadMagic, expected.magic, magic, error);
  }
  const uint32_t version =
      header->abi_version.load(std::memory_order_relaxed);
  if (version != expected.abi_version) {
    return Fail(ShmPageErrc::kBadAbiVersion, expected.abi_version, version,
                error);
  }
  const uint64_t layout = header->layout_hash.load(std::memory_order_relaxed);
  if (layout != expected.layout_hash) {
    return Fail(ShmPageErrc::kBadLayoutHash, expected.layout_hash, layout,
                error);
  }
  const uint64_t bytes = header->page_bytes.load(std::memory_order_relaxed);
  if (bytes != expected.page_bytes) {
    return Fail(ShmPageErrc::kBadPageBytes, expected.page_bytes, bytes, error);
  }
  const uint64_t features =
      header->feature_bits.load(std::memory_order_relaxed);
  if (features != expected.feature_bits) {
    return Fail(ShmPageErrc::kBadFeatureBits, expected.feature_bits, features,
                error);
  }
  if (header->reserved0 != 0) {
    return Fail(ShmPageErrc::kBadReserved, 0, header->reserved0, error);
  }
  for (const uint64_t value : header->reserved) {
    if (value != 0) return Fail(ShmPageErrc::kBadReserved, 0, value, error);
  }
  return ShmPageErrc::kOk;
}

}  // namespace thyme
