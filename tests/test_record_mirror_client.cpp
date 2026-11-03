#include "thyme/check.h"
#include "thyme/record/record_mirror_client.h"
#include "thyme/record/record_table.h"

#include <cstdio>
#include <cstring>
#include <string>

#if !defined(_WIN32)
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

using namespace thyme;


namespace {

uint32_t TestPayloadSize(uint16_t) { return 8; }
ThymeRecordPriority TestPriority(uint16_t) { return ThymeRecordPriority::kP1; }
void TestFillDescs(ThymeStructDesc* out, std::size_t cap) {
  if (cap == 0) return;
  out[0].struct_id = 0;
  out[0].contract_version = 1;
  out[0].payload_size = 8;
  std::snprintf(out[0].name, sizeof(out[0].name), "Demo");
}

}  // namespace

// Create(DefaultShmName(), ...) must work: the default name helper returns a
// fresh string, and the client stores its own copy.
int main() {
#if defined(_WIN32)
  return 0;
#else
  const ThymeRecordTable table{1, TestPayloadSize, TestPriority, TestFillDescs};

  const std::string expected = record_mirror::DefaultShmName();
  ShmRegion::UnlinkName(expected.c_str());

  record_mirror::Client client;
  CHECK(client.Create(record_mirror::DefaultShmName().c_str(), table, 7));
  CHECK(client.Page() != nullptr);
  CHECK(client.Page()->Validate());

  // OPS-02: a second same-name Create must not unlink the live page.
  record_mirror::Client duplicate;
  CHECK(!duplicate.Create(expected.c_str(), table, 8));
  CHECK(client.Page() != nullptr);
  CHECK(client.Page()->Validate());

  // The shm object must exist under exactly the default name.
  const int fd = shm_open(expected.c_str(), O_RDONLY, 0600);
  CHECK(fd >= 0);
  close(fd);

  client.Shutdown(true);
  CHECK(shm_open(expected.c_str(), O_RDONLY, 0600) < 0);  // unlinked
  return 0;
#endif
}
