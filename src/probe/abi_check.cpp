// abi_check.cpp — OPS-06: compare /dev/shm pages against this build's ABI
// constants. Read-only like the rest of the probe: PROT_READ mappings and
// load-only reads, no unlink, no lease acquire, no cursor advance.

#include "thyme/probe/abi_matrix.h"
#include "thyme/probe/probe.h"

#include "page_reader.h"

#include "thyme/runtime/pinned_slot_store.h"

#include <algorithm>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace thyme {
namespace probe {
namespace {

void Record(AbiReport* report, AbiCheck check) {
  if (check.compatible) {
    ++report->compatible_count;
  } else {
    ++report->mismatch_count;
  }
  report->pages.push_back(std::move(check));
}

void CheckLeaseTable(const std::string& channel, AbiReport* report) {
  const std::string name = channel + ".lease";
  detail::MappedPage lease;
  if (lease.Open(name) != Status::kOk) {
    report->unverifiable.push_back(
        name + " (PinnedSlot lease table absent)");
    return;
  }
  if (lease.size() < sizeof(PinnedSlotLeaseHeader)) {
    report->unverifiable.push_back(
        name + " (region smaller than PinnedSlotLeaseHeader)");
    return;
  }
  const auto* header =
      reinterpret_cast<const PinnedSlotLeaseHeader*>(lease.base());
  if (header->super.magic.load(std::memory_order_acquire) !=
      kPinnedSlotLeaseMagic) {
    report->unverifiable.push_back(
        name + " (not a PinnedSlot lease table)");
    return;
  }
  const AbiConstant* row = FindAbiConstant("PinnedSlotLeaseHeader");
  if (row == nullptr) {
    report->unverifiable.push_back(name + " (no matrix row for lease table)");
    return;
  }
  AbiCheck check;
  check.name = name;
  check.kind = Kind::kPinnedSlot;
  check.object = row->object;
  check.expected = row->version;
  check.actual = header->super.abi_version.load(std::memory_order_acquire);
  check.checked = true;
  check.compatible = check.actual == check.expected;
  if (!check.compatible) {
    check.note = "lease table version differs from this build";
  }
  Record(report, std::move(check));
}

}  // namespace

Status ProbeApi::CheckAbi(AbiReport* out) {
  if (out == nullptr) return Status::kUsage;
  *out = AbiReport{};
  std::vector<RegionInfo> rows;
  const Status list_status = List(rows);
  if (list_status != Status::kOk) return list_status;

  for (const RegionInfo& region : rows) {
    const AbiConstant* row = PrimaryPageConstant(region.kind);
    if (row == nullptr || !row->in_page_version) {
      out->unverifiable.push_back(
          region.name + " (no matrix row for this page kind)");
      continue;
    }
    AbiCheck check;
    check.name = region.name;
    check.kind = region.kind;
    check.object = row->object;
    check.expected = row->version;
    check.actual = region.abi_version;
    check.checked = region.abi_version != 0;
    check.compatible = region.compatible && check.checked &&
                       check.actual == check.expected;
    check.note = region.note;
    if (!check.compatible && check.note.empty()) {
      check.note = check.actual == check.expected
                       ? "page rejected by this build"
                       : "page version differs from this build";
    }
    Record(out, std::move(check));

    if (region.kind == Kind::kPinnedSlot) {
      CheckLeaseTable(region.name, out);
    }
  }

  std::sort(out->pages.begin(), out->pages.end(),
            [](const AbiCheck& a, const AbiCheck& b) { return a.name < b.name; });
  std::sort(out->unverifiable.begin(), out->unverifiable.end());
  return out->mismatch_count == 0 ? Status::kOk : Status::kBadGeometry;
}

}  // namespace probe
}  // namespace thyme
