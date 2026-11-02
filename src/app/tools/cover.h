// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "app/state.h"
#include "core/geom.h"

namespace noise::app {

// Cover and Un-cover modify only the document selection, regardless of the selected layer plane.
void cover_press(AppState& state, Vec2 document_point);
void cover_drag(AppState& state, Vec2 document_point);
void cover_release(AppState& state);

}  // namespace noise::app
