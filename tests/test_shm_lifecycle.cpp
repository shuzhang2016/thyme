// OPS-02: stale SHM reclamation and the parent-death signal helper.
//
// (a) a segment held by a live process (fd + mapping) is never touched,
// (b) a segment left behind by an exited creator is proved stale and swept
//     (and ShmRegion::Create reclaims it automatically),
// (c) the PDEATHSIG helper is armed on Linux and a no-op elsewhere.
#include "thyme/check.h"
#include "thyme/runtime/parent_death.h"
#include "thyme/runtime/shm_lifecycle.h"
#include "thyme/runtime/shm_region.h"

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

using namespace thyme;

namespace {

bool SegmentExists(const char* name) {
  const int fd = shm_open(name, O_RDONLY, 0);
  if (fd >= 0) {
    close(fd);
    return true;
  }
  return errno != ENOENT;
}

void WaitForExit(pid_t pid) {
  int status = 0;
  while (waitpid(pid, &status, 0) == -1 && errno == EINTR) {
  }
  CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

// Child creates the segment with a ShmRegion and exits, leaving the node
// behind with no live holder.
bool CreateInChildAndExit(const char* name) {
  const pid_t pid = fork();
  if (pid == 0) {
    ShmRegion r;
    if (!r.Create(name, 4096)) _exit(3);
    _exit(0);
  }
  if (pid < 0) return false;
  int status = 0;
  while (waitpid(pid, &status, 0) == -1 && errno == EINTR) {
  }
  return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

// (a) A live segment is reported live and left in place.
void TestLiveSegmentLeftAlone(const char* name) {
  ShmRegion::UnlinkName(name);
  ShmRegion live;
  CHECK(live.Create(name, 4096));
  CHECK(ProbeShmSegmentLiveness(name) == ShmLiveness::kLive);

  ShmSweepStats stats{};
  CHECK(SweepStaleShmSegment(name, &stats) == ShmSweepResult::kLive);
  CHECK(stats.examined == 1);
  CHECK(stats.skipped_live == 1);
  CHECK(stats.swept == 0);
  CHECK(SegmentExists(name));

  live.Unmap();
  ShmRegion::UnlinkName(name);
}

// (b) A same-name segment left by an exited creator is swept, and the
// ordinary create path reclaims it by itself.
void TestStaleSegmentSwept(const char* name) {
  ShmRegion::UnlinkName(name);
  CHECK(CreateInChildAndExit(name));
  CHECK(SegmentExists(name));  // the shm node outlived its creator
  CHECK(ProbeShmSegmentLiveness(name) == ShmLiveness::kStale);

  ShmSweepStats stats{};
  CHECK(SweepStaleShmSegment(name, &stats) == ShmSweepResult::kSwept);
  CHECK(stats.examined == 1);
  CHECK(stats.swept == 1);
  CHECK(!SegmentExists(name));

  // End-to-end: Create() sees EEXIST, proves staleness and reclaims.
  CHECK(CreateInChildAndExit(name));
  CHECK(SegmentExists(name));
  ShmRegion reclaimed;
  CHECK(reclaimed.Create(name, 4096));
  reclaimed.Unmap();
  ShmRegion::UnlinkName(name);
}

// A crashed *peer* is live until it dies: sweep must not reclaim while the
// child still holds the mapping open.
void TestLivePeerBlocksSweepUntilExit(const char* name) {
  ShmRegion::UnlinkName(name);
  int ready[2];
  CHECK(pipe(ready) == 0);
  const pid_t pid = fork();
  CHECK(pid >= 0);
  if (pid == 0) {
    close(ready[0]);
    ShmRegion r;
    if (!r.Create(name, 4096)) _exit(3);
    const char b = 'x';
    const ssize_t wr = write(ready[1], &b, 1);
    (void)wr;
    pause();
    _exit(0);
  }
  close(ready[1]);
  char b = 0;
  CHECK(read(ready[0], &b, 1) == 1);
  close(ready[0]);
  CHECK(SegmentExists(name));
  CHECK(SweepStaleShmSegment(name) == ShmSweepResult::kLive);
  CHECK(SegmentExists(name));

  CHECK(kill(pid, SIGKILL) == 0);
  int status = 0;
  while (waitpid(pid, &status, 0) == -1 && errno == EINTR) {
  }
  CHECK(WIFSIGNALED(status));
  CHECK(SweepStaleShmSegment(name) == ShmSweepResult::kSwept);
  CHECK(!SegmentExists(name));
}

void TestAbsentSegment(const char* name) {
  ShmRegion::UnlinkName(name);
  CHECK(ProbeShmSegmentLiveness(name) == ShmLiveness::kAbsent);
  ShmSweepStats stats{};
  CHECK(SweepStaleShmSegment(name, &stats) == ShmSweepResult::kNotPresent);
  CHECK(stats.examined == 1);
  CHECK(stats.swept == 0);
}

// (c) On Linux the helper arms PR_SET_PDEATHSIG in the calling process;
// elsewhere it is documented as an unsupported no-op. Also verify it works
// from a freshly forked child (the supervisor launch path).
void TestParentDeathSignal() {
#ifdef __linux__
  CHECK(InstallParentDeathSignal(SIGTERM) == ParentDeathSignal::kInstalled);
  const pid_t pid = fork();
  CHECK(pid >= 0);
  if (pid == 0) {
    _exit(InstallParentDeathSignal(SIGTERM) == ParentDeathSignal::kInstalled
              ? 0
              : 1);
  }
  WaitForExit(pid);
#else
  CHECK(InstallParentDeathSignal(SIGTERM) ==
        ParentDeathSignal::kUnsupported);
#endif
}

}  // namespace

int main() {
  char name[64];
  std::snprintf(name, sizeof(name), "/ops02live_%d",
                static_cast<int>(getpid()));
  TestLiveSegmentLeftAlone(name);

  std::snprintf(name, sizeof(name), "/ops02stale_%d",
                static_cast<int>(getpid()));
  TestStaleSegmentSwept(name);

  std::snprintf(name, sizeof(name), "/ops02peer_%d",
                static_cast<int>(getpid()));
  TestLivePeerBlocksSweepUntilExit(name);

  std::snprintf(name, sizeof(name), "/ops02none_%d",
                static_cast<int>(getpid()));
  TestAbsentSegment(name);

  TestParentDeathSignal();
  return 0;
}
