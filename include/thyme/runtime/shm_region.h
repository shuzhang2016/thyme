#pragma once
#include "thyme/runtime/shm_page.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <new>
#include <utility>

namespace thyme {
class ShmRegion {
 public:
  ShmRegion() = default;
  ~ShmRegion();
  ShmRegion(ShmRegion&& o) noexcept;
  ShmRegion& operator=(ShmRegion&& o) noexcept;
  ShmRegion(const ShmRegion&) = delete;
  ShmRegion& operator=(const ShmRegion&) = delete;

  // Raw-bytes region: creates `bytes` (page-rounded up) of zero-filled shared
  // memory. The mapped memory holds NO C++ object; callers that need a live
  // object should prefer Create<T> below (or construct into data() themselves
  // and take on the object-lifetime bookkeeping).
  //
  // OPS-02: if the name already exists, the segment is reclaimed only when
  // its liveness is proved absent (no live fd/mapping; see shm_lifecycle.h).
  // A live or unprovable segment makes Create fail without touching it.
  bool Create(const char* name, std::size_t bytes);

  // Construct-in-place: like Create(), and additionally constructs a T at
  // data() via placement new once the region is mapped, returning the
  // constructed object (nullptr on failure, including the raw-Create
  // failure paths).
  //
  // `bytes` defaults to sizeof(T). Requirements:
  //  - alignof(T) <= page size: mmap hands back page-aligned addresses, so an
  //    over-aligned T cannot be placed at data(). Violations are rejected
  //    (with a diagnostic) and reported as nullptr.
  //  - bytes >= sizeof(T): the object must fit inside the mapped region.
  //
  // On failure nothing is left behind: the region is unmapped and the shm
  // object unlinked, matching Create()'s no-half-created-object guarantee.
  //
  // Lifetime: the constructed T is owned by the caller. Call obj->~T() before
  // Unmap()/destruction whenever the destructor has side effects (releasing
  // resources, notifying peer processes, ...). If T is trivially destructible
  // -- the typical SHM case: POD-like structs and structs of lock-free
  // atomics whose teardown is just unmapping -- Unmap() without an explicit
  // destructor call is fine.
  template <typename T, typename... Args>
  T* Create(const char* name, std::size_t bytes = sizeof(T), Args&&... args);

  bool OpenRetry(const char* name, std::size_t bytes, uint64_t timeout_ns,
                 bool read_only = false);

  // Described regions validate a common super-header before exposing data().
  // OpenDescribed maps the 64 B header first, validates
  // magic/abi/layout_hash/page_bytes/feature_bits/reserved (retrying
  // kNotReady until timeout_ns), then remaps the full page and validates
  // again so a producer that recreates the object cannot slip a truncated or
  // foreign page past the attach. On failure data() is null and `error`
  // carries the first mismatch with expected/actual values. The raw APIs
  // above remain available for legacy and low-level callers.
  bool CreateDescribed(const char* name, const ShmPageSpec& spec);
  bool OpenDescribed(const char* name, const ShmPageSpec& spec,
                     uint64_t timeout_ns, bool read_only = false,
                     ShmPageError* error = nullptr);

  void* data() const { return addr_; }
  std::size_t size() const { return size_; }
  // Unmaps the region and closes its fd. Does NOT destroy any object that was
  // constructed into the region; see Create<T> for the destructor contract.
  void Unmap();
  static void UnlinkName(const char* name);

 private:
  void Reset();
  static std::size_t PageSize();
  static std::size_t RoundUpPage(std::size_t bytes);
  bool MapFd(int fd, std::size_t bytes, bool read_only);
  void* addr_{nullptr};
  std::size_t size_{0};
  int fd_{-1};
};

template <typename T, typename... Args>
T* ShmRegion::Create(const char* name, std::size_t bytes, Args&&... args) {
  if (!Create(name, bytes)) return nullptr;
  const std::size_t page = PageSize();
  if (alignof(T) > page || bytes < sizeof(T)) {
    std::fprintf(stderr,
                 "ShmRegion: cannot construct %zu-byte T (alignof %zu) in "
                 "%zu-byte region, page size %zu\n",
                 sizeof(T), alignof(T), bytes, page);
    Unmap();
    UnlinkName(name);
    return nullptr;
  }
  // The caller owns the mapped region and invokes Destroy<T>() before Unmap();
  // this placement new does not allocate or transfer a heap resource.
  // NOLINTNEXTLINE(cppcoreguidelines-owning-memory): object lives in SHM.
  return new (addr_) T(std::forward<Args>(args)...);
}

}  // namespace thyme
