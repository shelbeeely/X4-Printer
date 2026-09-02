#pragma once
// Display orientation for ui::TimelineView — a horizontal (left->right =
// time) or vertical (top->bottom = time) layout. This is new; neither
// PictoStick nor this project's earlier design had an orientation toggle.
//
// Deliberately just an enum + a pure toggle function: the actual
// M5GFX::setRotation() call this drives lives in ui/TimelineView.cpp,
// which needs the real display library and so isn't part of the
// host-testable core (test/orientation/OrientationTest.cpp covers this
// file; ui/ has no host tests, same split as the rest of this project).

namespace orientation {

enum class Mode { Horizontal, Vertical };

inline Mode toggle(Mode m) { return m == Mode::Horizontal ? Mode::Vertical : Mode::Horizontal; }

}  // namespace orientation
