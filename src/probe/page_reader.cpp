// page_reader.cpp — read-only classification and newest-sample reads for the
// thyme SHM containers. Every access is a load from a PROT_READ mapping; no
// function here can mutate a page.

#include "page_reader.h"

#include "thyme/app/mp_control.h"
#include "thyme/runtime/pinned_slot_store.h"
#include "thyme/runtime/latest_slot.h"
#include "thyme/runtime/record_mirror.h"
#include "thyme/runtime/record_mirror_ring.h"
#include "thyme/runtime/shared_health.h"
#include "thyme/runtime/shm_spmc_ring.h"
#include "thyme/runtime/shm_spsc_ring.h"

#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace thyme {
namespace probe {
namespace detail {
namespace {

// RT-09: this is a READ-ONLY diagnostic path, so it cannot carry the reader
// retry counters the runtime containers expose (nothing here may write to a
// page, and a probe has no per-reader state). It uses the same bounded budget
// as the runtime loops and surfaces a torn/unstable read as RawSample::torn
// instead; the authoritative counters live in
// LatestSlotReadStats/SpmcReader/PinnedSlotReader/RecordMirror* (see
// docs/reference/cacheline-audit.md).
constexpr int kProbeReadRetries = 16;

// Type-independent header offsets of the two ring layouts, derived from the
// real templates so a layout change cannot silently desynchronise the probe.
struct ProbeDummy8 {
  uint8_t b[8];
};
using SpscLayout = ShmSpscRing<ProbeDummy8, 2>::Layout;
using SpmcLayout = ShmSpmcRing<ProbeDummy8, 2>::Layout;

constexpr uint64_t kSpscCap = offsetof(SpscLayout, cap);
constexpr uint64_t kSpscHead = offsetof(SpscLayout, head);
constexpr uint64_t kSpscTail = offsetof(SpscLayout, tail);
constexpr uint64_t kSpscEpoch = offsetof(SpscLayout, epoch);
constexpr uint64_t kSpscPushFail = offsetof(SpscLayout, push_fail);
constexpr uint64_t kSpscSlots = offsetof(SpscLayout, slots);

constexpr uint64_t kSpmcCap = offsetof(SpmcLayout, cap);
constexpr uint64_t kSpmcHead = offsetof(SpmcLayout, head);
constexpr uint64_t kSpmcEpoch = offsetof(SpmcLayout, epoch);
constexpr uint64_t kSpmcReaderDrops = offsetof(SpmcLayout, reader_drops);
constexpr uint64_t kSpmcReaderDropSlots = offsetof(SpmcLayout, reader_drop_slots);
constexpr uint64_t kSpmcSlots = offsetof(SpmcLayout, slots);

// LatestSlot page geometry (ARCH-11 v3): the header + fixed geometry block
// carry the cell stride; the probe derives the control offsets from it and
// never guesses 2*RoundUp64(sizeof(T)) + 64 or a bare v2 body.
constexpr uint64_t LatestIndexOffset(uint64_t slots_offset, uint64_t stride) {
  return slots_offset + 2u * stride;
}
constexpr uint64_t LatestSeqOffset(uint64_t slots_offset, uint64_t stride) {
  return slots_offset + 2u * stride + 8u;
}

uint32_t RoundUp64(uint32_t n) { return (n + 63u) & ~63u; }

uint64_t LoadU64(const uint8_t* p) {
  return reinterpret_cast<const std::atomic<uint64_t>*>(p)->load(
      std::memory_order_acquire);
}
uint32_t LoadU32(const uint8_t* p) {
  return reinterpret_cast<const std::atomic<uint32_t>*>(p)->load(
      std::memory_order_acquire);
}

Kind KindFromMagic(uint64_t magic) {
  if (magic == PinnedSlotPageHeader::kMagic) return Kind::kPinnedSlot;
  if (magic == kShmSpscRingMagic) return Kind::kSpscRing;
  if (magic == kShmSpmcRingMagic) return Kind::kSpmcRing;
  if (magic == SharedHealthPage::kMagic) return Kind::kSharedHealth;
  if (magic == app::MpControl::kMagic) return Kind::kMpControl;
  if (magic == kLatestSlotPageMagic) return Kind::kLatestSlot;
  if (magic == RecordMirrorRingPage::kMagic) return Kind::kRecordMirrorRing;
  if (magic == RecordMirrorPage::kMagic) return Kind::kRecordMirror;
  return Kind::kUnknown;
}

std::string PageErrorNote(const ShmPageError& error) {
  char buffer[160];
  const std::size_t written =
      FormatShmPageError(error, buffer, sizeof(buffer));
  return std::string(buffer, written);
}

void CopyHeader(const MappedPage& page, uint64_t offset, uint32_t cap,
                bool frame_shaped, RawSample* out) {
  if (!frame_shaped || cap < sizeof(FrameHeader)) return;
  FrameHeader h{};
  std::memcpy(&h, page.base() + offset, sizeof(FrameHeader));
  if (!FrameHeaderMetadataOk(h, cap) || h.mono_ns == 0) return;
  out->has_stamp = true;
  out->producer_ns = h.mono_ns;
}

Status BuildPinnedSlot(const std::string& name, const MappedPage& page,
                     const ManifestEntry* entry, Geometry* g) {
  (void)entry;
  g->kind = Kind::kPinnedSlot;
  if (page.size() < sizeof(PinnedSlotPageHeader)) {
    g->compatible = false;
    g->note = "region smaller than PinnedSlotPageHeader";
    return Status::kBadGeometry;
  }
  const auto* h =
      reinterpret_cast<const PinnedSlotPageHeader*>(page.base());
  g->abi_version = h->super.abi_version.load(std::memory_order_acquire);
  g->slot_bytes = h->slot_bytes.load(std::memory_order_acquire);
  g->slots = h->slot_count.load(std::memory_order_acquire);
  g->stride = h->slot_stride.load(std::memory_order_acquire);
  g->head = h->head.load(std::memory_order_acquire);
  g->epoch = h->epoch.load(std::memory_order_acquire);
  g->drop = h->dropped_oversize.load(std::memory_order_acquire);
  const uint32_t meta = h->meta_blob_bytes.load(std::memory_order_acquire);
  g->source = "page";
  if (g->abi_version != PinnedSlotPageHeader::kVersion) {
    g->compatible = false;
    g->note = "unsupported PinnedSlotStore version";
    return Status::kBadGeometry;
  }
  g->slots_offset = PinnedSlotWriter::SlotsOffset(meta);
  if (g->slots == 0 || g->slot_bytes == 0 ||
      g->stride != PinnedSlotWriter::SlotStride(g->slot_bytes)) {
    g->compatible = false;
    g->note = "PinnedSlot geometry is inconsistent";
    return Status::kBadGeometry;
  }
  const std::size_t need = PinnedSlotWriter::RegionBytes(
      g->slot_bytes, g->slots, meta);
  if (page.size() < need) {
    g->compatible = false;
    g->note = "region smaller than the announced PinnedSlot geometry";
    return Status::kBadGeometry;
  }
  // Lease counters live in "<name>.lease" (BF-01). Missing lease table = no
  // pin/skip/steal numbers, not an error.
  MappedPage lease;
  if (lease.Open(name + ".lease") == Status::kOk &&
      lease.size() >= sizeof(PinnedSlotLeaseHeader)) {
    const auto* lh =
        reinterpret_cast<const PinnedSlotLeaseHeader*>(lease.base());
    if (lh->super.magic.load(std::memory_order_acquire) ==
            kPinnedSlotLeaseMagic &&
        lh->super.abi_version.load(std::memory_order_acquire) ==
            kPinnedSlotLeaseVersion &&
        lh->slot_count.load(std::memory_order_acquire) == g->slots) {
      g->skip = lh->pinned_skip.load(std::memory_order_acquire);
      g->steal = lh->lease_steal.load(std::memory_order_acquire);
    }
  }
  return Status::kOk;
}

Status BuildSpsc(const MappedPage& page, const ManifestEntry* entry,
                 Geometry* g) {
  g->kind = Kind::kSpscRing;
  if (page.size() < kSpscSlots) {
    g->compatible = false;
    g->note = "region smaller than the ShmSpscRing header";
    return Status::kBadGeometry;
  }
  g->abi_version = LoadU32(page.base() + 8);
  g->slots = LoadU32(page.base() + kSpscCap);
  g->head = LoadU32(page.base() + kSpscHead);
  g->tail = LoadU32(page.base() + kSpscTail);
  g->epoch = LoadU32(page.base() + kSpscEpoch);
  g->drop = LoadU32(page.base() + kSpscPushFail);
  g->slots_offset = kSpscSlots;
  g->source = entry != nullptr && entry->slot_bytes != 0 ? "page+manifest"
                                                         : "page";
  if (g->abi_version != kShmSpscRingVersion) {
    g->compatible = false;
    g->note = "unsupported ShmSpscRing version";
    return Status::kBadGeometry;
  }
  if (g->slots < 2 || (g->slots & (g->slots - 1u)) != 0) {
    g->compatible = false;
    g->note = "ShmSpscRing capacity is not a power of two";
    return Status::kBadGeometry;
  }
  if (entry != nullptr && entry->slot_bytes != 0) {
    g->slot_bytes = entry->slot_bytes;
    g->stride = RoundUp64(entry->slot_bytes);
    g->frame_shaped = entry->frame_shaped;
    if (g->slots_offset +
            static_cast<uint64_t>(g->slots) * g->stride >
        page.size()) {
      g->compatible = false;
      g->note = "manifest slot geometry exceeds the region";
      return Status::kBadGeometry;
    }
  } else {
    g->note =
        "slot stride unknown (ShmSpscRing does not record sizeof(T)); add a "
        "manifest entry for bw/delay/echo";
  }
  return Status::kOk;
}

Status BuildSpmc(const MappedPage& page, const ManifestEntry* entry,
                 Geometry* g) {
  g->kind = Kind::kSpmcRing;
  if (page.size() < kSpmcSlots) {
    g->compatible = false;
    g->note = "region smaller than the ShmSpmcRing header";
    return Status::kBadGeometry;
  }
  g->abi_version = LoadU32(page.base() + 8);
  g->slots = LoadU32(page.base() + kSpmcCap);
  g->head = LoadU64(page.base() + kSpmcHead);
  g->epoch = LoadU32(page.base() + kSpmcEpoch);
  g->drop = LoadU64(page.base() + kSpmcReaderDrops);
  g->slots_offset = kSpmcSlots;
  g->source = entry != nullptr && entry->slot_bytes != 0 ? "page+manifest"
                                                         : "page";
  if (g->abi_version != kShmSpmcRingVersion) {
    g->compatible = false;
    g->note = "unsupported ShmSpmcRing version";
    return Status::kBadGeometry;
  }
  if (g->slots < 2 || (g->slots & (g->slots - 1u)) != 0) {
    g->compatible = false;
    g->note = "ShmSpmcRing capacity is not a power of two";
    return Status::kBadGeometry;
  }
  if (entry != nullptr && entry->slot_bytes != 0) {
    g->slot_bytes = entry->slot_bytes;
    g->stride = RoundUp64(8u + entry->slot_bytes);
    g->frame_shaped = entry->frame_shaped;
    if (g->slots_offset +
            static_cast<uint64_t>(g->slots) * g->stride >
        page.size()) {
      g->compatible = false;
      g->note = "manifest slot geometry exceeds the region";
      return Status::kBadGeometry;
    }
  } else {
    g->note =
        "slot stride unknown (ShmSpmcRing does not record sizeof(T)); add a "
        "manifest entry for bw/delay/echo";
  }
  g->reader_drop_slots.reserve(kAbiMaxSpmcReaders);
  for (uint32_t i = 0; i < kAbiMaxSpmcReaders; ++i) {
    g->reader_drop_slots.push_back(
        LoadU64(page.base() + kSpmcReaderDropSlots + i * sizeof(uint64_t)));
  }
  return Status::kOk;
}

Status BuildLatest(const MappedPage& page, const ManifestEntry* entry,
                   Geometry* g) {
  using namespace latest_slot_page_layout;
  g->kind = Kind::kLatestSlot;
  g->source = "page";
  // ARCH-11 v3: the page is self-describing. A magic-less v2 body (or a page
  // declared as latest through a manifest but still holding the old bare
  // body) is rejected, never decoded with guessed offsets.
  if (page.size() < kSlotsOffset) {
    g->compatible = false;
    g->note = "region smaller than the LatestSlot page header";
    return Status::kBadGeometry;
  }
  const auto* header =
      reinterpret_cast<const ShmSuperHeader*>(page.base());
  if (header->magic.load(std::memory_order_acquire) != kLatestSlotPageMagic) {
    g->compatible = false;
    g->note = "LatestSlot page rejected: legacy v2 body has no v3 header";
    return Status::kBadGeometry;
  }
  ShmPageError error;
  const ShmPageErrc code =
      ValidateLatestSlotPageGeometry(page.base(), page.size(), &error);
  if (code != ShmPageErrc::kOk) {
    g->compatible = false;
    g->note = "LatestSlot page rejected: " + PageErrorNote(error);
    return Status::kBadGeometry;
  }
  const auto* geometry = reinterpret_cast<const LatestSlotPageGeometry*>(
      page.base() + kGeometryOffset);
  const uint64_t payload_bytes =
      geometry->payload_bytes.load(std::memory_order_relaxed);
  const uint64_t stride =
      geometry->cell_stride.load(std::memory_order_relaxed);
  if (payload_bytes > UINT32_MAX || stride > UINT32_MAX) {
    g->compatible = false;
    g->note = "LatestSlot geometry exceeds the probe's 32-bit report fields";
    return Status::kBadGeometry;
  }
  g->abi_version = header->abi_version.load(std::memory_order_relaxed);
  g->slot_bytes = static_cast<uint32_t>(payload_bytes);
  g->stride = static_cast<uint32_t>(stride);
  g->slots =
      geometry->slot_count.load(std::memory_order_relaxed);
  g->slots_offset = kSlotsOffset;
  if (entry != nullptr) {
    g->source = entry->slot_bytes != 0 ? "page+manifest" : "page";
    g->frame_shaped = entry->frame_shaped;
    if (entry->slot_bytes != 0 && entry->slot_bytes != g->slot_bytes) {
      g->compatible = false;
      g->note = "manifest slot_bytes disagrees with the page geometry";
      return Status::kBadGeometry;
    }
  }
  const uint64_t seq = LoadU64(
      page.base() + LatestSeqOffset(g->slots_offset, g->stride));
  g->head = seq / 2u;
  return Status::kOk;
}

Status BuildSharedHealth(const MappedPage& page, Geometry* g) {
  g->kind = Kind::kSharedHealth;
  g->source = "page";
  if (page.size() < sizeof(SharedHealthPage)) {
    g->compatible = false;
    g->note = "region smaller than SharedHealthPage";
    return Status::kBadGeometry;
  }
  const auto* h = reinterpret_cast<const SharedHealthPage*>(page.base());
  g->abi_version = h->abi_version();
  g->session = h->session_id();
  g->has_session = true;
  g->workers = h->worker_count();
  g->slots = g->workers;
  g->slot_bytes = sizeof(HealthSlot);
  g->stride = sizeof(HealthSlot);
  g->slots_offset = offsetof(SharedHealthPage, slots);
  // ARCH-11: the probe validates the same super-header a role does. A stale
  // v2 page (or a corrupted layout/page size/feature word) is reported as
  // incompatible instead of being walked with this build's offsets.
  ShmPageError error;
  if (!h->Validate(&error)) {
    g->compatible = false;
    g->note = "SharedHealthPage rejected: " + PageErrorNote(error);
    return Status::kBadGeometry;
  }
  if (g->workers > SharedHealthPage::kMaxWorkers) {
    g->compatible = false;
    g->note = "SharedHealthPage worker count exceeds capacity";
    return Status::kBadGeometry;
  }
  g->drop = 0;
  for (uint32_t i = 0; i < g->workers; ++i) {
    g->drop += h->slots[i].drop_cnt.load(std::memory_order_acquire);
  }
  return Status::kOk;
}

Status BuildMpControl(const MappedPage& page, Geometry* g) {
  g->kind = Kind::kMpControl;
  g->source = "page";
  if (page.size() < sizeof(app::MpControl)) {
    g->compatible = false;
    g->note = "region smaller than MpControl";
    return Status::kBadGeometry;
  }
  const auto* c = reinterpret_cast<const app::MpControl*>(page.base());
  g->abi_version = c->abi_version();
  g->slots = static_cast<uint32_t>(app::MpControl::kMaxProcSlots);
  g->slot_bytes = sizeof(uint64_t);
  g->stride = sizeof(uint64_t);
  g->slots_offset = offsetof(app::MpControl, proc_loops);
  ShmPageError error;
  if (!c->Validate(&error)) {
    g->compatible = false;
    g->note = "MpControl rejected: " + PageErrorNote(error);
    return Status::kBadGeometry;
  }
  g->note = "supervisor control page; counters are not sampled";
  return Status::kOk;
}

Status BuildRecordRing(const MappedPage& page, Geometry* g) {
  g->kind = Kind::kRecordMirrorRing;
  g->source = "page";
  if (page.size() < sizeof(RecordMirrorRingPage)) {
    g->compatible = false;
    g->note = "region smaller than RecordMirrorRingPage";
    return Status::kBadGeometry;
  }
  const auto* p = reinterpret_cast<const RecordMirrorRingPage*>(page.base());
  g->abi_version = p->super.abi_version.load(std::memory_order_acquire);
  g->session = p->header.session_id.load(std::memory_order_acquire);
  g->has_session = true;
  g->drop = p->header.mirror_odd_skips.load(std::memory_order_acquire);
  uint32_t count = p->header.struct_count.load(std::memory_order_acquire);
  if (count > kRecordMirrorRingMaxStructs) count = kRecordMirrorRingMaxStructs;
  g->slots = count;
  for (uint32_t i = 0; i < count; ++i) {
    const ThymeStructDesc& d = p->struct_table[i];
    StructEntry e;
    e.id = d.struct_id;
    e.bytes = d.payload_size;
    e.name.assign(d.name, strnlen(d.name, sizeof(d.name)));
    g->structs.push_back(e);
  }
  if (g->abi_version != RecordMirrorRingPage::kVersion) {
    g->compatible = false;
    g->note = "unsupported RecordMirrorRingPage version";
    return Status::kBadGeometry;
  }
  g->note = "registry summary only; per-struct rings are not sampled";
  return Status::kOk;
}

Status BuildRecordMirror(const MappedPage& page, Geometry* g) {
  g->kind = Kind::kRecordMirror;
  g->source = "page";
  if (page.size() < sizeof(RecordMirrorPage)) {
    g->compatible = false;
    g->note = "region smaller than RecordMirrorPage";
    return Status::kBadGeometry;
  }
  const auto* p = reinterpret_cast<const RecordMirrorPage*>(page.base());
  g->abi_version = p->super.abi_version.load(std::memory_order_acquire);
  g->session = p->header.session_id.load(std::memory_order_acquire);
  g->has_session = true;
  g->drop = p->header.mirror_drop_cnt.load(std::memory_order_acquire);
  uint32_t count = p->header.slot_count.load(std::memory_order_acquire);
  if (count > kRecordMirrorMaxSlots) count = kRecordMirrorMaxSlots;
  g->slots = count;
  for (uint32_t i = 0; i < count; ++i) {
    const ThymeStructDesc& d = p->struct_table[i];
    StructEntry e;
    e.id = d.struct_id;
    e.bytes = d.payload_size;
    e.name.assign(d.name, strnlen(d.name, sizeof(d.name)));
    g->structs.push_back(e);
  }
  if (g->abi_version != RecordMirrorPage::kVersion) {
    g->compatible = false;
    g->note = "unsupported RecordMirrorPage version";
    return Status::kBadGeometry;
  }
  return Status::kOk;
}

}  // namespace

MappedPage::~MappedPage() { Close(); }

Status MappedPage::Open(const std::string& shm_name) {
  Close();
  const char* name = shm_name.c_str();
  const int fd = shm_open(name, O_RDONLY, 0600);
  if (fd < 0) {
    return (errno == EACCES || errno == EPERM) ? Status::kNotReadable
                                               : Status::kNotFound;
  }
  struct stat st {};
  if (fstat(fd, &st) != 0 || st.st_size <= 0) {
    close(fd);
    return Status::kBadGeometry;
  }
  const auto size = static_cast<uint64_t>(st.st_size);
  void* p = mmap(nullptr, static_cast<std::size_t>(size), PROT_READ,
                 MAP_SHARED, fd, 0);
  if (p == MAP_FAILED) {
    close(fd);
    return Status::kNotReadable;
  }
  // A producer may recreate the object between fstat and mmap (the suite does
  // this under -j); mapping a shorter inode would SIGBUS on the first access
  // past EOF. Re-check and retry once, then give up as "vanished" instead of
  // crashing the inspection tool.
  struct stat st2 {};
  if (fstat(fd, &st2) != 0 || static_cast<uint64_t>(st2.st_size) != size) {
    munmap(p, static_cast<std::size_t>(size));
    close(fd);
    return Status::kNotFound;
  }
  fd_ = fd;
  base_ = static_cast<uint8_t*>(p);
  size_ = size;
  return Status::kOk;
}

void MappedPage::Close() {
  if (base_ != nullptr) {
    munmap(base_, static_cast<std::size_t>(size_));
    base_ = nullptr;
  }
  size_ = 0;
  if (fd_ != -1) {
    close(fd_);
    fd_ = -1;
  }
}

Status OpenGeometry(const std::string& name, const ManifestEntry* entry,
                    MappedPage* page, Geometry* geometry) {
  *geometry = Geometry{};
  const Status st = page->Open(name);
  if (st != Status::kOk) return st;
  if (page->size() < 8) {
    geometry->compatible = false;
    geometry->note = "region too small to hold a header";
    return Status::kBadGeometry;
  }
  const Kind magic_kind = KindFromMagic(LoadU64(page->base()));
  geometry->region_bytes = page->size();
  const Kind declared = entry != nullptr ? entry->kind : Kind::kUnknown;
  if (magic_kind == Kind::kUnknown && declared == Kind::kUnknown) {
    return Status::kNotFound;  // not a thyme page
  }
  if (magic_kind != Kind::kUnknown && declared != Kind::kUnknown &&
      magic_kind != declared) {
    geometry->kind = declared;
    geometry->compatible = false;
    geometry->note = "manifest kind disagrees with the page magic";
    return Status::kBadGeometry;
  }
  const Kind kind = magic_kind != Kind::kUnknown ? magic_kind : declared;
  switch (kind) {
    case Kind::kPinnedSlot:
      return BuildPinnedSlot(name, *page, entry, geometry);
    case Kind::kSpscRing:
      return BuildSpsc(*page, entry, geometry);
    case Kind::kSpmcRing:
      return BuildSpmc(*page, entry, geometry);
    case Kind::kLatestSlot:
      return BuildLatest(*page, entry, geometry);
    case Kind::kSharedHealth:
      return BuildSharedHealth(*page, geometry);
    case Kind::kMpControl:
      return BuildMpControl(*page, geometry);
    case Kind::kRecordMirrorRing:
      return BuildRecordRing(*page, geometry);
    case Kind::kRecordMirror:
      return BuildRecordMirror(*page, geometry);
    case Kind::kUnknown:
      break;
  }
  return Status::kNotFound;
}

bool StreamKind(Kind kind) {
  return kind == Kind::kLatestSlot || kind == Kind::kSpscRing ||
         kind == Kind::kSpmcRing || kind == Kind::kPinnedSlot;
}

bool ReadNewest(const MappedPage& page, const Geometry& g, RawSample* out) {
  *out = RawSample{};
  if (!page.valid() || !g.compatible) return false;
  switch (g.kind) {
    case Kind::kLatestSlot: {
      const uint64_t stride = g.stride;
      const uint64_t slots_offset = g.slots_offset;
      if (stride == 0 || slots_offset == 0 ||
          page.size() < LatestSeqOffset(slots_offset, stride) + 8u) {
        return false;
      }
      const auto* seq = reinterpret_cast<const std::atomic<uint64_t>*>(
          page.base() + LatestSeqOffset(slots_offset, stride));
      const auto* idx = reinterpret_cast<const std::atomic<uint32_t>*>(
          page.base() + LatestIndexOffset(slots_offset, stride));
      for (int attempt = 0; attempt < kProbeReadRetries; ++attempt) {
        const uint64_t s0 = seq->load(std::memory_order_acquire);
        if (s0 == 0) return false;  // never written
        if ((s0 & 1u) != 0) continue;
        const uint64_t off =
            slots_offset +
            static_cast<uint64_t>(idx->load(std::memory_order_acquire) & 1u) *
                stride;
        RawSample s;
        s.has = true;
        s.sequence = s0 / 2u;
        s.size = g.slot_bytes;
        s.payload_offset = off;
        s.payload_len = g.slot_bytes;
        CopyHeader(page, off, g.slot_bytes, g.frame_shaped, &s);
        const uint64_t s1 = seq->load(std::memory_order_acquire);
        if (s1 != s0) continue;
        *out = s;
        return true;
      }
      return false;
    }
    case Kind::kSpscRing: {
      if (g.slots == 0) return false;
      const uint64_t h0 = LoadU32(page.base() + kSpscHead);
      if (h0 == 0) return false;
      const uint64_t idx = h0 - 1;
      RawSample s;
      s.has = true;
      s.sequence = h0;
      s.size = g.slot_bytes;
      if (g.stride != 0 && g.slots_offset != 0) {
        const uint64_t off =
            g.slots_offset +
            static_cast<uint64_t>(static_cast<uint32_t>(idx) &
                                  (g.slots - 1u)) *
                g.stride;
        if (off + g.stride <= page.size()) {
          s.payload_offset = off;
          s.payload_len = g.slot_bytes;
          CopyHeader(page, off, g.slot_bytes, g.frame_shaped, &s);
        }
      }
      const uint64_t h1 = LoadU32(page.base() + kSpscHead);
      // SPSC has no per-slot seqlock. The newest slot is overwritten only
      // after `capacity` further publishes; flag that as torn.
      s.torn = (h1 - h0) >= g.slots;
      *out = s;
      return true;
    }
    case Kind::kSpmcRing: {
      if (g.slots == 0 || g.stride == 0 || !page.valid()) return false;
      const uint64_t h = LoadU64(page.base() + kSpmcHead);
      if (h == 0) return false;
      const uint64_t idx = h - 1;
      const uint64_t off =
          g.slots_offset +
          static_cast<uint64_t>(static_cast<uint32_t>(idx) & (g.slots - 1u)) *
              g.stride;
      if (off + g.stride > page.size()) return false;
      const auto* cell_seq =
          reinterpret_cast<const std::atomic<uint64_t>*>(page.base() + off);
      for (int attempt = 0; attempt < kProbeReadRetries; ++attempt) {
        const uint64_t s0 = cell_seq->load(std::memory_order_acquire);
        if (s0 != idx + 1u) {
          if (s0 == 0) continue;  // writer mid-commit
          return false;           // slot already replaced
        }
        RawSample s;
        s.has = true;
        s.sequence = h;
        s.size = g.slot_bytes;
        s.payload_offset = off + 8u;
        s.payload_len = g.slot_bytes;
        CopyHeader(page, off + 8u, g.slot_bytes, g.frame_shaped, &s);
        const uint64_t s1 = cell_seq->load(std::memory_order_acquire);
        if (s1 != s0) continue;
        *out = s;
        return true;
      }
      return false;
    }
    case Kind::kPinnedSlot: {
      const auto* hdr =
          reinterpret_cast<const PinnedSlotPageHeader*>(page.base());
      const uint64_t head = hdr->head.load(std::memory_order_acquire);
      if (head == 0) return false;
      const uint64_t idx = head - 1u;
      const uint64_t off =
          g.slots_offset +
          static_cast<uint64_t>(static_cast<uint32_t>(idx % g.slots)) *
              g.stride;
      if (off + g.stride > page.size()) return false;
      const auto* slot =
          reinterpret_cast<const PinnedSlotHeader*>(page.base() + off);
      const uint64_t s0 = slot->seq.load(std::memory_order_acquire);
      if (s0 == 0) return false;  // mid-commit / killed writer
      if (s0 != idx + 1u) return false;
      const uint32_t len = slot->data_len.load(std::memory_order_acquire);
      const uint64_t mono = slot->mono_ns.load(std::memory_order_acquire);
      if (len > g.slot_bytes) return false;
      const auto* footer = reinterpret_cast<const PinnedSlotFooter*>(
          page.base() + off + sizeof(PinnedSlotHeader) +
          PinnedSlotWriter::RoundUp64(g.slot_bytes));
      const uint64_t footer_magic =
          footer->magic.load(std::memory_order_acquire);
      const uint64_t footer_index =
          footer->frame_index.load(std::memory_order_acquire);
      const uint64_t s1 = slot->seq.load(std::memory_order_acquire);
      if (s1 != s0 || footer_magic != kPinnedSlotFooterMagic ||
          footer_index != idx) {
        return false;
      }
      RawSample s;
      s.has = true;
      s.sequence = head;
      s.size = len;
      s.producer_ns = mono;
      s.has_stamp = mono != 0;
      s.payload_offset = off + sizeof(PinnedSlotHeader);
      s.payload_len = len;
      *out = s;
      return true;
    }
    case Kind::kUnknown:
    case Kind::kSharedHealth:
    case Kind::kMpControl:
    case Kind::kRecordMirrorRing:
    case Kind::kRecordMirror:
      return false;
  }
  return false;
}

bool ReadFrameSize(const MappedPage& page, const Geometry& g,
                   uint64_t frame_index, uint32_t* size) {
  if (!page.valid() || g.kind != Kind::kPinnedSlot || g.slots == 0) return false;
  const auto* hdr =
      reinterpret_cast<const PinnedSlotPageHeader*>(page.base());
  const uint64_t head = hdr->head.load(std::memory_order_acquire);
  if (frame_index >= head) return false;
  if (head - frame_index > g.slots) return false;  // evicted
  const uint64_t off =
      g.slots_offset +
      static_cast<uint64_t>(static_cast<uint32_t>(frame_index % g.slots)) *
          g.stride;
  if (off + g.stride > page.size()) return false;
  const auto* slot =
      reinterpret_cast<const PinnedSlotHeader*>(page.base() + off);
  const uint64_t s0 = slot->seq.load(std::memory_order_acquire);
  if (s0 != frame_index + 1u) return false;
  const uint32_t len = slot->data_len.load(std::memory_order_acquire);
  if (len > g.slot_bytes) return false;
  const auto* footer = reinterpret_cast<const PinnedSlotFooter*>(
      page.base() + off + sizeof(PinnedSlotHeader) +
      PinnedSlotWriter::RoundUp64(g.slot_bytes));
  if (footer->magic.load(std::memory_order_acquire) != kPinnedSlotFooterMagic ||
      footer->frame_index.load(std::memory_order_acquire) != frame_index) {
    return false;
  }
  const uint64_t s1 = slot->seq.load(std::memory_order_acquire);
  if (s1 != s0) return false;
  *size = len;
  return true;
}

bool ReadStablePayload(const MappedPage& page, const Geometry& g, uint32_t cap,
                       uint8_t* dst, RawSample* out, uint32_t* copied) {
  for (int attempt = 0; attempt < kProbeReadRetries; ++attempt) {
    RawSample a;
    if (!ReadNewest(page, g, &a)) return false;
    if (!a.has || a.torn) continue;
    const uint32_t n = a.payload_len < cap ? a.payload_len : cap;
    if (n > 0) {
      std::memcpy(dst, page.base() + a.payload_offset, n);
    }
    RawSample b;
    if (!ReadNewest(page, g, &b)) continue;
    if (b.has && !b.torn && b.sequence == a.sequence &&
        b.payload_offset == a.payload_offset) {
      *out = a;
      *copied = n;
      return true;
    }
  }
  return false;
}

}  // namespace detail
}  // namespace probe
}  // namespace thyme
