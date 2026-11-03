// OPS-03: the gated sd_notify shim. Covers the wire contract (single
// "READY=1" / "WATCHDOG=1" datagrams), the filesystem and Linux abstract
// NOTIFY_SOCKET spellings, the THYME_SD_NOTIFY gate, and the non-fatal
// failure modes (missing socket, over-long path, bad input).
#include "thyme/check.h"
#include "thyme/app/systemd_notify.h"

#include <cerrno>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

namespace {

using thyme::app::SystemdNotifyEnabled;
using thyme::app::SystemdNotifyReady;
using thyme::app::SystemdNotifySend;
using thyme::app::SystemdNotifyWatchdog;

std::string TempDir() {
  char tmpl[] = "/tmp/thyme_notify_XXXXXX";
  char* dir = ::mkdtemp(tmpl);
  CHECK(dir != nullptr);
  return std::string(dir);
}

void SetRecvTimeout(int fd, int ms) {
  struct timeval tv {};
  tv.tv_sec = ms / 1000;
  tv.tv_usec = static_cast<suseconds_t>((ms % 1000) * 1000);
  CHECK(::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) == 0);
}

int BindFilesystemSocket(const std::string& path) {
  const int fd = ::socket(AF_UNIX, SOCK_DGRAM, 0);
  CHECK(fd >= 0);
  (void)::unlink(path.c_str());
  struct sockaddr_un addr {};
  addr.sun_family = AF_UNIX;
  CHECK(std::strlen(path.c_str()) < sizeof(addr.sun_path));
  std::snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path.c_str());
  CHECK(::bind(fd, reinterpret_cast<const struct sockaddr*>(&addr),
               sizeof(addr)) == 0);
  SetRecvTimeout(fd, 2000);
  return fd;
}

// Linux abstract-namespace NOTIFY_SOCKET spelling ("@name").
#if defined(__linux__)
int BindAbstractSocket(const std::string& name) {
  const int fd = ::socket(AF_UNIX, SOCK_DGRAM, 0);
  CHECK(fd >= 0);
  struct sockaddr_un addr {};
  addr.sun_family = AF_UNIX;
  addr.sun_path[0] = '\0';
  CHECK(name.size() + 1 < sizeof(addr.sun_path));
  std::memcpy(addr.sun_path + 1, name.data(), name.size());
  const socklen_t len = static_cast<socklen_t>(
      offsetof(struct sockaddr_un, sun_path) + 1 + name.size());
  CHECK(::bind(fd, reinterpret_cast<const struct sockaddr*>(&addr), len) == 0);
  SetRecvTimeout(fd, 2000);
  return fd;
}
#endif

std::string RecvDatagram(int fd, bool* ok) {
  char buf[128];
  const ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
  *ok = n >= 0;
  if (n < 0) return std::string();
  return std::string(buf, static_cast<std::size_t>(n));
}

// True when no datagram arrives within a short window (EAGAIN/EWOULDBLOCK).
bool NoDatagram(int fd) {
  SetRecvTimeout(fd, 200);
  char buf[8];
  const ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
  const bool timeout = n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK);
  SetRecvTimeout(fd, 2000);
  return timeout;
}

void TestFilesystemSocket() {
  const std::string dir = TempDir();
  const std::string path = dir + "/notify.sock";
  const int fd = BindFilesystemSocket(path);

  CHECK(::setenv("NOTIFY_SOCKET", path.c_str(), 1) == 0);
  CHECK(::setenv("THYME_SD_NOTIFY", "1", 1) == 0);
  CHECK(SystemdNotifyEnabled());

  bool ok = false;
  CHECK(SystemdNotifyReady());
  CHECK(RecvDatagram(fd, &ok) == "READY=1");
  CHECK(ok);
  CHECK(SystemdNotifyWatchdog());
  CHECK(RecvDatagram(fd, &ok) == "WATCHDOG=1");
  CHECK(ok);

  // Accepted truthy spellings.
  CHECK(::setenv("THYME_SD_NOTIFY", "yes", 1) == 0);
  CHECK(SystemdNotifyEnabled());
  CHECK(::setenv("THYME_SD_NOTIFY", "true", 1) == 0);
  CHECK(SystemdNotifyEnabled());
  CHECK(::setenv("THYME_SD_NOTIFY", "on", 1) == 0);
  CHECK(SystemdNotifyEnabled());

  // Anything else disables both sends; nothing may reach the socket.
  CHECK(::setenv("THYME_SD_NOTIFY", "0", 1) == 0);
  CHECK(!SystemdNotifyEnabled());
  CHECK(!SystemdNotifyReady());
  CHECK(!SystemdNotifyWatchdog());
  CHECK(NoDatagram(fd));

  // NOTIFY_SOCKET missing: disabled even when the gate is on.
  CHECK(::unsetenv("NOTIFY_SOCKET") == 0);
  CHECK(::setenv("THYME_SD_NOTIFY", "1", 1) == 0);
  CHECK(!SystemdNotifyEnabled());
  CHECK(!SystemdNotifyReady());

  // A configured but unbound path is a soft failure, never a crash.
  const std::string missing = dir + "/missing.sock";
  CHECK(::setenv("NOTIFY_SOCKET", missing.c_str(), 1) == 0);
  CHECK(SystemdNotifyEnabled());
  CHECK(!SystemdNotifyReady());

  // A path that cannot fit sun_path is rejected before sendto.
  const std::string too_long(200, 'x');
  CHECK(::setenv("NOTIFY_SOCKET", too_long.c_str(), 1) == 0);
  CHECK(SystemdNotifyEnabled());
  CHECK(!SystemdNotifySend("READY=1"));

  // Invalid state strings are rejected regardless of environment.
  CHECK(!SystemdNotifySend(nullptr));
  CHECK(!SystemdNotifySend(""));

  (void)::close(fd);
  (void)::unlink(path.c_str());
  (void)::rmdir(dir.c_str());
  (void)::unsetenv("NOTIFY_SOCKET");
  (void)::unsetenv("THYME_SD_NOTIFY");
}

#if defined(__linux__)
void TestAbstractSocket() {
  char name[64];
  std::snprintf(name, sizeof(name), "thyme-notify-%d", static_cast<int>(getpid()));
  const int fd = BindAbstractSocket(name);
  const std::string path = std::string("@") + name;
  CHECK(::setenv("NOTIFY_SOCKET", path.c_str(), 1) == 0);
  CHECK(::setenv("THYME_SD_NOTIFY", "1", 1) == 0);

  bool ok = false;
  CHECK(SystemdNotifyReady());
  CHECK(RecvDatagram(fd, &ok) == "READY=1");
  CHECK(ok);
  CHECK(SystemdNotifyWatchdog());
  CHECK(RecvDatagram(fd, &ok) == "WATCHDOG=1");
  CHECK(ok);

  (void)::close(fd);
  (void)::unsetenv("NOTIFY_SOCKET");
  (void)::unsetenv("THYME_SD_NOTIFY");
}
#endif

}  // namespace

int main() {
  TestFilesystemSocket();
#if defined(__linux__)
  TestAbstractSocket();
#endif
  return 0;
}
