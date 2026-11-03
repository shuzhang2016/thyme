#pragma once

#include <type_traits>
#include <utility>

namespace thyme {

// ARCH-12: RAII write section over the five in-place publishers
// (LatestSlot, ShmSpmcRing, ShmSpscRing, NotifPipe, PinnedLatestStore).
//
// Constructor = BeginWrite, commit() = CommitWrite, destructor = AbortWrite
// when the section is still open. Delivery semantics stay on the container
// (ADR 0014). This type only owns the failure path so an early return cannot
// leave seq_ odd, a free-list slot stranded, or write_idx_ stuck.
//
// AbortWrite has four classes; each container documents its own, summarized
// here so the names stay greppable in one place:
//
//   1. seqlock rollback (LatestSlot): close the odd seq_ without flipping
//      published_idx_. Readers keep the previous value.
//   2. head not advanced / no-op (ShmSpscRing): BeginWrite touched no shared
//      head, so AbortWrite is empty in release; the next BeginWrite reuses
//      the same slot.
//   3. slot allocated then sacrificed (ShmSpmcRing): seq=0 hole; the frame
//      that occupied the slot is dropped, never resurrected with torn bytes.
//   4. recycle unpublished slot (NotifPipe free-list, PinnedLatestStore
//      writer-bit): AbortWrite returns the slot to the free/unpinned set.
//
// BeginWrite/CommitWrite/AbortWrite remain the hooks (and the test API).
// First-party fill paths go through WriteSection so they never hand-write
// Abort. A null BeginWrite (full pipe / no free pin slot) is a failed
// publish: bool(*this) is false and the destructor is a no-op.
template <typename Container>
class WriteSection {
 public:
  using Slot = decltype(std::declval<Container&>().BeginWrite());

  explicit WriteSection(Container& container)
      : container_(&container), slot_(container.BeginWrite()) {}

  ~WriteSection() { AbortIfOpen(); }

  WriteSection(const WriteSection&) = delete;
  WriteSection& operator=(const WriteSection&) = delete;

  WriteSection(WriteSection&& other) noexcept
      : container_(other.container_),
        slot_(other.slot_),
        committed_(other.committed_) {
    other.container_ = nullptr;
    other.slot_ = Slot{};
    other.committed_ = true;
  }
  WriteSection& operator=(WriteSection&& other) noexcept {
    if (this != &other) {
      AbortIfOpen();
      container_ = other.container_;
      slot_ = other.slot_;
      committed_ = other.committed_;
      other.container_ = nullptr;
      other.slot_ = Slot{};
      other.committed_ = true;
    }
    return *this;
  }

  explicit operator bool() const { return slot_ != nullptr; }
  Slot get() const { return slot_; }
  std::remove_pointer_t<Slot>& operator*() const { return *slot_; }
  Slot operator->() const { return slot_; }

  void commit() {
    if (container_ == nullptr || slot_ == nullptr || committed_) return;
    container_->CommitWrite();
    committed_ = true;
  }

 private:
  void AbortIfOpen() {
    if (container_ == nullptr || slot_ == nullptr || committed_) return;
    container_->AbortWrite();
    committed_ = true;
  }

  Container* container_ = nullptr;
  Slot slot_{};
  bool committed_ = false;
};

}  // namespace thyme
