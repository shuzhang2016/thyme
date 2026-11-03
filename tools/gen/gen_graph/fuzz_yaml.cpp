#include "yaml.h"

#include <cstddef>
#include <cstdint>
#include <string>

// libFuzzer entry: LoadGraphYamlFromString must not crash on arbitrary input.
extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  if (size == 0 || size > 65536) return 0;
  std::string text(reinterpret_cast<const char*>(data), size);
  Graph g;
  (void)LoadGraphYamlFromString(text, &g);
  return 0;
}
