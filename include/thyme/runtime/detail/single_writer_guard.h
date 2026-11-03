#pragma once

#ifndef NDEBUG

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <mutex>
#include <unordered_map>

namespace thyme {
namespace detail {

// Process-local debug bookkeeping for the single-writer in-place publish APIs.
// Keeping it outside the primitives preserves their SHM layout in every build.
// Completed sections stay as inactive entries so repeated Commit/Abort and
// calls without Begin are rejected; the next Begin reactivates the entry.
// Sequential handoff is valid. A process-local token never gets reused when a
// std::thread::id could be recycled. Writers in different processes require
// external single-producer arbitration; their registries are independent.
class SingleWriterGuard {
 public:
  static void Begin(const char* primitive, const void* page, const void* slot,
                    uint64_t publish_seq) {
    const uint64_t token = CurrentToken();
    Registry& registry = GetRegistry();
    std::lock_guard<std::mutex> lock(registry.mutex);
    const auto existing = registry.writers.find(page);
    if (existing != registry.writers.end() && existing->second.active) {
      const Entry& owner = existing->second;
      Fail("BeginWrite", primitive, page, slot, publish_seq, owner, token);
    }
    if (existing == registry.writers.end()) {
      registry.writers.emplace(
          page, Entry{primitive, slot, publish_seq, token, true});
    } else {
      existing->second = Entry{primitive, slot, publish_seq, token, true};
    }
  }

  static void Check(const char* operation, const char* primitive,
                    const void* page, const void* slot, uint64_t publish_seq) {
    const uint64_t token = CurrentToken();
    Registry& registry = GetRegistry();
    std::lock_guard<std::mutex> lock(registry.mutex);
    const auto existing = registry.writers.find(page);
    if (existing == registry.writers.end() || !existing->second.active) {
      const Entry missing{primitive, slot, publish_seq, 0, false};
      Fail(operation, primitive, page, slot, publish_seq, missing, token);
    }
    const Entry& owner = existing->second;
    if (owner.token != token || owner.slot != slot ||
        owner.publish_seq != publish_seq ||
        std::strcmp(owner.primitive, primitive) != 0) {
      Fail(operation, primitive, page, slot, publish_seq, owner, token);
    }
  }

  static void End(const char* operation, const char* primitive,
                  const void* page, const void* slot, uint64_t publish_seq) {
    const uint64_t token = CurrentToken();
    Registry& registry = GetRegistry();
    std::lock_guard<std::mutex> lock(registry.mutex);
    const auto existing = registry.writers.find(page);
    if (existing == registry.writers.end() || !existing->second.active) {
      const Entry missing{primitive, slot, publish_seq, 0, false};
      Fail(operation, primitive, page, slot, publish_seq, missing, token);
    }
    Entry& owner = existing->second;
    if (owner.token != token || owner.slot != slot ||
        owner.publish_seq != publish_seq ||
        std::strcmp(owner.primitive, primitive) != 0) {
      Fail(operation, primitive, page, slot, publish_seq, owner, token);
    }
    owner.active = false;
  }

 private:
  struct Entry {
    const char* primitive;
    const void* slot;
    uint64_t publish_seq;
    uint64_t token;
    bool active;
  };

  struct Registry {
    std::mutex mutex;
    std::unordered_map<const void*, Entry> writers;
  };

  static Registry& GetRegistry() {
    static Registry registry;
    return registry;
  }

  static uint64_t CurrentToken() {
    static std::atomic<uint64_t> next_token{1};
    thread_local const uint64_t token =
        next_token.fetch_add(1, std::memory_order_relaxed);
    return token;
  }

  [[noreturn]] static void Fail(const char* operation,
                                const char* attempted_primitive,
                                const void* attempted_page,
                                const void* attempted_slot,
                                uint64_t attempted_publish_seq,
                                const Entry& owner, uint64_t contender_token) {
    std::fprintf(stderr,
                 "thyme single-writer violation: operation=%s primitive=%s "
                 "page=%p slot=%p publish=%llu contender_token=%llu; "
                 "active_primitive=%s active_slot=%p active_publish=%llu "
                 "owner_token=%llu\n",
                 operation, attempted_primitive, attempted_page, attempted_slot,
                 static_cast<unsigned long long>(attempted_publish_seq),
                 static_cast<unsigned long long>(contender_token),
                 owner.primitive, owner.slot,
                 static_cast<unsigned long long>(owner.publish_seq),
                 static_cast<unsigned long long>(owner.token));
    std::fflush(stderr);
    std::abort();
  }
};

}  // namespace detail
}  // namespace thyme

#endif
