// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "app/state.h"
#include "core/geom.h"

namespace noise::app {

// The pointer end of a coverage stroke: the same dabs the brush walks, with the dab's alpha taken as
// coverage rather than colour. Where it lands is `AppState::coverage_target` — the marked area, or the
// active layer's mask — and that is the whole difference between the two: one shape language, one mask,
// two lifetimes.
void cover_press(AppState& state, Vec2 document_point);
void cover_drag(AppState& state, Vec2 document_point);
void cover_release(AppState& state);

}  // namespace noise::app
