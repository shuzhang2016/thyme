#pragma once

// replay_export.h — REP-09 package export / republish (sibling tool only).
//
// `thyme-replay export <pkg> --out <file> [--format mcap|ndjson]` streams the
// package's unified mono_ns timeline to an external-tool format. It never runs
// in the replay hot path: the export reads the package through ReplayPackage
// (mmap payloads) and writes sequentially, one event at a time.
//
// Formats (both documented in docs/guides/replay.md):
//
//   mcap    Reuses the ECO-01 streaming McapWriter. One MCAP channel per
//           (package channel, struct) data pair, topics
//           "thyme/<channel>/<struct>"; big-frame payloads are the raw .raw
//           segments (message_encoding "thyme.frame"), small/meta payloads the
//           recorded bytes ("thyme"). Gap/trigger/session rows become JSON
//           messages on "thyme/<channel>/{gap,trigger,session}". A synthetic
//           message on "thyme/package" links the ABI-09 schema.json sidecar and
//           the ABI-10 record manifest.
//   ndjson  One JSON object per line: a header record, one event record per
//           timeline entry (payload base64, schema_ref into the sidecar) and a
//           summary record. Generic republish format for tools that do not
//           link thyme.
//
// Bounded memory: MCAP is written through McapWriter's fixed buffer (default
// 64 KiB, --mcap-buffer-bytes) and messages larger than the buffer are written
// straight through (McapWriter::WriteDirect); NDJSON writes fields and base64
// in fixed 3 KiB/4 KiB stack chunks. Neither path buffers the file or a whole
// .raw segment on the heap.

#include "replay_cli.h"

#include <cstdint>
#include <ostream>
#include <string>
#include <vector>

namespace thyme {
namespace replaycli {

enum class ExportFormat : uint8_t {
  kMcap = 0,
  kNdjson = 1,
};

// kRaw keeps today's MCAP/NDJSON bytes. cbor/msgpack/tlv (ECO-13) write a
// self-describing file and are mutually exclusive with --format.
// protobuf/cdr (REP-15) are the message encoding inside MCAP/NDJSON.
enum class ExportEncode : uint8_t {
  kRaw = 0,
  kCbor = 1,
  kMsgpack = 2,
  kTlv = 3,
  kProtobuf = 4,
  kCdr = 5,
};

inline bool IsSelfDescribing(ExportEncode encode) {
  return encode == ExportEncode::kCbor || encode == ExportEncode::kMsgpack ||
         encode == ExportEncode::kTlv;
}

inline bool IsCanonicalEncode(ExportEncode encode) {
  return encode == ExportEncode::kProtobuf || encode == ExportEncode::kCdr;
}

const char* ExportFormatName(ExportFormat format);
bool ParseExportFormat(const std::string& text, ExportFormat* out);
const char* ExportEncodeName(ExportEncode encode);
bool ParseExportEncode(const std::string& text, ExportEncode* out);

struct ExportOptions {
  std::string package_dir;
  std::string out_path;
  ExportFormat format = ExportFormat::kMcap;
  ExportEncode encode = ExportEncode::kRaw;
  bool format_explicit = false;
  // Per-frame payload CRC policy: verify every frame that declares a CRC and
  // fail on a mismatch; frames with payload_crc == 0 (recorder crc=off) are
  // counted as skipped, never silently "verified".
  bool verify_crc = true;
  bool has_from = false;
  TimeSpec from{};
  bool has_to = false;
  TimeSpec to{};
  // Optional copy of <pkg>/schema.json next to the export; when set, the
  // export's schema_ref points at this path instead of the package sidecar.
  std::string schema_out;
  uint32_t mcap_buffer_bytes = 64u * 1024u;
  bool json = false;
  bool have_expect_manifest_hash = false;
  uint64_t expect_manifest_hash = 0;
  // SEC-06: offline redaction. When non-empty, every record on this channel
  // has its payload zeroed (length preserved) on export; structure, timing
  // and other channels are untouched. Applies to raw, MCAP, NDJSON and the
  // canonical/self-describing encoders. Never rewrites the recorded package.
  std::string redact_channel;
  // REC-27: resolve each big-frame channel's payload profile against the
  // exporter registry. Unknown/mismatched profiles refuse the decode path
  // (non-zero exit) but the raw export is still written.
  bool decode_profile = false;
};

struct ExportChannelSummary {
  uint32_t id = 0;
  std::string name;
  std::string kind;
  uint64_t messages = 0;
  uint64_t bytes = 0;
};

struct ExportSummary {
  bool ok = false;
  int exit_code = kExitFailure;
  std::string error;
  std::string format;
  std::string encode = "raw";
  std::string package_dir;
  std::string out_path;
  std::string schema_ref;    // empty when the package has no sidecar
  std::string schema_state;  // ok | absent
  uint64_t events = 0;
  uint64_t data_messages = 0;
  uint64_t gap_messages = 0;
  uint64_t trigger_messages = 0;
  uint64_t session_messages = 0;
  uint64_t payload_bytes = 0;
  uint64_t crc_checked = 0;
  uint64_t crc_bad = 0;
  uint64_t crc_skipped = 0;  // frames with payload_crc == 0 (policy off)
  uint64_t first_mono_ns = 0;
  uint64_t last_mono_ns = 0;
  uint32_t schemas = 0;
  uint32_t channels = 0;
  uint64_t output_bytes = 0;
  uint64_t encoded_bytes = 0;
  uint64_t encode_ns = 0;
  bool removed_partial = false;
  bool decode_refused = false;
  std::vector<ExportChannelSummary> per_channel;
};

// Streams the package to `options.out_path`. On failure the partial output is
// removed and `summary.error` says why (exit 1 failure, 2 usage).
int RunExport(const ExportOptions& options, std::ostream& out,
              ExportSummary* summary);

}  // namespace replaycli
}  // namespace thyme
