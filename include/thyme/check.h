#pragma once
#include <cstdio>
#include <cstdlib>

#define CHECK(cond)                                                          \
  do {                                                                       \
    if (!(cond)) {                                                           \
      std::fprintf(stderr, "CHECK failed: %s (%s:%d)\n", #cond, __FILE__,    \
                   __LINE__);                                                \
      std::fflush(stderr);                                                 \
      std::exit(EXIT_FAILURE);                                             \
    }                                                                        \
  } while (0)
