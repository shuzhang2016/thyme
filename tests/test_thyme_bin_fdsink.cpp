#include "thyme/check.h"
#include "thyme/record/thyme_bin.h"

#include <vector>

#if !defined(_WIN32)
#include <cerrno>
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

using namespace thyme;

#if !defined(_WIN32)
namespace {

// Drains everything currently readable on `fd` (non-blocking) and returns the
// byte count. Used to prove a disabled sink no longer appends to the wire.
uint64_t DrainAll(int fd) {
  uint64_t total = 0;
  char buf[4096];
  for (;;) {
    const ssize_t n = read(fd, buf, sizeof(buf));
    if (n > 0) {
      total += static_cast<uint64_t>(n);
    } else {
      break;  // 0 = EOF, -1 = EAGAIN on a non-blocking fd
    }
  }
  return total;
}

// Drains until two consecutive sweeps find nothing new: bytes already inside
// the socketpair send buffer arrive at the peer with a small delay, so a
// single DrainAll can return before the in-flight remainder lands.
uint64_t DrainStable(int fd) {
  uint64_t total = 0;
  for (int i = 0; i < 20; ++i) {
    const uint64_t before = total;
    total += DrainAll(fd);
    if (total == before) break;
    usleep(1000);
  }
  return total;
}

void SetNonBlocking(int fd) {
  const int flags = fcntl(fd, F_GETFL, 0);
  CHECK(flags >= 0);
  CHECK(fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0);
}

}  // namespace
#endif

// A sink that fails mid-write (partial frame on the wire) must disable itself
// so the corrupt stream cannot be continued: after the failure, Write() fails
// immediately and no further bytes reach the wire. Both the non-owned and the
// owned path must behave identically; the owned path additionally closes its
// fd.
int main() {
#if defined(_WIN32)
  return 0;
#else
  // ---- Non-owned sink ----
  {
    int sv[2];
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    int sndbuf = 4096;
    setsockopt(sv[0], SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));
    SetNonBlocking(sv[1]);

    ThymeBinFdSink sink(sv[0], /*owned=*/false, /*non_blocking=*/true);
    CHECK(sink.ok());
    const std::vector<uint8_t> big(1u << 20, 0xAB);
    // Nobody drains sv[1]: the send buffer fills mid-write, so the first send()
    // succeeds partially and the next one returns EAGAIN.
    CHECK(!sink.Write(big.data(), big.size()));
    CHECK(sink.write_failures() >= 1);
    CHECK(!sink.ok());

    // Drain everything the first (partial) write managed to push, then prove
    // the disabled sink never appends again.
    const uint64_t received_before = DrainStable(sv[1]);
    CHECK(received_before > 0);  // baseline: the partial write did deliver data
    CHECK(!sink.Write(big.data(), big.size()));  // fails immediately
    usleep(20000);  // give any (nonexistent) in-flight bytes time to land
    CHECK(DrainAll(sv[1]) == 0);

    // Non-owned: the fd is *disabled* but not closed by the sink.
    CHECK(fcntl(sv[0], F_GETFL) >= 0);
    close(sv[0]);
    close(sv[1]);
  }

  // ---- Owned sink ----
  {
    int sv[2];
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    int sndbuf = 4096;
    setsockopt(sv[0], SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));
    SetNonBlocking(sv[1]);

    ThymeBinFdSink sink(sv[0], /*owned=*/true, /*non_blocking=*/true);
    CHECK(sink.ok());
    const std::vector<uint8_t> big(1u << 20, 0xCD);
    CHECK(!sink.Write(big.data(), big.size()));
    CHECK(sink.write_failures() >= 1);
    CHECK(!sink.ok());

    (void)DrainStable(sv[1]);
    CHECK(!sink.Write(big.data(), big.size()));
    usleep(20000);
    CHECK(DrainAll(sv[1]) == 0);

    // Owned: the sink closed its fd; the number must be free again.
    errno = 0;
    CHECK(fcntl(sv[0], F_GETFL) == -1);
    CHECK(errno == EBADF);
    // The peer end sees EOF.
    char buf[1];
    CHECK(read(sv[1], buf, sizeof(buf)) == 0);
    close(sv[1]);
  }

  return 0;
#endif
}
