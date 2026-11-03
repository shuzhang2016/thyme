#include "thyme/check.h"
#include "thyme/record/thyme_bin.h"
#include "thyme/record/thyme_bin_stream.h"
#include "test_wait.h"

#include <chrono>
#include <cstdio>
#include <thread>

#if !defined(_WIN32)
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

using namespace thyme;


namespace {
#if !defined(_WIN32)
// Reserve an ephemeral TCP port (probe-bind then release). A small race with
// other processes exists, which beats hardcoding a port that collides on
// shared test machines.
uint16_t PickEphemeralPort() {
  const int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return 0;
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = 0;
  if (bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
    close(fd);
    return 0;
  }
  sockaddr_in bound{};
  socklen_t len = sizeof(bound);
  uint16_t port = 0;
  if (getsockname(fd, reinterpret_cast<sockaddr*>(&bound), &len) == 0) {
    port = ntohs(bound.sin_port);
  }
  close(fd);
  return port;
}
#endif
}  // namespace

int main() {
#if defined(_WIN32)
  return 0;
#else
  const char* path = "/tmp/thyme_tcp_bin.bin";
  ThymeStructDesc desc{};
  desc.struct_id = 0;
  desc.contract_version = 1;
  desc.payload_size = 8;
  std::snprintf(desc.name, sizeof(desc.name), "Demo");

  const uint16_t port = PickEphemeralPort();
  CHECK(port != 0);

  std::thread server([&]() {
    CHECK(ThymeBinTcpCaptureToFile(port, path, 5));
  });

  // bind()/listen() happen inside the server thread, so a fixed sleep is a
  // guess. Retry the connect under a deadline: ECONNREFUSED means "not
  // listening yet", a successful fd is the readiness handshake.
  int fd = -1;
  CHECK(thyme_test::WaitFor([&] {
    fd = ThymeBinTcpConnect("127.0.0.1", port);
    return fd >= 0;
  }));
  CHECK(fd >= 0);
  {
    ThymeBinFdSink sink(fd, true);
    ThymeBinEncoder enc(&sink);
    CHECK(enc.WriteHeader(99, 1234, &desc, 1));
    uint8_t payload[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    CHECK(enc.WriteData(1, 0, 5678, 3, payload, 8));
  }

  server.join();

  ThymeBinReader reader(path);
  CHECK(reader.ok());
  CHECK(reader.header().session_id == 99);
  ThymeBinDataEntry e{};
  CHECK(reader.ReadNext(e) == thyme::ThymeBinReader::ReadResult::kOk);
  CHECK(e.record_sequence == 1);
  CHECK(e.payload_size == 8);

  std::remove(path);
  return 0;
#endif
}
