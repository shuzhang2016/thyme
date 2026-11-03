#pragma once

#include <cstdint>
#include <ostream>
#include <string>

namespace thyme {
namespace replaycli {

struct RepairSummary {
  bool ok = false;
  std::string error;
  std::string input;
  std::string output;
  bool input_corrupt = false;
  uint64_t recovered_records = 0;
  uint64_t corrupt_offset = 0;
  uint64_t discarded_tail_bytes = 0;
  uint64_t output_bytes = 0;
  bool partial = false;
};

int RunRepair(const std::string& input, const std::string& output, bool json,
              std::ostream& out, RepairSummary* summary);

}  // namespace replaycli
}  // namespace thyme
