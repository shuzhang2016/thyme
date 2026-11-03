#pragma once

#include <cstdio>
#include <memory>

namespace thyme {

// Deleter for stdio streams. This is the single place a raw FILE* is
// released, so the owning-memory check has exactly one site to inspect.
struct FileCloser {
  static int Close(std::FILE* f) noexcept {
    if (f == nullptr) return 0;
    // FILE* is a POSIX/stdio handle owned by this deleter; the check cannot
    // follow ownership through a custom std::unique_ptr deleter.
    // NOLINTNEXTLINE(cppcoreguidelines-owning-memory)
    return std::fclose(f);
  }

  void operator()(std::FILE* f) const noexcept { (void)Close(f); }
};

// Owning stdio handle: move-only, no heap allocation of its own (same size as
// a raw FILE*). Error paths reset it instead of calling fclose by hand.
using FilePtr = std::unique_ptr<std::FILE, FileCloser>;

}  // namespace thyme
