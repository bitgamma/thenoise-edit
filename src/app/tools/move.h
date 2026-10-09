// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "app/state.h"
#include "core/geom.h"

namespace noise::app {

// The pointer end of the move tool: the layer slides under the cursor. Press takes the snapshot the
// gesture draws from and paints nothing, so a click that never moved commits no undo step; a drag
// repaints the union of where the content was and where it is going; release commits the gesture as
// one step.
//
// Offsets are whole document pixels. Resampling is a different feature with a different undo, and a
// move that quietly blurred the layer would be the worse tool for not saying so.
void move_press(AppState& state, Vec2 document_point);
void move_drag(AppState& state, Vec2 document_point);
void move_release(AppState& state);

}  // namespace noise::app
