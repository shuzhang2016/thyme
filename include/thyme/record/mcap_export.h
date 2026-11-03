#pragma once

// .thyme.bin -> MCAP conversion (REC-06).
//
// Mapping:
//   * one MCAP schema + channel per registered thyme struct, topic
//     "thyme/<name>" (or "thyme/struct_<id>" when the descriptor has no
//     usable name). Schema encoding "jsonschema".
//   * kData payloads are passed through byte-for-byte with message encoding
//     "thyme"; mono_ns becomes both log_time and publish_time.
//   * kGap / kTrigger / kSession become JSON messages on the dedicated topics
//     "thyme/gap", "thyme/trigger" and "thyme/session" (message encoding
//     "json"). A synthetic "session_start" message carries the file header
//     (device_id, session_id, start_mono_ns) at start_mono_ns.
//
// ThymeStructDesc carries only id/contract_version/payload_size/name, no field
// layout. The generated JSON schema is therefore advisory: it names the struct
// and declares the payload as an opaque block. Foxglove can open the file and
// show channels/timestamps/raw bytes, but cannot decode thyme struct fields.
//
// The thyme record_sequence/source_sequence fields have no MCAP equivalent
// that preserves the payload unchanged; MCAP message `sequence` is generated
// per channel (0,1,2,...). MCAP log_time is thyme mono_ns (a monotonic clock,
// not Unix epoch), so tools show a relative timeline.
//
// kSession entries carry no timestamp in .thyme.bin; they are stamped with the
// last observed mono_ns (or the header start_mono_ns before the first record).

#include "thyme/record/mcap_writer.h"
#include "thyme/record/thyme_bin.h"

#include <cstdint>

namespace thyme {

enum class ThymeBinExportStatus : uint8_t {
  kOk = 0,
  kInvalidArgument,
  kInputOpen,
  kInputCorrupt,
  kUnknownStruct,
  kWriterError,
};

struct ThymeBinToMcapOptions {
  bool emit_events = true;
  bool emit_header_session = true;
  bool include_summary = true;
  bool data_section_crc = false;
};

struct ThymeBinToMcapStats {
  uint64_t data_messages = 0;
  uint64_t gap_messages = 0;
  uint64_t trigger_messages = 0;
  uint64_t session_messages = 0;
  uint64_t total_messages = 0;
  uint32_t channels = 0;
  uint64_t input_records = 0;
  uint64_t corrupt_offset = 0;
  uint32_t corrupt_crc_expected = 0;
  uint32_t corrupt_crc_actual = 0;
  bool output_opened = false;
  McapStatus writer_status = McapStatus::kOk;
};

// Convenience overload: opens `in_path`, creates `out_path`, exports, closes.
ThymeBinExportStatus ThymeBinToMcap(const char* in_path, const char* out_path,
                                    const ThymeBinToMcapOptions& options = {},
                                    ThymeBinToMcapStats* stats = nullptr);

// Streaming overload. The caller owns both objects; `writer` must already be
// open. Schemas/channels are added by this function and must not be
// pre-registered. The writer is deliberately NOT closed here so several
// recordings can be appended to one MCAP; call McapWriter::Close() yourself.
ThymeBinExportStatus ThymeBinToMcap(ThymeBinReader& reader, McapWriter& writer,
                                    const ThymeBinToMcapOptions& options = {},
                                    ThymeBinToMcapStats* stats = nullptr);

}  // namespace thyme
