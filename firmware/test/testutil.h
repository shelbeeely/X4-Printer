#pragma once
// CHECK(), not assert(): CMakeLists.txt here defaults to a Release build
// (matching the firmware's own optimized builds), which defines NDEBUG and
// compiles assert() to nothing — a test file full of assert() calls would
// build cleanly and "pass" while checking literally nothing. CHECK always
// evaluates and aborts with a message + nonzero exit on failure, which is
// what ctest actually needs to detect a broken test.

#include <cstdio>
#include <cstdlib>

#define CHECK(cond)                                                                        \
  do {                                                                                      \
    if (!(cond)) {                                                                          \
      std::fprintf(stderr, "CHECK FAILED: %s\n  at %s:%d\n", #cond, __FILE__, __LINE__);    \
      std::exit(1);                                                                         \
    }                                                                                        \
  } while (0)
