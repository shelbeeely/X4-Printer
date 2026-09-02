#include "testutil.h"
#include <cstdio>

#include "orientation/Orientation.h"

using orientation::Mode;
using orientation::toggle;

int main() {
  CHECK(toggle(Mode::Horizontal) == Mode::Vertical);
  CHECK(toggle(Mode::Vertical) == Mode::Horizontal);
  CHECK(toggle(toggle(Mode::Horizontal)) == Mode::Horizontal);  // round-trips

  std::puts("OrientationTest OK");
  return 0;
}
