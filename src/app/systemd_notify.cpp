#include "thyme/app/systemd_notify.h"

#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace thyme {
namespace app {
namespace {

bool Truthy(const char* value) {
  if (value == nullptr) return false;
  return std::strcmp(value, "1") == 0 || std::strcmp(value, "yes") == 0 ||
         std::strcmp(value, "true") == 0 || std::strcmp(value, "on") == 0;
}

}  // namespace

bool SystemdNotifyEnabled() {
  if (!Truthy(std::getenv("THYME_SD_NOTIFY"))) return false;
  const char* path = std::getenv("NOTIFY_SOCKET");
  return path != nullptr && path[0] != '\0';
}

bool SystemdNotifySend(const char* state) {
  if (state == nullptr || state[0] == '\0') return false;
  if (!SystemdNotifyEnabled()) return false;

  const char* path = std::getenv("NOTIFY_SOCKET");
  const std::size_t path_len = std::strlen(path);
  struct sockaddr_un addr {};
  addr.sun_family = AF_UNIX;
  socklen_t addr_len = 0;
  if (path[0] == '@') {
    // Linux abstract namespace: systemd spells the leading NUL as '@'. The
    // address length stops at the end of the name, not at sun_path's size.
    if (path_len > sizeof(addr.sun_path)) return false;
    addr.sun_path[0] = '\0';
    std::memcpy(addr.sun_path + 1, path + 1, path_len - 1);
    addr_len = static_cast<socklen_t>(offsetof(struct sockaddr_un, sun_path) +
                                      1 + (path_len - 1));
  } else {
    if (path_len >= sizeof(addr.sun_path)) return false;
    std::memcpy(addr.sun_path, path, path_len + 1);
    addr_len = static_cast<socklen_t>(offsetof(struct sockaddr_un, sun_path) +
                                      path_len + 1);
  }

  const int fd = ::socket(AF_UNIX, SOCK_DGRAM, 0);
  if (fd < 0) return false;
  int flags = MSG_DONTWAIT;
#ifdef MSG_NOSIGNAL
  flags |= MSG_NOSIGNAL;
#endif
  const std::size_t state_len = std::strlen(state);
  const ssize_t sent =
      ::sendto(fd, state, state_len, flags,
               reinterpret_cast<const struct sockaddr*>(&addr), addr_len);
  (void)::close(fd);
  return sent == static_cast<ssize_t>(state_len);
}

bool SystemdNotifyReady() { return SystemdNotifySend("READY=1"); }

bool SystemdNotifyWatchdog() { return SystemdNotifySend("WATCHDOG=1"); }

}  // namespace app
}  // namespace thyme
