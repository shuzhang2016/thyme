#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace thyme {
namespace detail {

// Seqlock payload bytes are touched by a consumer while the producer may
// already be overwriting the slot it is copying. Under the C++ memory model
// that is a data race (UB) even though the reader discards the torn copy after
// re-checking `seq`. C++17 has no std::atomic_ref, so the copies below use the
// compiler's relaxed atomic load/store builtins instead of memcpy; `relaxed`
// is sufficient because the seqlock's release-store / acquire-load on `seq`
// already orders the payload, the reader validates `seq` around the copy, and
// the bytes are only ever observed as an all-or-nothing snapshot.
//
// Both helpers copy exactly `n` bytes and never read or write past the
// caller's ranges. Only the side facing concurrent access needs atomics:
// CopyOut's source may be rewritten by the producer, CopyIn's destination is
// the producer's slot. The peer side is caller-private and uses plain
// stores/loads, so it may be arbitrarily aligned. The atomic side is walked
// as 8-, 4- or 2-byte words depending on its own alignment, with a byte-wise
// tail. Requiring both pointers to share an alignment (the earlier version)
// made every `CopyPayloadTo` into an unaligned user buffer take the ~30x
// slower byte-wise path. The byte-wise tail is also the fallback for
// compilers without the GCC/Clang atomic builtins.

#if defined(__GNUC__) || defined(__clang__)

// The payload has no declared type (raw bytes in shared memory); may_alias
// lets the word accesses touch them without tripping strict aliasing.
typedef std::uint64_t AtomicWord8 __attribute__((may_alias));
typedef std::uint32_t AtomicWord4 __attribute__((may_alias));
typedef std::uint16_t AtomicWord2 __attribute__((may_alias));

inline void AtomicPayloadCopyOut(void* dst, const void* src, size_t n) {
  auto* d = static_cast<unsigned char*>(dst);
  const auto* s = static_cast<const unsigned char*>(src);
  const std::uintptr_t sa = reinterpret_cast<std::uintptr_t>(s);
  size_t i = 0;
  if ((sa & 7u) == 0) {
    for (; i + 8 <= n; i += 8) {
      const AtomicWord8 w = __atomic_load_n(
          reinterpret_cast<const AtomicWord8*>(s + i), __ATOMIC_RELAXED);
      std::memcpy(d + i, &w, sizeof(w));
    }
  } else if ((sa & 3u) == 0) {
    for (; i + 4 <= n; i += 4) {
      const AtomicWord4 w = __atomic_load_n(
          reinterpret_cast<const AtomicWord4*>(s + i), __ATOMIC_RELAXED);
      std::memcpy(d + i, &w, sizeof(w));
    }
  } else if ((sa & 1u) == 0) {
    for (; i + 2 <= n; i += 2) {
      const AtomicWord2 w = __atomic_load_n(
          reinterpret_cast<const AtomicWord2*>(s + i), __ATOMIC_RELAXED);
      std::memcpy(d + i, &w, sizeof(w));
    }
  }
  for (; i < n; ++i) d[i] = __atomic_load_n(s + i, __ATOMIC_RELAXED);
}

inline void AtomicPayloadCopyIn(void* dst, const void* src, size_t n) {
  auto* d = static_cast<unsigned char*>(dst);
  const auto* s = static_cast<const unsigned char*>(src);
  const std::uintptr_t da = reinterpret_cast<std::uintptr_t>(d);
  size_t i = 0;
  if ((da & 7u) == 0) {
    for (; i + 8 <= n; i += 8) {
      AtomicWord8 w{};
      std::memcpy(&w, s + i, sizeof(w));
      __atomic_store_n(reinterpret_cast<AtomicWord8*>(d + i), w,
                       __ATOMIC_RELAXED);
    }
  } else if ((da & 3u) == 0) {
    for (; i + 4 <= n; i += 4) {
      AtomicWord4 w{};
      std::memcpy(&w, s + i, sizeof(w));
      __atomic_store_n(reinterpret_cast<AtomicWord4*>(d + i), w,
                       __ATOMIC_RELAXED);
    }
  } else if ((da & 1u) == 0) {
    for (; i + 2 <= n; i += 2) {
      AtomicWord2 w{};
      std::memcpy(&w, s + i, sizeof(w));
      __atomic_store_n(reinterpret_cast<AtomicWord2*>(d + i), w,
                       __ATOMIC_RELAXED);
    }
  }
  for (; i < n; ++i) __atomic_store_n(d + i, s[i], __ATOMIC_RELAXED);
}

#else  // !(defined(__GNUC__) || defined(__clang__))

inline void AtomicPayloadCopyOut(void* dst, const void* src, size_t n) {
  std::memcpy(dst, src, n);
}

inline void AtomicPayloadCopyIn(void* dst, const void* src, size_t n) {
  std::memcpy(dst, src, n);
}

#endif

}  // namespace detail
}  // namespace thyme
