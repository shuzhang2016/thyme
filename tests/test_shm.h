#pragma once

#include <cstdlib>
#include <string>
#include <unistd.h>

namespace thyme::test {

inline std::string ShmTestPrefix() {
  const char* prefix = std::getenv("THYME_SHM_TEST_PREFIX");
  return prefix != nullptr && prefix[0] != '\0' ? prefix : "thyme_test_";
}

inline std::string ShmTestName(const char* tag) {
  return "/" + ShmTestPrefix() + tag + "_" +
         std::to_string(static_cast<long>(getpid()));
}

inline std::string ShmTestScanPrefix() {
  return ShmTestPrefix();
}

}  // namespace thyme::test
