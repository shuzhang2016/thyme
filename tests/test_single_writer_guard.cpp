#include "thyme/check.h"
#include "thyme/runtime/latest_slot.h"
#include "thyme/runtime/shm_spmc_ring.h"
#include "thyme/runtime/shm_spsc_ring.h"

#ifndef NDEBUG

#include <sys/wait.h>
#include <unistd.h>

#include <csignal>
#include <cstdint>
#include <cstdio>
#include <string>
#include <thread>

using namespace thyme;

namespace {

struct Pod {
  uint32_t value;
};

template <typename Fn>
std::string ExpectAbort(Fn fn, const char* primitive, const char* operation) {
  int pipefd[2];
  CHECK(pipe(pipefd) == 0);
  const pid_t pid = fork();
  CHECK(pid >= 0);
  if (pid == 0) {
    close(pipefd[0]);
    CHECK(dup2(pipefd[1], STDERR_FILENO) == STDERR_FILENO);
    close(pipefd[1]);
    fn();
    _exit(0);
  }

  close(pipefd[1]);
  std::string diagnostic;
  char buffer[1024];
  for (;;) {
    const ssize_t count = read(pipefd[0], buffer, sizeof(buffer));
    if (count == 0) {
      break;
    }
    CHECK(count > 0);
    diagnostic.append(buffer, static_cast<std::size_t>(count));
  }
  close(pipefd[0]);
  int status = 0;
  CHECK(waitpid(pid, &status, 0) == pid);
  CHECK(WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT);
  CHECK(diagnostic.find("thyme single-writer violation") != std::string::npos);
  CHECK(diagnostic.find(primitive) != std::string::npos);
  CHECK(diagnostic.find(operation) != std::string::npos);
  CHECK(diagnostic.find("page=0x") != std::string::npos);
  CHECK(diagnostic.find("active_slot=0x") != std::string::npos);
  CHECK(diagnostic.find("active_publish=") != std::string::npos);
  CHECK(diagnostic.find("owner_token=") != std::string::npos);
  CHECK(diagnostic.find("contender_token=") != std::string::npos);
  return diagnostic;
}

void TestLatestSlotRejections() {
  ExpectAbort(
      [] {
        LatestSlot<Pod> slot;
        slot.BeginWrite();
        slot.BeginWrite();
      },
      "LatestSlot", "BeginWrite");

  ExpectAbort(
      [] {
        LatestSlot<Pod> slot;
        slot.BeginWrite();
        std::thread contender([&] { slot.BeginWrite(); });
        contender.join();
      },
      "LatestSlot", "BeginWrite");

  ExpectAbort(
      [] {
        LatestSlot<Pod> slot;
        slot.BeginWrite();
        std::thread contender([&] { slot.CommitWrite(); });
        contender.join();
      },
      "LatestSlot", "CommitWrite");

  ExpectAbort(
      [] {
        LatestSlot<Pod> slot;
        slot.BeginWrite();
        std::thread contender([&] { slot.AbortWrite(); });
        contender.join();
      },
      "LatestSlot", "AbortWrite");
}

void TestRingRejections() {
  char spsc_name[64];
  std::snprintf(spsc_name, sizeof(spsc_name), "/c9swg_spsc_%d",
                static_cast<int>(getpid()));
  ShmRegion::UnlinkName(spsc_name);
  ExpectAbort(
      [&] {
        ShmSpscRing<Pod, 2> ring;
        CHECK(ring.Create(spsc_name));
        ring.BeginWrite();
        std::thread contender([&] { ring.BeginWrite(); });
        contender.join();
      },
      "ShmSpscRing", "BeginWrite");
  ShmRegion::UnlinkName(spsc_name);

  char spmc_name[64];
  std::snprintf(spmc_name, sizeof(spmc_name), "/c9swg_spmc_%d",
                static_cast<int>(getpid()));
  ShmRegion::UnlinkName(spmc_name);
  ExpectAbort(
      [&] {
        ShmSpmcRing<Pod, 2> ring;
        CHECK(ring.Create(spmc_name));
        ring.BeginWrite();
        std::thread contender([&] { ring.BeginWrite(); });
        contender.join();
      },
      "ShmSpmcRing", "BeginWrite");
  ShmRegion::UnlinkName(spmc_name);
}

void TestMisuseRejections() {
  ExpectAbort(
      [] {
        LatestSlot<Pod> slot;
        slot.BeginWrite();
        slot.CommitWrite();
        slot.CommitWrite();
      },
      "LatestSlot", "CommitWrite");
  ExpectAbort(
      [] {
        LatestSlot<Pod> slot;
        slot.BeginWrite();
        slot.AbortWrite();
        slot.CommitWrite();
      },
      "LatestSlot", "CommitWrite");
  ExpectAbort(
      [] {
        LatestSlot<Pod> slot;
        slot.CommitWrite();
      },
      "LatestSlot", "CommitWrite");
  ExpectAbort(
      [] {
        LatestSlot<Pod> slot;
        slot.BeginWrite();
        slot.AbortWrite();
        slot.AbortWrite();
      },
      "LatestSlot", "AbortWrite");
  ExpectAbort(
      [] {
        LatestSlot<Pod> slot;
        Pod* const pending = slot.BeginWrite();
        detail::SingleWriterGuard::Check("CommitWrite", "ShmSpmcRing", &slot,
                                         pending, 1);
      },
      "ShmSpmcRing", "CommitWrite");
  ExpectAbort(
      [] {
        LatestSlot<Pod> slot;
        (void)slot.BeginWrite();
        Pod wrong_slot{};
        detail::SingleWriterGuard::Check("CommitWrite", "LatestSlot", &slot,
                                         &wrong_slot, 1);
      },
      "LatestSlot", "CommitWrite");
  ExpectAbort(
      [] {
        LatestSlot<Pod> slot;
        Pod* const pending = slot.BeginWrite();
        detail::SingleWriterGuard::Check("CommitWrite", "LatestSlot", &slot,
                                         pending, 2);
      },
      "LatestSlot", "CommitWrite");
}

void TestSequentialHandoff() {
  LatestSlot<Pod> slot;
  std::thread first([&] { slot.Write(Pod{1}); });
  first.join();
  std::thread second([&] { slot.Write(Pod{2}); });
  second.join();
  Pod out{};
  CHECK(slot.Read(out) && out.value == 2);
  first = std::thread([&] {
    slot.BeginWrite();
    slot.AbortWrite();
  });
  first.join();
  second = std::thread([&] { slot.Write(Pod{3}); });
  second.join();
  CHECK(slot.Read(out) && out.value == 3);
}

void TestSpscFailureAndHandoff() {
  char name[64];
  std::snprintf(name, sizeof(name), "/c9swg_handoff_%d",
                static_cast<int>(getpid()));
  ShmRegion::UnlinkName(name);
  ShmSpscRing<Pod, 2> producer;
  ShmSpscRing<Pod, 2> consumer;
  CHECK(producer.Create(name));
  CHECK(consumer.Open(name, 1000000000ULL));

  std::thread first([&] { CHECK(producer.Push(Pod{1})); });
  first.join();
  std::thread second([&] { CHECK(producer.Push(Pod{2})); });
  second.join();
  CHECK(producer.BeginWrite() == nullptr);
  Pod out{};
  CHECK(consumer.TryPop(out) && out.value == 1);
  second = std::thread([&] {
    Pod* const pending = producer.BeginWrite();
    CHECK(pending != nullptr);
    producer.AbortWrite();
  });
  second.join();
  CHECK(producer.Push(Pod{3}));
  CHECK(consumer.TryPop(out) && out.value == 2);
  CHECK(consumer.TryPop(out) && out.value == 3);

  producer.Close();
  consumer.Close();
  ShmRegion::UnlinkName(name);
}

void TestSpmcHandoff() {
  char name[64];
  std::snprintf(name, sizeof(name), "/c9swg_spmc_handoff_%d",
                static_cast<int>(getpid()));
  ShmRegion::UnlinkName(name);
  ShmSpmcRing<Pod, 2> ring;
  CHECK(ring.Create(name));
  std::thread first([&] { ring.Push(Pod{1}); });
  first.join();
  std::thread second([&] { ring.Push(Pod{2}); });
  second.join();
  first = std::thread([&] {
    ring.BeginWrite();
    ring.AbortWrite();
  });
  first.join();
  second = std::thread([&] { ring.Push(Pod{4}); });
  second.join();
  CHECK(ring.layout()->head.load(std::memory_order_relaxed) == 3);
  ring.Close();
  ShmRegion::UnlinkName(name);
}

}  // namespace

#endif

int main() {
#ifndef NDEBUG
  TestLatestSlotRejections();
  TestRingRejections();
  TestMisuseRejections();
  TestSequentialHandoff();
  TestSpscFailureAndHandoff();
  TestSpmcHandoff();
#endif
  return 0;
}
