// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "app/state.h"

#include "core/geom.h"

namespace noise::app {

// The rectangle-selection gesture. Press sets one corner, drag moves the other, and the document's
// selection follows live so a report taken mid-drag tells the truth about what is selected.
void select_press(AppState& state, Vec2 document_point);
void select_drag(AppState& state, Vec2 document_point);
// An empty marquee puts the selection down rather than selecting nothing: a click is how you get rid
// of a selection when you are not reaching for the keyboard.
void select_release(AppState& state);

// The rectangle two document points span, rounded outwards so every pixel the marquee passed over is
// inside it, then clamped to the canvas. Exported because the gesture's meaning is this function and
// a test should check it rather than the event plumbing.
Rect marquee(Vec2 from, Vec2 to, Extent canvas);

}  // namespace noise::app
