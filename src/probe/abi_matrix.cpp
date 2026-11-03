// abi_matrix.cpp — OPS-06 ABI constants matrix and its deterministic JSON.
//
// The versions are read from the headers at compile time (no literals), so a
// version bump is reflected here automatically and the deployment-facing JSON
// cannot disagree with the binaries. The rows mirror the "current version" table in
// docs/reference/abi-compatibility.md; keep the two in sync when a format is
// added.

#include "thyme/probe/abi_matrix.h"

#include "thyme/app/mp_control.h"
#include "thyme/record/package_integrity.h"
#include "thyme/record/record_schema.h"
#include "thyme/record/thyme_bin.h"
#include "thyme/record/trigger_protocol.h"
#include "thyme/runtime/pinned_slot_store.h"
#include "thyme/runtime/latest_slot.h"
#include "thyme/runtime/mp_session_identity_page.h"
#include "thyme/runtime/record_mirror.h"
#include "thyme/runtime/record_mirror_ring.h"
#include "thyme/runtime/shared_health.h"
#include "thyme/runtime/session_lease.h"
#include "thyme/runtime/shm_spmc_ring.h"
#include "thyme/runtime/shm_spsc_ring.h"

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

namespace thyme {
namespace probe {
namespace {

std::string JsonStr(const char* value) {
  std::string out = "\"";
  for (const char* p = value; p != nullptr && *p != '\0'; ++p) {
    const unsigned char c = static_cast<unsigned char>(*p);
    switch (c) {
      case '"':
        out += "\\\"";
        break;
      case '\\':
        out += "\\\\";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\r':
        out += "\\r";
        break;
      case '\t':
        out += "\\t";
        break;
      default:
        if (c < 0x20) {
          char buf[8];
          std::snprintf(buf, sizeof(buf), "\\u%04x",
                        static_cast<unsigned int>(c));
          out += buf;
        } else {
          out += static_cast<char>(c);
        }
        break;
    }
  }
  out += '"';
  return out;
}

std::string JsonStr(const std::string& value) { return JsonStr(value.c_str()); }

// Declaration order is logical (grouped by subsystem); the public accessor
// sorts by (component, object) so the JSON is stable regardless of edits.
std::vector<AbiConstant> BuildConstants() {
  std::vector<AbiConstant> rows = {
      {"app", "MpControl", "kMpControlAbiVersion",
       app::MpControl::kMpControlAbiVersion, AbiSurface::kShmPage,
       Kind::kMpControl, true, true,
       "v3: ARCH-11 super-header + explicit magic; v2 rejected"},
      {"app", "SharedHealthPage", "SharedHealthPage::kVersion",
       SharedHealthPage::kVersion, AbiSurface::kShmPage, Kind::kSharedHealth,
       true, true,
       "v3: ARCH-11 super-header + 64-bit drop counters; v2 rejected"},
      {"app", "MpSessionIdentityPage", "kMpSessionIdentityVersion",
       kMpSessionIdentityVersion, AbiSurface::kShmPage, Kind::kUnknown, false,
       true,
       "ARCH-17 session identity page; probes explicitly with 'session'"},
      {"app", "SessionLeasePage", "kSessionLeaseVersion", kSessionLeaseVersion,
       AbiSurface::kShmPage, Kind::kUnknown, false, true,
       "ARCH-14 topology session lease; a live second writer gets kBusy"},
      {"pinned_slot", "PinnedSlotPageHeader", "kPinnedSlotStoreVersion",
       kPinnedSlotStoreVersion, AbiSurface::kShmPage, Kind::kPinnedSlot, true,
       true, "v3: REP-10 ProducerGate in the header; v2 peers rejected"},
      {"pinned_slot", "PinnedSlotLeaseHeader", "kPinnedSlotLeaseVersion",
       kPinnedSlotLeaseVersion, AbiSurface::kShmPage, Kind::kPinnedSlot, false,
       true,
       "v5: copy-active + dead-owner recovery; v4/v3/v2 peers rejected; "
       "companion <name>.lease"},
      {"latest_slot", "LatestSlot<T>", "kLatestSlotLayoutVersion",
       kLatestSlotLayoutVersion, AbiSurface::kShmPage, Kind::kLatestSlot, true,
       true,
       "v3: ARCH-11 super-header + 64 B geometry block; v2 body rejected"},
      {"record", ".thyme.bin", "kThymeBinFileAbiVersion",
       kThymeBinFileAbiVersion, AbiSurface::kFile, Kind::kUnknown, false, false,
       "v4: ABI-11 keyed registry (128 B ThymeStructDesc) + ABI-10 manifest "
       "block; v1..v3 read as legacy (40 B registry, manifest absent in "
       "v1/v2); a pre-v4 reader rejects v4 by version"},
      {"record", ".tbin record-manifest", "kThymeBinManifestBlockVersion",
       kThymeBinManifestBlockVersion, AbiSurface::kFile, Kind::kUnknown, false,
       false,
       "ABI-10: 32 B CRC-protected block after the registry; present iff file "
       "version >= 3"},
      {"record", "record manifest", "schema::kManifestVersion",
       schema::kManifestVersion, AbiSurface::kFile, Kind::kUnknown, false,
       false,
       "ABI-10: RecordStructId renumbering is an ABI change; readers refuse a "
       "mismatched manifest hash"},
      {"record", "package schema.json", "schema::kVersion", schema::kVersion,
       AbiSurface::kFile, Kind::kUnknown, false, false,
       "ABI-09/ABI-11/REC-27 sidecar; v2 adds the ABI-11 channel key table, "
       "v3 adds payload_profiles[]; absent = pre-ABI-09/legacy package"},
      {"record", "RecordMirrorPage", "RecordMirrorPage::kVersion",
       RecordMirrorPage::kVersion, AbiSurface::kShmPage, Kind::kRecordMirror,
       true, true,
       "fixed 2 MB arena tap page; v4 adds the ARCH-11 super-header"},
      {"record", "RecordMirrorRingPage", "RecordMirrorRingPage::kVersion",
       RecordMirrorRingPage::kVersion, AbiSurface::kShmPage,
       Kind::kRecordMirrorRing, true, true,
       "tap page; v6 adds a per-struct REP-10 ProducerGate to each ring "
       "descriptor; v5 and earlier peers rejected"},
      {"ring", "ShmSpmcRing", "kShmSpmcRingVersion", kShmSpmcRingVersion,
       AbiSurface::kShmPage, Kind::kSpmcRing, true, true,
       "v4: ARCH-11 super-header; per-reader drop slots + shared bitmap"},
      {"ring", "ShmSpscRing", "kShmSpscRingVersion", kShmSpscRingVersion,
       AbiSurface::kShmPage, Kind::kSpscRing, true, true,
       "v4: ARCH-11 super-header + epoch_waiters"},
      {"trigger", "TriggerRequest/TriggerResponse", "kTriggerVersion",
       trigger_proto::kTriggerVersion, AbiSurface::kWire, Kind::kUnknown,
       false, false,
       "v4 request_id dedup; v1/v2 compat flags, v3 replay policy unchanged"},
      {"evidence", "integrity.manifest", "kManifestVersion",
       integrity::kManifestVersion, AbiSurface::kFile, Kind::kUnknown, false,
       false, "evidence package manifest; not a mixed-run ABI"},
  };
  std::sort(rows.begin(), rows.end(),
            [](const AbiConstant& a, const AbiConstant& b) {
              const int by_component = std::string(a.component).compare(
                  std::string(b.component));
              if (by_component != 0) return by_component < 0;
              return std::string(a.object).compare(std::string(b.object)) < 0;
            });
  return rows;
}

}  // namespace

const char* SurfaceName(AbiSurface surface) {
  switch (surface) {
    case AbiSurface::kShmPage:
      return "shm_page";
    case AbiSurface::kWire:
      return "wire";
    case AbiSurface::kFile:
      return "file";
    case AbiSurface::kLayout:
      return "layout";
  }
  return "unknown";
}

const std::vector<AbiConstant>& AbiConstants() {
  static const std::vector<AbiConstant> rows = BuildConstants();
  return rows;
}

const AbiConstant* PrimaryPageConstant(Kind kind) {
  if (kind == Kind::kUnknown) return nullptr;
  const auto& rows = AbiConstants();
  for (const auto& row : rows) {
    if (row.kind == kind && row.primary_page_version) {
      return &row;
    }
  }
  return nullptr;
}

bool ExpectedPageVersion(Kind kind, uint32_t* version) {
  const AbiConstant* row = PrimaryPageConstant(kind);
  if (row == nullptr || !row->in_page_version) return false;
  if (version != nullptr) *version = row->version;
  return true;
}

const AbiConstant* FindAbiConstant(const char* object) {
  if (object == nullptr) return nullptr;
  const std::string wanted(object);
  for (const auto& row : AbiConstants()) {
    if (wanted == row.object) return &row;
  }
  return nullptr;
}

std::string AbiMatrixJson() {
  const auto& rows = AbiConstants();
  std::string out = "{\"schema\":1,\"entries\":[";
  for (std::size_t i = 0; i < rows.size(); ++i) {
    const AbiConstant& row = rows[i];
    out += i == 0 ? "\n" : ",\n";
    out += "{\"component\":" + JsonStr(row.component);
    out += ",\"object\":" + JsonStr(row.object);
    out += ",\"constant\":" + JsonStr(row.constant);
    out += ",\"version\":" + std::to_string(row.version);
    out += ",\"surface\":" + JsonStr(SurfaceName(row.surface));
    out += ",\"probe_kind\":";
    out += (row.surface == AbiSurface::kShmPage && row.kind != Kind::kUnknown)
               ? JsonStr(KindName(row.kind))
               : "null";
    out += ",\"primary\":";
    out += row.primary_page_version ? "true" : "false";
    out += ",\"in_page_version\":";
    out += row.in_page_version ? "true" : "false";
    out += ",\"note\":" + JsonStr(row.note);
    out += "}";
  }
  out += rows.empty() ? "]}" : "\n]}";
  return out;
}

std::string AbiReportJson(const AbiReport& report) {
  std::string out = "{\"schema\":1,\"ok\":";
  out += report.ok() ? "true" : "false";
  out += ",\"checked\":" + std::to_string(report.pages.size());
  out += ",\"mismatches\":" + std::to_string(report.mismatch_count);
  out += ",\"pages\":[";
  for (std::size_t i = 0; i < report.pages.size(); ++i) {
    const AbiCheck& page = report.pages[i];
    out += i == 0 ? "\n" : ",\n";
    out += "{\"name\":" + JsonStr(page.name);
    out += ",\"kind\":" + JsonStr(KindName(page.kind));
    out += ",\"object\":" + JsonStr(page.object);
    out += ",\"expected\":" + std::to_string(page.expected);
    out += ",\"actual\":" + std::to_string(page.actual);
    out += ",\"checked\":";
    out += page.checked ? "true" : "false";
    out += ",\"compatible\":";
    out += page.compatible ? "true" : "false";
    out += ",\"note\":" + JsonStr(page.note);
    out += "}";
  }
  out += report.pages.empty() ? "]" : "\n]";
  out += ",\"unverifiable\":[";
  for (std::size_t i = 0; i < report.unverifiable.size(); ++i) {
    out += i == 0 ? "\n" : ",\n";
    out += JsonStr(report.unverifiable[i]);
  }
  out += report.unverifiable.empty() ? "]}" : "\n]}";
  return out;
}

}  // namespace probe
}  // namespace thyme
