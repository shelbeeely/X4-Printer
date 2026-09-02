#pragma once
// CHECK(), not assert(): this project's CMakeLists.txt defaults to a
// Release build, which defines NDEBUG and compiles assert() to nothing —
// a test file full of assert() calls would build cleanly and "pass"
// while checking literally nothing. CHECK always evaluates and aborts
// with a message + nonzero exit on failure, which is what ctest actually
// needs to detect a broken test. (Same file as
// ../../firmware/test/testutil.h — kept as a separate copy rather than a
// shared include across the two independent CMake projects.)

#include <cstdio>
#include <cstdlib>

#define CHECK(cond)                                                                     \
  do {                                                                                  \
    if (!(cond)) {                                                                      \
      std::fprintf(stderr, "CHECK FAILED: %s\n  at %s:%d\n", #cond, __FILE__, __LINE__); \
      std::exit(1);                                                                     \
    }                                                                                    \
  } while (0)
