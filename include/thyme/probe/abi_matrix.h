#pragma once
// OPS-06: compiled-in ABI constants matrix + pre-flight compatibility report.
//
// Every version below is taken directly from the owning header, never copied
// as a literal, so the matrix cannot drift from the binaries that produce it.
// `thyme_probe abi --matrix` prints it as deterministic JSON for deployment
// scripts; `thyme_probe abi` compares the pages actually living in /dev/shm
// against it (read-only, same contract as the rest of the probe).
//
// The matrix is also the machine-readable form of the "current version" table in
// docs/reference/abi-compatibility.md and docs/guides/upgrade.md.

#include "thyme/probe/probe.h"

#include <cstdint>
#include <string>
#include <vector>

namespace thyme {
namespace probe {

enum class AbiSurface : uint8_t {
  kShmPage = 0,  // version word lives in a /dev/shm page
  kWire,         // version on a socket/wire message
  kFile,         // version in a file format
  kLayout,       // compile-time layout only, no in-page version word
};

const char* SurfaceName(AbiSurface surface);

struct AbiConstant {
  const char* component;  // "app", "pinned_slot", "ring", ...
  const char* object;     // header/struct/format name
  const char* constant;   // header constant (or layout constant) name
  uint32_t version;
  AbiSurface surface;
  Kind kind;  // page kind for kShmPage; kUnknown when the probe cannot see it
  // True when this row describes the page's own version word. False for
  // companion pages (the PinnedSlot lease table lives in "<name>.lease").
  bool primary_page_version;
  // False when the object has no readable version word (LatestSlot layout).
  bool in_page_version;
  const char* note;
};

// Stable, deterministic order (sorted by component then object).
const std::vector<AbiConstant>& AbiConstants();

// Version this build requires for a page kind's own version word. Returns
// false for kinds without one (LatestSlot) and for non-page kinds.
bool ExpectedPageVersion(Kind kind, uint32_t* version);

// Row describing the page's own version word, or nullptr.
const AbiConstant* PrimaryPageConstant(Kind kind);

// Row whose `object` matches (exact string), or nullptr.
const AbiConstant* FindAbiConstant(const char* object);

// Deterministic JSON:
//   {"schema":1,"entries":[{"component":...,"version":...,...},...]}
std::string AbiMatrixJson();

// JSON for a CheckAbi() result (see probe.h).
std::string AbiReportJson(const AbiReport& report);

}  // namespace probe
}  // namespace thyme
