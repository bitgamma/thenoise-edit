// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "app/state.h"

#include "core/geom.h"

namespace noise::app {

bool select_press(AppState& state, Vec2 document_point, MaskOp mode);
void select_drag(AppState& state, Vec2 document_point);
void select_release(AppState& state, std::optional<Vec2> document_point);
void select_cancel(AppState& state);

// The rectangle two document points span, rounded outwards so every pixel the marquee passed over is
// inside it, then clamped to the canvas. Exported because the gesture's meaning is this function and
// a test should check it rather than the event plumbing.
Rect marquee(Vec2 from, Vec2 to, Extent canvas);

}  // namespace noise::app
