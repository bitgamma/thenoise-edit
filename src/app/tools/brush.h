// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "app/state.h"
#include "core/geom.h"

namespace noise::app {

// The pointer end of a brush stroke. Press stamps, a drag adds the dabs the walk owes since
// the last point, release commits the gesture as one undo step.
//
// Painting goes through the document's provisional edit rather than a separate preview: the
// pixels a stroke will replace are held aside once, every repaint restarts from them, so the
// stroke shows live at its true opacity and a gesture fed in point by point ends on the same
// bytes as the same polyline handed to `paint_stroke` at once.
void brush_press(AppState& state, Vec2 document_point);
void brush_drag(AppState& state, Vec2 document_point);
void brush_release(AppState& state);

}  // namespace noise::app
