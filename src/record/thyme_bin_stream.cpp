#include "thyme/record/thyme_bin_stream.h"

#include "thyme/record/file_ptr.h"

#include <cstdlib>
#include <cstdio>
#include <cstring>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace thyme {
bool ThymeBinParseHostPort(const char* spec, char* host_out, std::size_t host_cap,
                           uint16_t* port_out) {
  if (spec == nullptr || host_out == nullptr || port_out == nullptr ||
      host_cap == 0) {
    return false;
  }
  const char* colon = std::strrchr(spec, ':');
  if (colon == nullptr || colon == spec) return false;
  const std::size_t host_len = static_cast<std::size_t>(colon - spec);
  if (host_len + 1 >= host_cap) return false;
  std::memcpy(host_out, spec, host_len);
  host_out[host_len] = '\0';
  char* end = nullptr;
  const long p = strtol(colon + 1, &end, 10);
  if (end == colon + 1 || p <= 0 || p > 65535) return false;
  *port_out = static_cast<uint16_t>(p);
  return true;
}

int ThymeBinTcpConnect(const char* host, uint16_t port, int timeout_ms) {
  if (host == nullptr) return -1;
  const int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return -1;

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  if (inet_pton(AF_INET, host, &addr.sin_addr) != 1) {
#if defined(_WIN32)
    closesocket(fd);
#else
    close(fd);
#endif
    return -1;
  }

#if defined(_WIN32)
  (void)timeout_ms;
  if (connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
    closesocket(fd);
    return -1;
  }
  return fd;
#else
  const int flags = fcntl(fd, F_GETFL, 0);
  if (flags >= 0) fcntl(fd, F_SETFL, flags | O_NONBLOCK);
  if (connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
    if (errno != EINPROGRESS) {
      close(fd);
      return -1;
    }
    pollfd pfd{};
    pfd.fd = fd;
    pfd.events = POLLOUT;
    const int pr = poll(&pfd, 1, timeout_ms);
    if (pr <= 0) {
      close(fd);
      return -1;
    }
    int err = 0;
    socklen_t err_len = sizeof(err);
    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &err_len) != 0 || err != 0) {
      close(fd);
      return -1;
    }
  }
  if (flags >= 0) fcntl(fd, F_SETFL, flags);  // restore original mode
  return fd;
#endif
}

int ThymeBinTcpAcceptOne(uint16_t listen_port, int timeout_sec) {
  const int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
  if (listen_fd < 0) return -1;

  int yes = 1;
  setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<char*>(&yes),
             sizeof(yes));

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  addr.sin_port = htons(listen_port);
  if (bind(listen_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
#if defined(_WIN32)
    closesocket(listen_fd);
#else
    close(listen_fd);
#endif
    return -1;
  }
  if (listen(listen_fd, 1) != 0) {
#if defined(_WIN32)
    closesocket(listen_fd);
#else
    close(listen_fd);
#endif
    return -1;
  }

#if !defined(_WIN32)
  if (timeout_sec > 0) {
    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(listen_fd, &rfds);
    timeval tv{};
    tv.tv_sec = timeout_sec;
    tv.tv_usec = 0;
    const int sel = select(listen_fd + 1, &rfds, nullptr, nullptr, &tv);
    if (sel <= 0) {
      close(listen_fd);
      return -1;
    }
  }
#endif

  const int client_fd = accept(listen_fd, nullptr, nullptr);
#if defined(_WIN32)
  closesocket(listen_fd);
#else
  close(listen_fd);
#endif
  return client_fd;
}

bool ThymeBinTcpCaptureToFile(uint16_t listen_port, const char* out_path,
                              int timeout_sec) {
  if (out_path == nullptr) return false;
  const int fd = ThymeBinTcpAcceptOne(listen_port, timeout_sec);
  if (fd < 0) return false;

  FilePtr fp(std::fopen(out_path, "wb"));
  if (!fp) {
#if defined(_WIN32)
    closesocket(fd);
#else
    close(fd);
#endif
    return false;
  }

  uint8_t buf[65536];
  bool ok = true;
  for (;;) {
    const ssize_t n =
#if defined(_WIN32)
        recv(fd, reinterpret_cast<char*>(buf), sizeof(buf), 0);
#else
        recv(fd, buf, sizeof(buf), 0);
#endif
    if (n == 0) break;
    if (n < 0) {
      ok = false;
      break;
    }
    if (std::fwrite(buf, 1, static_cast<std::size_t>(n), fp.get()) !=
        static_cast<std::size_t>(n)) {
      ok = false;
      break;
    }
  }

#if defined(_WIN32)
  closesocket(fd);
#else
  close(fd);
#endif
  return ok;
}

}  // namespace thyme
