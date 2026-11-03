// TRG-15: a second process applies a parameterized record_control trigger.
// The datagram carries the window and the channel name. The frame stays in
// the big-frame ring and is harvested from shared memory.
#include "thyme/check.h"
#include "thyme/record/record_control.h"
#include "thyme/record/triggered_recorder.h"
#include "thyme/runtime/pinned_slot_store.h"
#include "thyme/runtime/session_lease.h"
#include "thyme/runtime/shm_region.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

using namespace thyme;

namespace {

uint64_t NowNs() {
  struct timespec ts {};
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<uint64_t>(ts.tv_sec) * 1000000000ULL +
         static_cast<uint64_t>(ts.tv_nsec);
}

void Publish(PinnedSlotWriter &w, uint8_t mark, uint32_t len) {
  PinnedSlotWriter::BeginView bv = w.Begin(len);
  CHECK(bv.payload != nullptr);
  std::memset(bv.payload, 0x5A, len);
  bv.payload[0] = mark;
  w.Commit(NowNs());
}

bool FileHasMark(const std::string &path, uint8_t mark) {
  FILE *fp = std::fopen(path.c_str(), "rb");
  if (fp == nullptr) return false;
  uint8_t buf[256];
  bool found = false;
  while (!found) {
    const std::size_t n = std::fread(buf, 1, sizeof(buf), fp);
    if (n == 0) break;
    for (std::size_t i = 0; i < n; ++i) {
      if (buf[i] == mark) found = true;
    }
  }
  std::fclose(fp);
  return found;
}

[[noreturn]] void RecorderChild(const char *shm, const char *sock,
                                const char *out, int stamp_fd) {
  triggered::Config cfg;
  cfg.out_dir = out;
  cfg.reattach_scan_ns = 1000000;
  cfg.post_grace_ns = 2000000ULL;
  triggered::SourceConfig sc;
  sc.kind = triggered::SourceConfig::Kind::kPinnedSlot;
  sc.shm_name = shm;
  sc.channel_name = "cam";
  cfg.sources.push_back(sc);
  triggered::TriggeredRecorder rec;
  if (!rec.Init(cfg)) _exit(2);
  record_control::Server control;
  record_control::Config ccfg;
  ccfg.require_peer_pid = true;
  if (!control.Open(sock, ccfg)) _exit(3);

  const uint64_t deadline = NowNs() + 2000000000ULL;
  record_control::TriggerParams params{};
  std::string channels;
  bool got = false;
  uint64_t seen_ns = 0;
  while (NowNs() < deadline) {
    record_control::ControlCmd cmd = record_control::ControlCmd::kNone;
    if (!control.Poll(cmd)) continue;
    if (cmd != record_control::ControlCmd::kTrigger) continue;
    if (!control.TakeTrigger(&params, &channels)) continue;
    seen_ns = NowNs();
    uint64_t wake = 0;
    if (control.WakeFd() < 0 ||
        read(control.WakeFd(), &wake, sizeof(wake)) !=
            static_cast<ssize_t>(sizeof(wake)) ||
        wake == 0) {
      _exit(4);
    }
    got = true;
    break;
  }
  if (!got) _exit(5);
  const ssize_t wrote = write(stamp_fd, &seen_ns, sizeof(seen_ns));
  if (wrote != static_cast<ssize_t>(sizeof(seen_ns))) _exit(6);

  std::vector<std::string> selected;
  if (!channels.empty()) selected.push_back(channels);
  if (!rec.Trigger(params.trigger_id, params.before_ns, params.after_ns,
                   params.t0_mono_ns,
                   (params.flags & record_control::kTriggerParamFlagRealtime) ==
                       0,
                   (params.flags & record_control::kTriggerParamFlagHigh) != 0,
                   selected)) {
    _exit(7);
  }
  const uint64_t seal_deadline = NowNs() + 2000000000ULL;
  while (rec.phase() != triggered::Phase::kArmed && NowNs() < seal_deadline) {
    rec.Tick();
  }
  const bool sealed =
      rec.phase() == triggered::Phase::kArmed && rec.stats().captures_done == 1;
  const std::string pkg = rec.last_package_dir();
  rec.Shutdown();
  control.Close();
  if (!sealed || !triggered::TriggeredRecorder::PackageComplete(pkg)) _exit(8);
  if (!FileHasMark(pkg + "/frames/cam.raw", 0xA5)) _exit(9);
  _exit(0);
}

void TestSplitTriggerDoesNotCarryTheFrame() {
  const int me = static_cast<int>(getpid());
  char shm[96];
  char sock[96];
  char out[96];
  std::snprintf(shm, sizeof(shm), "/c9split_%d", me);
  std::snprintf(sock, sizeof(sock), "/tmp/c9split_%d.sock", me);
  std::snprintf(out, sizeof(out), "/tmp/c9split_%d", me);
  ShmRegion::UnlinkName(shm);
  unlink(sock);
  std::string rm = std::string("rm -rf ") + out;
  CHECK(std::system(rm.c_str()) == 0);

  PinnedSlotWriter writer;
  CHECK(writer.Create(shm, 2048, 8, 0));
  const uint32_t frame_len = 800;
  Publish(writer, 0xA5, frame_len);

  int pipes[2];
  CHECK(pipe(pipes) == 0);
  const pid_t child = fork();
  CHECK(child >= 0);
  if (child == 0) {
    close(pipes[0]);
    RecorderChild(shm, sock, out, pipes[1]);
  }
  close(pipes[1]);

  record_control::TriggerParams params{};
  params.trigger_id = 7;
  params.before_ns = 10000000000ULL;
  params.flags = record_control::kTriggerParamFlagRealtime;
  const char *channel = "cam";
  params.channel_bytes = 3;
  record_control::Result sent;
  const uint64_t send_deadline = NowNs() + 2000000000ULL;
  do {
    sent = record_control::SendTrigger(sock, params, channel, 200);
    if (sent.transport_ok) break;
    usleep(2000);
  } while (NowNs() < send_deadline);
  CHECK(sent.transport_ok);
  CHECK(sent.code == record_control::ControlCode::kAccepted);
  const std::size_t datagram =
      sizeof(record_control::ControlRequest) +
      sizeof(record_control::TriggerParams) + params.channel_bytes;
  CHECK(datagram < frame_len);

  uint64_t seen_ns = 0;
  CHECK(read(pipes[0], &seen_ns, sizeof(seen_ns)) ==
        static_cast<ssize_t>(sizeof(seen_ns)));
  CHECK(seen_ns >= sent.sent_mono_ns);
  const uint64_t wake_ns = seen_ns - sent.sent_mono_ns;
  std::printf("recorder split wake %llu ns\n",
              static_cast<unsigned long long>(wake_ns));

  int status = 0;
  CHECK(waitpid(child, &status, 0) == child);
  CHECK(WIFEXITED(status));
  CHECK(WEXITSTATUS(status) == 0);

  close(pipes[0]);
  writer.Close();
  ShmRegion::UnlinkName(shm);
  unlink(sock);
  char lease[64];
  if (SessionLeaseNameForRoot(out, lease, sizeof(lease))) {
    ShmRegion::UnlinkName(lease);
  }
  CHECK(std::system(rm.c_str()) == 0);
}

}  // namespace

int main() {
  TestSplitTriggerDoesNotCarryTheFrame();
  std::printf("test_recorder_split: all checks passed\n");
  return 0;
}
