#pragma once

// replay_cli.h — REP-08 (player CLI) + REC-07 (trigger-package replay tool).
//
// The `thyme-replay` sibling tool is a thin shell over the replay core:
//
//   list   <pkg>   channel/kind/count/span/geometry/index/completeness
//   info   <pkg>   one package summary with per-channel rate and bytes
//   schema <pkg>   ABI-09 record schema sidecar (contracts, field layout,
//                  manifest identity); non-zero when absent or tampered
//   play   <pkg>... replay into live targets through the REP-04/REP-05
//                  injectors; absolute slices replace only the ReplaySession
//   verify <pkg>   read-only integrity check (record CRCs, per-frame CRCs,
//                  index consistency); non-zero exit on corruption
//   integrity <pkg> SEC-03 evidence manifest verification (per-file sha256,
//                  hash chain, optional HMAC-SHA256 with --key); works
//                  offline, no daemon; non-zero on tamper/missing/absent
//
// CLI policy lives outside the core libraries: exceptions and iostreams are
// allowed here; session, package-contract and injector behavior stays in
// `thyme::replay`.
//
// Target safety (play)
// --------------------
// Targets are explicit and never guessed:
//   --pinned_slot <channel>=<shm-name>
//   --small <channel>:<struct_id>=<shm-ring-name>:<target-struct-id>
//   --small <channel>=<shm-ring-name>   (REP-11: resolve struct ids by the
//                                        ABI-11 record key tuple)
// A package data channel without a target (small/big-frame) is skipped and
// counted; --strict-targets turns that into a pre-play error (exit 3). The
// meta channel is informational: its control rows are always `ignored`, and
// its data rows are only injected when the caller explicitly maps them.
// Big-frame replay is a *producer*: the tool creates and owns the target
// channel (from package geometry). An existing channel is refused unless
// --force, because a live producer would keep the old mapping. Small-struct
// rings are attached read-write, never created or clobbered.
//
// Time values (--seek/--from/--to)
// --------------------------------
// <int>              nanoseconds relative to the first package's first event
// <num>ns/us/ms/s/m/h  scaled offset from the first event
// HH:MM:SS.mmm       offset from the first event
// @<mono_ns>         absolute timeline mono_ns
// --to is inclusive: playback stops before the first event whose mono_ns
// exceeds it, and that event is never injected.

#include "thyme/record/package_integrity.h"
#include "thyme/replay/replay_session.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <ostream>
#include <string>
#include <vector>

namespace thyme {
namespace replaycli {

// Exit codes of every command.
inline constexpr int kExitOk = 0;
inline constexpr int kExitFailure = 1;   // package/inject/verify failure
inline constexpr int kExitUsage = 2;     // bad arguments
inline constexpr int kExitTargets = 3;   // target configuration / strict check

struct TimeSpec {
  bool absolute = false;  // @<mono_ns> (still usable as a --to bound)
  uint64_t ns = 0;
};

// Accepts the forms documented above. `err` is filled on failure.
bool ParseTimeSpec(const std::string& text, TimeSpec* out, std::string* err);

// One repeated target option. `channel` is the option's left-hand text
// (package channel name, or a decimal id); it is resolved against the
// package at play time.
struct TargetSpec {
  std::string raw;
  std::string channel;
  std::string shm_name;
  uint16_t struct_id = 0;         // --small only
  uint16_t target_struct_id = 0;  // --small only
  uint32_t slot_bytes = 0;        // --pinned_slot override (0 = package geometry)
  // REP-11: `--small <channel>=<ring>` resolves every struct of the package
  // channel against the target ring's descriptor table by
  // (record_key, contract_version, payload_size, layout_hash). The explicit
  // `<channel>:<struct_id>=<ring>:<id>` form keeps its old meaning.
  bool auto_map = false;
};

bool ParsePinnedSlotTarget(const std::string& text, TargetSpec* out,
                         std::string* err);
bool ParseSmallTarget(const std::string& text, TargetSpec* out,
                      std::string* err);

struct PlayOptions {
  std::string package_dir;
  // Empty means package_dir is the only input. Otherwise these are the explicit
  // package paths in command-line order (package_dir remains their first path).
  std::vector<std::string> package_dirs;
  std::string timeline = "absolute";
  std::vector<TargetSpec> pinned_slots;
  std::vector<TargetSpec> smalls;
  double rate = 1.0;
  bool as_fast = false;
  bool start_paused = false;
  bool step_once = false;  // deliver exactly one event, then stop
  bool has_from = false;
  TimeSpec from{};
  bool has_to = false;
  TimeSpec to{};
  bool has_seek = false;
  TimeSpec seek{};
  bool strict_targets = false;
  bool force = false;
  // REP-11 topology mapping:
  //   --allow-missing   an auto-mapped package channel/struct with no target
  //                     is skipped (default: hard error)
  //   --check-compat    print the mapping table and exit without touching
  //                     live targets
  //   --dry-run         same as --check-compat (resolve + report, no inject)
  bool allow_missing = false;
  bool check_compat = false;
  bool dry_run = false;
  // REP-10: hold the in-header producer gate for every big-frame target and
  // heartbeat it while playing. --no-gate is the explicit escape hatch for
  // legacy targets whose writers cannot honour it (unsafe: no double-write
  // detection). Default on.
  bool gate = true;
  bool verify_crc = true;
  bool preserve_sequences = false;
  uint32_t pinned_slot_slots = 64;
  bool json = false;
  // REP-08 live status: publish the JSON snapshot (and the final summary) to
  // this path while playing; 0 disables it. See replay_status.h.
  std::string status_file;
  uint32_t status_interval_ms = 200;
  // ABI-10: refuse to decode/inject when the package's record manifest does
  // not match the caller's build (RecordStructId is a row index; a different
  // manifest means different channel ids). No silent decode.
  bool have_expect_manifest_hash = false;
  uint64_t expect_manifest_hash = 0;
};

// Result of a runner-side control rendezvous.
enum class ControlTurn : uint8_t {
  kProceed = 0,  // deliver the next event (session continues unpaused)
  kStep,         // deliver exactly one event (Step()), stay paused
  kStop,         // terminate the run
  kRetry,        // pending rate/seek request: apply it, then rendezvous again
};

// Thread-safe playback control surface. RequestStop() interrupts a paused or
// pacing NextEvent() through ReplaySession::Stop(); the runner never has to
// poll for signals inside the session. RequestRate/RequestSeek are queued and
// applied by the playback thread between events (Seek is playback-thread-only
// in ReplaySession), which is what makes them safe from a tty/signal thread.
class PlayControl {
 public:
  PlayControl();
  ~PlayControl();
  PlayControl(const PlayControl&) = delete;
  PlayControl& operator=(const PlayControl&) = delete;

  void RequestPause();
  void RequestResume();
  void RequestStep();
  void RequestStop();
  // Queued runtime controls; the last request wins. Rate must be > 0 (a
  // non-positive value is ignored here — the CLI validates input).
  void RequestRate(double rate);
  // Absolute package mono_ns, already resolved from the CLI time form.
  void RequestSeek(uint64_t mono_ns);
  bool stop_requested() const { return stop_.load(std::memory_order_relaxed); }
  bool paused() const;

  void Attach(replay::ReplaySession* session);
  void Detach();

  // Runner-side rendezvous: blocks while the run is paused (and no pending
  // request could unblock it), consumes one step request, and returns kRetry
  // when a queued rate/seek must be applied first.
  ControlTurn WaitForTurn();
  // Non-blocking handoff of queued runtime controls to the playback thread.
  void TakeRequests(double* rate, bool* has_rate, uint64_t* seek_ns,
                    bool* has_seek);

 private:
  mutable std::mutex mu_;
  std::condition_variable cv_;
  bool paused_ = false;
  uint32_t steps_ = 0;
  bool has_rate_ = false;
  double rate_ = 1.0;
  bool has_seek_ = false;
  uint64_t seek_ns_ = 0;
  std::atomic<bool> stop_{false};
  std::atomic<replay::ReplaySession*> session_{nullptr};
};

// ---- read-only commands -------------------------------------------------

struct StructReport {
  uint16_t struct_id = 0;
  std::string name;
  std::string channel;
  std::string record_key;
  uint16_t contract_version = 0;
  uint32_t payload_size = 0;
  uint64_t layout_hash = 0;
};

struct ChannelReport {
  uint32_t id = 0;
  std::string name;
  std::string kind;  // "meta" | "small" | "pinned_slot"
  // ABI-11 registry identity (empty keys on a legacy package).
  std::vector<StructReport> structs;
  uint64_t records = 0;
  uint64_t data = 0;
  uint64_t gaps = 0;
  uint64_t first_mono_ns = 0;
  uint64_t last_mono_ns = 0;
  bool indexed = false;
  uint32_t max_payload_bytes = 0;
  uint32_t min_payload_bytes = 0;
  uint64_t tbin_bytes = 0;
  uint64_t raw_bytes = 0;
  std::string tbin_path;
  std::string raw_path;
  // REP-06 seek/index metadata (from the Open validation scan).
  uint64_t data_offset = 0;
  uint64_t records_end_offset = 0;
  uint64_t first_record_offset = 0;
  uint64_t last_record_offset = 0;
  uint32_t seek_entries = 0;
  uint32_t seek_stride = 0;
  uint64_t record_crc_checked = 0;
  uint64_t index_crc_checked = 0;
  uint64_t frame_crc_declared = 0;
  uint64_t frame_crc_absent = 0;
  uint64_t first_raw_offset = 0;
  uint64_t last_raw_end = 0;
  // Filled by info/verify (one event pass).
  uint64_t data_bytes = 0;
  double rate_hz = 0.0;
  double bytes_per_s = 0.0;
  // verify: big-frame payload CRC counts and frame geometry.
  uint64_t crc_checked = 0;
  uint64_t crc_ok = 0;
  uint64_t crc_bad = 0;
  uint64_t crc_unchecked = 0;
  uint32_t min_frame_len = 0;
  uint32_t max_frame_len = 0;
};

struct PackageReport {
  bool ok = false;
  std::string error;
  std::string package_dir;
  bool wal_present = false;
  bool package_complete = false;
  uint64_t events = 0;
  uint64_t first_mono_ns = 0;
  uint64_t last_mono_ns = 0;
  uint64_t duration_ns = 0;
  uint64_t data_bytes = 0;
  uint64_t mono_regressions = 0;
  std::vector<ChannelReport> channels;
  // ABI-09/ABI-10 record contract (see ReplayPackage). `schema_state` is
  // "absent" for a legacy package; "ok" means the sidecar parsed and agreed
  // with the .tbin manifest block.
  std::string schema_state;   // ok | absent | invalid
  std::string schema_error;
  bool schema_partial = false;
  uint32_t schema_version = 0;
  uint32_t schema_records = 0;
  bool manifest_declared = false;
  uint64_t manifest_hash = 0;
  uint16_t manifest_version = 0;

  const ChannelReport* Find(const std::string& name) const;
};

// list: header/channel table only (no event pass).
int RunList(const std::string& dir, bool json, std::ostream& out,
            PackageReport* report, bool have_expect_manifest = false,
            uint64_t expect_manifest_hash = 0);
// info: list + one event pass for per-channel rate/bytes.
int RunInfo(const std::string& dir, bool json, std::ostream& out,
            PackageReport* report, bool have_expect_manifest = false,
            uint64_t expect_manifest_hash = 0);
// verify: Open validation (record CRCs, registry, footer/offsets, ABI-09
// schema sidecar, ABI-10 manifest identity) plus a per-frame CRC pass over
// every big-frame payload. Non-zero on corruption.
int RunVerify(const std::string& dir, bool json, std::ostream& out,
              PackageReport* report, bool have_expect_manifest = false,
              uint64_t expect_manifest_hash = 0);

// schema: read-only ABI-09 sidecar dump. Absent or tampered -> non-zero.
int RunSchema(const std::string& dir, bool json, std::ostream& out,
              PackageReport* report);

// ABI-10: compares a package's declared manifest with an expected build hash.
// Returns true when the package may be decoded (no expectation, or a match);
// fills *err with the explicit mismatch otherwise.
bool CheckManifestIdentity(bool declared, uint64_t hash, bool have_expect,
                           uint64_t expect_hash, std::string* err);
bool CheckExpectedManifest(const PackageReport& report, bool have_expect,
                           uint64_t expect_hash, std::string* err);

// SEC-03: verify the evidence manifest offline (no daemon). `key_path` is the
// optional HMAC-SHA256 key file; a signed manifest requires it. Reports
// OK/absent/invalid, per-file and chain status; non-zero on mismatch, missing
// or tampered files (and on an absent manifest: an unsealed package is not a
// verified one).
int RunIntegrity(const std::string& dir, bool json,
                 const std::string& key_path, std::ostream& out,
                 integrity::VerifyResult* result);

// ---- play ---------------------------------------------------------------

struct PlayChannelSummary {
  uint32_t id = 0;
  std::string name;
  std::string kind;
  uint64_t events = 0;
  uint64_t injected = 0;
  uint64_t skipped = 0;   // no target / pinned / oversize / crc / write fail
  uint64_t ignored = 0;   // gap/trigger/session rows (no payload)
  uint64_t failed = 0;    // unusable event or injector not open
  uint64_t unmapped = 0;  // subset of skipped: channel has no target
  uint64_t bytes = 0;
  uint64_t last_mono_ns = 0;
};

struct PlaySummary {
  bool ok = false;
  int exit_code = kExitFailure;
  std::string error;
  std::vector<std::string> warnings;
  std::vector<PlayChannelSummary> channels;
  uint64_t events = 0;
  uint64_t injected = 0;
  uint64_t skipped = 0;
  uint64_t ignored = 0;
  uint64_t failed = 0;
  uint64_t unmapped = 0;
  uint64_t first_event_ns = 0;
  uint64_t last_event_ns = 0;
  uint64_t mono_regressions = 0;
  uint64_t slice_joins = 0;
  uint64_t join_duplicates = 0;
  uint64_t join_holes = 0;
  uint64_t gate_rejects = 0;  // REP-10: real-producer writes blocked
  uint64_t gate_steals = 0;   // REP-10: stale gates reclaimed
  // REP-11: resolved topology mappings (counted before playback).
  uint64_t mappings_mapped = 0;
  uint64_t mappings_legacy = 0;
  uint64_t mappings_missing = 0;  // skipped because --allow-missing
  uint64_t mapping_conflicts = 0; // incompatible tuples (always fatal)
  uint64_t setup_ns = 0;
  uint64_t play_ns = 0;
  bool stopped = false;        // SIGINT/SIGTERM/control stop
  bool window_closed = false;  // --to reached
  bool step_done = false;      // --step delivered its one event
  // REP-08: effective run parameters and terminal state, so the JSON summary
  // and the status document agree (the effective rate can change at runtime).
  std::string mode;            // paced | as-fast | step
  std::string timeline;        // absolute | stitch
  double rate = 1.0;
  std::string state;           // finished | failed | stopped
  std::string status_file;     // empty when no status document was written
};

// Runs one playback. `control` may be null (tests / non-interactive); when
// non-null it is attached for the duration so another thread can stop, pause
// or step the run. Prints warnings before playback and the final summary
// (text or JSON) to `out`.
int RunPlay(const PlayOptions& options, PlayControl* control, std::ostream& out,
            PlaySummary* summary);

// ---- argv dispatch ------------------------------------------------------

// Parses `args` (without argv[0]) and runs the command. Used by main.cpp and
// by tests so the CLI surface is exercised without a subprocess.
int RunCli(const std::vector<std::string>& args, std::ostream& out,
           std::ostream& err, PlayControl* control);
void PrintUsage(std::ostream& out, std::ostream& err);

// Human-readable helpers (shared with tests).
std::string FormatNs(uint64_t ns);
std::string FormatDurationNs(uint64_t ns);
std::string FormatBytes(uint64_t bytes);

}  // namespace replaycli
}  // namespace thyme
