// fault_role.cpp — OPS-08: role process used by tools/ops/fault_matrix/.
//
// One binary, one mode per role/observation. The Python driver
// (fault_matrix.py) spawns it, kills it with SIGKILL, or reads its final JSON
// line; every mode prints machine-readable lines on stdout and exits non-zero
// when its own observable does not hold. Keeping the assertions here (C++,
// library-level) means the driver only orchestrates.
//
// Modes:
//   producer <shm> <slot_bytes> <slots> <max_frames|0>
//       Creates a PinnedSlotStore (CRC on) and commits frames forever. Prints
//       {"ready":true} once, then {"committed":N} every 32 frames.
//   reader <shm> <min_frames> <drain|follow>
//       Attaches and validates every committed frame (payload pattern + CRC).
//       drain: reads the committed prefix, prints the final JSON and exits 0
//       when frames>=min and no frame is bad/torn. follow: prints
//       {"attached":true} and loops until killed.
//   unlink <shm>
//       Removes a channel's payload + lease objects (driver cleanup).
//   shm-full <shm> <bytes>
//       Simulates an exhausted SHM filesystem with RLIMIT_FSIZE (no root):
//       Create() must fail, leave no object behind, and succeed once the
//       limit is restored.
//   recorder-enospc <out_dir> <mirror_shm>
//       Runs a real TriggeredRecorder against an injected drain-write failure
//       (Config::fault_inject_write, the documented ENOSPC seam) and reports
//       disk_full / disk_errors / the .incomplete package.
//   pdeathsig-parent
//       Forks/execs itself as pdeathsig-child, prints its pid, then sleeps.
//   pdeathsig-child
//       Arms PR_SET_PDEATHSIG(SIGTERM) and sleeps until the parent dies.
//   clock-jump
//       Simulated clock jump at the trigger boundary: an out-of-window t0 in
//       both directions must be rejected (kInvalidTimestamp), an in-window
//       one accepted.

#include "thyme/record/record_mirror_ring_client.h"
#include "thyme/record/trigger_center.h"
#include "thyme/record/trigger_config.h"
#include "thyme/record/triggered_recorder.h"
#include "thyme/runtime/pinned_slot_store.h"
#include "thyme/runtime/parent_death.h"
#include "thyme/runtime/record_mirror_ring.h"
#include "thyme/runtime/shm_region.h"

#include <csignal>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {

using namespace thyme;

constexpr char kSelfPathEnv[] = "THYME_FAULT_SELF";

uint64_t NowNs() {
  struct timespec ts {};
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<uint64_t>(ts.tv_sec) * 1000000000ULL +
         static_cast<uint64_t>(ts.tv_nsec);
}

void SleepNs(uint64_t ns) {
  struct timespec ts {};
  ts.tv_sec = static_cast<time_t>(ns / 1000000000ULL);
  ts.tv_nsec = static_cast<long>(ns % 1000000000ULL);
  nanosleep(&ts, nullptr);
}

void PrintJson(const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  std::vfprintf(stdout, fmt, ap);
  va_end(ap);
  std::fputc('\n', stdout);
  std::fflush(stdout);
}

uint32_t FrameLen(uint64_t frame) {
  return 256u + static_cast<uint32_t>(frame % 4) * 128u;
}

void FillPayload(uint8_t* p, uint32_t len, uint64_t frame) {
  for (uint32_t j = 0; j < len; ++j) {
    p[j] = static_cast<uint8_t>(frame * 31 + j * 3);
  }
}

bool PayloadValid(const uint8_t* p, uint32_t len, uint64_t frame) {
  if (len != FrameLen(frame)) return false;
  for (uint32_t j = 0; j < len; ++j) {
    if (p[j] != static_cast<uint8_t>(frame * 31 + j * 3)) return false;
  }
  return true;
}

int RunProducer(const char* name, uint32_t slot_bytes, uint32_t slots,
                uint64_t max_frames) {
  PinnedSlotWriter writer;
  if (!writer.Create(name, slot_bytes, slots, 0)) {
    PrintJson("{\"error\":\"create failed\"}");
    return 2;
  }
  writer.SetCrcEnabled(true);
  PrintJson("{\"ready\":true,\"region_bytes\":%llu}",
            static_cast<unsigned long long>(
                PinnedSlotWriter::RegionBytes(slot_bytes, slots, 0)));
  uint64_t frame = 0;
  while (max_frames == 0 || frame < max_frames) {
    const uint32_t len = FrameLen(frame);
    PinnedSlotWriter::BeginView view = writer.Begin(len);
    if (view.payload == nullptr) {
      PrintJson("{\"error\":\"begin failed\",\"committed\":%llu}",
                static_cast<unsigned long long>(frame));
      return 3;
    }
    FillPayload(view.payload, len, frame);
    writer.Commit(1000 + frame);
    ++frame;
    if (frame % 32 == 0) {
      PrintJson("{\"committed\":%llu}", static_cast<unsigned long long>(frame));
    }
    SleepNs(200000);  // 200 us: ~5k frames/s, bounded CPU
  }
  PrintJson("{\"finished\":true,\"committed\":%llu}",
            static_cast<unsigned long long>(frame));
  return 0;
}

int RunReader(const char* name, uint64_t min_frames, bool follow) {
  PinnedSlotReader reader;
  if (!reader.Open(name, 1000000000ULL)) {
    PrintJson("{\"error\":\"open failed\"}");
    return 2;
  }
  if (follow) PrintJson("{\"attached\":true}");
  uint64_t frames = 0;
  uint64_t bad = 0;
  const uint64_t deadline = NowNs() + 1000000000ULL;
  for (;;) {
    PinnedSlotReader::View view;
    if (reader.TryRead(view)) {
      ++frames;
      if (!view.crc_ok || !PayloadValid(view.payload, view.data_len,
                                       view.frame_index)) {
        ++bad;
      }
      if (follow && frames % 32 == 0) {
        PrintJson("{\"frames\":%llu}", static_cast<unsigned long long>(frames));
      }
      continue;
    }
    if (follow) {
      SleepNs(1000000);
      continue;
    }
    if (NowNs() >= deadline) break;
    SleepNs(1000000);
  }
  const uint64_t head = reader.head();
  const uint64_t torn = reader.torn_read_count();
  const uint64_t drops = reader.drop_count();
  reader.Close();
  const bool ok = frames >= min_frames && bad == 0 && torn == 0;
  PrintJson("{\"frames\":%llu,\"head\":%llu,\"bad\":%llu,\"torn\":%llu,"
            "\"drops\":%llu,\"min_frames\":%llu,\"ok\":%s}",
            static_cast<unsigned long long>(frames),
            static_cast<unsigned long long>(head),
            static_cast<unsigned long long>(bad),
            static_cast<unsigned long long>(torn),
            static_cast<unsigned long long>(drops),
            static_cast<unsigned long long>(min_frames),
            ok ? "true" : "false");
  return ok ? 0 : 1;
}

int RunUnlink(const char* name) {
  ShmRegion::UnlinkName(name);
  ShmRegion::UnlinkName(PinnedSlotWriter::LeaseShmName(name).c_str());
  PrintJson("{\"unlinked\":true}");
  return 0;
}

int RunShmFull(const char* name, uint64_t bytes) {
  // Exceeding RLIMIT_FSIZE raises SIGXFSZ (default: kill the process) instead
  // of letting ftruncate report EFBIG. The kernel's ENOSPC path is SIGXFSZ +
  // EFBIG; ignore the signal so the library sees the error return.
  std::signal(SIGXFSZ, SIG_IGN);
  struct rlimit original {};
  if (getrlimit(RLIMIT_FSIZE, &original) != 0) {
    PrintJson("{\"error\":\"getrlimit\"}");
    return 2;
  }
  const rlim_t limit = 1024ULL * 1024ULL;  // 1 MiB: a full tmpfs, scaled down
  struct rlimit bound {};
  bound.rlim_cur = limit;
  bound.rlim_max = original.rlim_max;
  if (bound.rlim_cur > bound.rlim_max &&
      bound.rlim_max != RLIM_INFINITY) {
    bound.rlim_cur = bound.rlim_max;
  }
  const bool limit_set = setrlimit(RLIMIT_FSIZE, &bound) == 0;
  ShmRegion::UnlinkName(name);
  ShmRegion region;
  const bool created = region.Create(name, static_cast<std::size_t>(bytes));
  bool residue = created;
  if (!created) {
    ShmRegion probe;
    residue = probe.OpenRetry(name, 4096, 20000000ULL, true);
    probe.Unmap();
  }
  region.Unmap();
  // Restore the soft limit and prove the same geometry fits again.
  const bool restored = setrlimit(RLIMIT_FSIZE, &original) == 0;
  bool retry_ok = false;
  if (restored) {
    ShmRegion retry;
    retry_ok = retry.Create(name, static_cast<std::size_t>(bytes));
    retry.Unmap();
  }
  ShmRegion::UnlinkName(name);
  const bool ok = limit_set && !created && !residue && retry_ok;
  PrintJson("{\"limit_set\":%s,\"create_failed\":%s,\"residue\":%s,"
            "\"retry_ok\":%s,\"limit_bytes\":%llu,\"requested_bytes\":%llu,"
            "\"ok\":%s}",
            limit_set ? "true" : "false", created ? "false" : "true",
            residue ? "true" : "false", retry_ok ? "true" : "false",
            static_cast<unsigned long long>(limit),
            static_cast<unsigned long long>(bytes), ok ? "true" : "false");
  return ok ? 0 : 1;
}

bool AlwaysFailWrite(void*) { return true; }

int RunRecorderEnospc(const char* out_dir, const char* mirror_name) {
  ThymeStructDesc desc{};
  desc.struct_id = 0;
  desc.contract_version = 1;
  desc.payload_size = 8;
  std::snprintf(desc.name, sizeof(desc.name), "FaultPod");
  RecordMirrorRingSpec spec{8, 64};
  record_mirror::RingClient mirror;
  if (!mirror.Create(mirror_name, &desc, &spec, 1, 1)) {
    PrintJson("{\"error\":\"mirror create failed\"}");
    return 2;
  }
  for (uint64_t i = 0; i < 10; ++i) {
    const uint64_t mono = NowNs();
    (void)mirror.Page()->TryWriteRing(0, &i, sizeof(i), mono,
                                      static_cast<uint32_t>(i));
  }

  triggered::Config cfg;
  cfg.sources = {{triggered::SourceConfig::Kind::kMirrorRing, mirror_name, ""}};
  cfg.out_dir = out_dir;
  cfg.fault_inject_write = AlwaysFailWrite;
  triggered::TriggeredRecorder rec;
  if (!rec.Init(cfg)) {
    PrintJson("{\"error\":\"recorder init failed\"}");
    ShmRegion::UnlinkName(mirror_name);
    return 2;
  }
  const bool triggered = rec.Trigger(100, 60000000000ULL, 0);
  const uint64_t deadline = NowNs() + 3000000000ULL;
  while (!rec.disk_full() && NowNs() < deadline) {
    rec.Tick();
    SleepNs(1000000);
  }
  const std::string package = rec.last_package_dir();
  const bool incomplete = !package.empty() &&
                          access((package + ".incomplete").c_str(), F_OK) == 0;
  const bool sealed = !package.empty() &&
                      triggered::TriggeredRecorder::PackageComplete(package);
  const uint64_t disk_errors = rec.stats().disk_errors;
  const bool disk_full = rec.disk_full();
  rec.Shutdown();
  ShmRegion::UnlinkName(mirror_name);
  const bool ok = triggered && disk_full && disk_errors >= 1 && incomplete &&
                  !sealed;
  PrintJson("{\"triggered\":%s,\"disk_full\":%s,\"disk_errors\":%llu,"
            "\"incomplete\":%s,\"sealed\":%s,\"ok\":%s}",
            triggered ? "true" : "false", disk_full ? "true" : "false",
            static_cast<unsigned long long>(disk_errors),
            incomplete ? "true" : "false", sealed ? "true" : "false",
            ok ? "true" : "false");
  return ok ? 0 : 1;
}

int RunPdeathsigParent() {
  const char* self = std::getenv(kSelfPathEnv);
  if (self == nullptr || self[0] == '\0') {
    PrintJson("{\"error\":\"THYME_FAULT_SELF unset\"}");
    return 2;
  }
  const pid_t pid = fork();
  if (pid < 0) {
    PrintJson("{\"error\":\"fork failed\"}");
    return 2;
  }
  if (pid == 0) {
    execl(self, self, "pdeathsig-child", static_cast<char*>(nullptr));
    _exit(127);
  }
  PrintJson("{\"child\":%ld}", static_cast<long>(pid));
  for (;;) SleepNs(1000000000ULL);
}

int RunPdeathsigChild() {
  const ParentDeathSignal armed = InstallParentDeathSignal(SIGTERM);
  if (armed != ParentDeathSignal::kInstalled) {
    PrintJson("{\"error\":\"pdeathsig not installed\"}");
    return 2;
  }
  PrintJson("{\"armed\":true,\"pid\":%ld}", static_cast<long>(getpid()));
  for (;;) SleepNs(1000000000ULL);
}

const char* CodeName(trigger_proto::TriggerCode code) {
  using trigger_proto::TriggerCode;
  switch (code) {
    case TriggerCode::kAccepted:
      return "kAccepted";
    case TriggerCode::kInvalidTimestamp:
      return "kInvalidTimestamp";
    case TriggerCode::kBusy:
      return "kBusy";
    case TriggerCode::kMuted:
      return "kMuted";
    case TriggerCode::kQuotaExceeded:
      return "kQuotaExceeded";
    case TriggerCode::kUnknownSource:
      return "kUnknownSource";
    case TriggerCode::kCancelled:
      return "kCancelled";
    case TriggerCode::kUnknownTrigger:
      return "kUnknownTrigger";
    default:
      break;
  }
  return "other";
}

bool HookReady(void*) { return true; }
bool HookActive(void*) { return false; }
bool HookStart(void*, uint64_t, uint64_t, uint64_t, uint64_t, bool, bool,
               const std::vector<std::string>&) {
  return true;
}
bool HookPreempt(void*) { return false; }

int RunClockJump() {
  trigger_cfg::CenterConfig cfg;
  char socket_path[96];
  std::snprintf(socket_path, sizeof(socket_path), "/tmp/thyme_fault_ts_%ld.sock",
                static_cast<long>(getpid()));
  (void)unlink(socket_path);
  cfg.socket_path = socket_path;
  trigger_cfg::SourceRule manual;
  manual.name = "manual";
  manual.priority = 10;
  manual.before_ns = 1000000000ULL;  // 1 s
  manual.after_ns = 1000000000ULL;
  manual.quota_class = "manual";
  cfg.sources = {manual};
  cfg.quota_limits = {{"manual", 0}};
  cfg.timestamp_future_tolerance_ns = 5000000000ULL;  // 5 s

  trigger_center::Hooks hooks;
  hooks.recorder_ready = HookReady;
  hooks.capture_active = HookActive;
  hooks.start_capture = HookStart;
  hooks.preempt_capture = HookPreempt;
  trigger_center::Center center;
  if (!center.Init(cfg, hooks)) {
    PrintJson("{\"error\":\"center init failed\"}");
    return 2;
  }

  const uint64_t now = NowNs();
  // A backward wall-clock jump (t0 far behind) and a forward one (beyond the
  // future tolerance) are both rejected; an in-window trigger is accepted.
  const trigger_center::SubmitResult backward =
      center.Submit("manual", -1, now - 10000000000ULL, true, false);
  const trigger_center::SubmitResult forward =
      center.Submit("manual", -1, now + 60000000000ULL, true, false);
  const trigger_center::SubmitResult in_window =
      center.Submit("manual", -1, now - 500000000ULL, true, false);
  const uint64_t rejected_bad = center.stats().rejected_bad_timestamp;
  center.Shutdown();
  // Center shutdown closes the control socket; the audit/FSM side files live
  // next to it and are ours to remove (the driver asserts no residue).
  char side[128];
  (void)unlink(socket_path);
  std::snprintf(side, sizeof(side), "%s.audit", socket_path);
  (void)unlink(side);
  std::snprintf(side, sizeof(side), "%s.fsm", socket_path);
  (void)unlink(side);

  const bool ok = backward.code == trigger_proto::TriggerCode::kInvalidTimestamp &&
                  forward.code == trigger_proto::TriggerCode::kInvalidTimestamp &&
                  in_window.code == trigger_proto::TriggerCode::kAccepted &&
                  rejected_bad == 2;
  PrintJson("{\"backward\":\"%s\",\"forward\":\"%s\",\"in_window\":\"%s\","
            "\"rejected_bad_timestamp\":%llu,\"ok\":%s}",
            CodeName(backward.code), CodeName(forward.code),
            CodeName(in_window.code),
            static_cast<unsigned long long>(rejected_bad),
            ok ? "true" : "false");
  return ok ? 0 : 1;
}

std::string SelfPath(int argc, char** argv) {
  if (argc > 0 && argv[0] != nullptr) return std::string(argv[0]);
  const char* env = std::getenv(kSelfPathEnv);
  return env != nullptr ? std::string(env) : std::string();
}

}  // namespace

int main(int argc, char** argv) {
  if (argc >= 2) {
    const char* mode = argv[1];
    // Re-export the executable path so a re-exec (pdeathsig-parent) works even
    // when the driver spawned us through a relative path.
    const std::string self = SelfPath(argc, argv);
    if (!self.empty()) (void)setenv(kSelfPathEnv, self.c_str(), 1);
    if (std::strcmp(mode, "producer") == 0 && argc == 6) {
      return RunProducer(argv[2], static_cast<uint32_t>(std::strtoul(argv[3], nullptr, 10)),
                         static_cast<uint32_t>(std::strtoul(argv[4], nullptr, 10)),
                         std::strtoull(argv[5], nullptr, 10));
    }
    if (std::strcmp(mode, "reader") == 0 && (argc == 4 || argc == 5)) {
      const bool follow = argc == 5 && std::strcmp(argv[4], "follow") == 0;
      return RunReader(argv[2], std::strtoull(argv[3], nullptr, 10), follow);
    }
    if (std::strcmp(mode, "unlink") == 0 && argc == 3) {
      return RunUnlink(argv[2]);
    }
    if (std::strcmp(mode, "shm-full") == 0 && argc == 4) {
      return RunShmFull(argv[2], std::strtoull(argv[3], nullptr, 10));
    }
    if (std::strcmp(mode, "recorder-enospc") == 0 && argc == 4) {
      return RunRecorderEnospc(argv[2], argv[3]);
    }
    if (std::strcmp(mode, "pdeathsig-parent") == 0 && argc == 2) {
      return RunPdeathsigParent();
    }
    if (std::strcmp(mode, "pdeathsig-child") == 0 && argc == 2) {
      return RunPdeathsigChild();
    }
    if (std::strcmp(mode, "clock-jump") == 0 && argc == 2) {
      return RunClockJump();
    }
  }
  std::fprintf(stderr,
               "usage: fault_role <producer|reader|unlink|shm-full|"
               "recorder-enospc|pdeathsig-parent|pdeathsig-child|clock-jump> "
               "[args...]\n");
  return 2;
}
