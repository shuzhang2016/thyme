// test_pinned_slot_view_lifetime.cpp — View is move-only (compile-time),
// CopyTo() produces a correct storable Snapshot, move invalidates the source,
// and — in the -UNDEBUG variant — destroying a ReadFrame view after its slot
// was rewritten aborts (store-and-use-async misuse detection).
#include "thyme/check.h"
#include "thyme/runtime/pinned_slot_store.h"
#include "thyme/runtime/shm_region.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <type_traits>
#include <sys/wait.h>
#include <unistd.h>

using namespace thyme;

static_assert(!std::is_copy_constructible<PinnedSlotReader::View>::value,
              "View must not be copy-constructible");
static_assert(!std::is_copy_assignable<PinnedSlotReader::View>::value,
              "View must not be copy-assignable");
static_assert(std::is_move_constructible<PinnedSlotReader::View>::value,
              "View must be move-constructible");
static_assert(
    std::is_copy_constructible<PinnedSlotReader::View::Snapshot>::value,
    "Snapshot must be copyable");
static_assert(
    std::is_copy_assignable<PinnedSlotReader::View::Snapshot>::value,
    "Snapshot must be copy-assignable");

namespace {

void WriteFrame(PinnedSlotWriter& w, uint8_t tag) {
  PinnedSlotWriter::BeginView bv = w.Begin(128);
  CHECK(bv.payload != nullptr);
  std::memset(bv.payload, tag, 128);
  w.Commit();
}

}  // namespace

int main() {
  char name[96];
  std::snprintf(name, sizeof(name), "/c9view_%d", static_cast<int>(getpid()));
  ShmRegion::UnlinkName(name);
  ShmRegion::UnlinkName(PinnedSlotWriter::LeaseShmName(name).c_str());

  PinnedSlotWriter w;
  CHECK(w.Create(name, 128, 4, 0));
  PinnedSlotReader r;
  CHECK(r.Open(name, 1000000000ULL));
  WriteFrame(w, 0x11);
  WriteFrame(w, 0x22);

  // CopyTo: metadata snapshot is faithful.
  {
    PinnedSlotReader::View v;
    CHECK(r.ReadFrame(1, v) == PinnedSlotReader::ReadResult::kOk);
    PinnedSlotReader::View::Snapshot snap;
    v.CopyTo(snap);
    CHECK(snap.data_len == 128);
    CHECK(snap.frame_index == 1);
    CHECK(snap.slot_seq == 2);
    CHECK(snap.mono_ns == v.mono_ns);
    CHECK(snap.crc32 == v.crc32);
    CHECK(snap.crc_ok);
    CHECK(snap.payload[0] == 0x22);
    const PinnedSlotReader::View::Snapshot snap2 = snap;  // Snapshot is copyable
    CHECK(snap2.frame_index == 1);
  }

  // Move: source becomes disarmed/empty; destination keeps the content.
  {
    PinnedSlotReader::View v;
    CHECK(r.ReadFrame(1, v) == PinnedSlotReader::ReadResult::kOk);
    PinnedSlotReader::View mv = std::move(v);
    CHECK(mv.data_len == 128);
    CHECK(mv.frame_index == 1);
    CHECK(mv.payload[0] == 0x22);
  }  // mv destructs here; slot untouched -> no debug abort

#if !defined(NDEBUG) && defined(RUN_DEBUG_ABORT)
  // Debug misuse detection: a ReadFrame view destroyed after its slot was
  // rewritten must abort (fork so the parent can assert the signal).
  {
    // The parent first disposes of its own view legally (destructs right after
    // the move, slot unchanged, safe).
    {
      PinnedSlotReader::View v;
      CHECK(r.ReadFrame(1, v) == PinnedSlotReader::ReadResult::kOk);
      PinnedSlotReader::View::Snapshot snap;
      v.CopyTo(snap);
      PinnedSlotReader::View mv = std::move(v);
      (void)mv;  // destructs here: the slot seq is unchanged, so no abort
    }
    const pid_t pid = fork();
    CHECK(pid >= 0);
    if (pid == 0) {
      PinnedSlotReader::View bad;
      CHECK(r.ReadFrame(1, bad) == PinnedSlotReader::ReadResult::kOk);
      WriteFrame(w, 0x33);  // frame 1's slot is overwritten (frames 2..5)
      WriteFrame(w, 0x44);
      WriteFrame(w, 0x55);
      WriteFrame(w, 0x66);
      return 0;  // main returns: bad destructs during the return sequence -> abort
    }
    int st = 0;
    CHECK(waitpid(pid, &st, 0) == pid);
    CHECK(WIFSIGNALED(st) && WTERMSIG(st) == SIGABRT);
  }
#endif

  r.Close();
  w.Close();
  ShmRegion::UnlinkName(name);
  ShmRegion::UnlinkName(PinnedSlotWriter::LeaseShmName(name).c_str());
  return 0;
}
