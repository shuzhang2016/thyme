#include "thyme/record/mcap_writer.h"
#include "thyme/record/thyme_bin.h"

int main() {
  thyme::ThymeBinFileHeader header{};
  thyme::McapWriterOptions options{};
  // Touch symbols from two exported libraries; linking all of them is what
  // proves the CMake/pkg-config package is complete.
  return header.file_abi_version >= 1 && options.include_summary ? 0 : 1;
}
