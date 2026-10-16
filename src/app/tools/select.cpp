// SPDX-License-Identifier: Apache-2.0
#include "app/tools/select.h"

#include "app/state.h"

#include "app/coverage.h"
#include "core/brush.h"
#include "core/log.h"

#include <algorithm>
#include <cmath>

namespace noise::app {

Rect marquee(Vec2 from, Vec2 to, Extent canvas) {
    const int left = static_cast<int>(std::floor(std::min(from.x, to.x)));
    const int top = static_cast<int>(std::floor(std::min(from.y, to.y)));
    const int right = static_cast<int>(std::ceil(std::max(from.x, to.x)));
    const int bottom = static_cast<int>(std::ceil(std::max(from.y, to.y)));
    return clamp_rect(Rect{left, top, right - left, bottom - top}, canvas);
}

namespace {

// One shape, replaced rather than added to - the modes that combine shapes are the commands'. Which
// coverage it lands on is the target's: the same drag marks the area or punches a layer mask, and the
// gesture is not two tools.
void apply_marquee(AppState& state) {
    if (state.coverage_target == CoverageTarget::mask) {
        state.doc.paint_mask(Rect{0, 0, state.doc.size().w, state.doc.size().h},
                             [&](Mask& mask) {
                                 op_shape(mask, state.select.shape, state.select.rect,
                                          MaskOp::replace);
                             });
        return;
    }
    Mask selection(state.doc.size());
    op_shape(selection, state.select.shape, state.select.rect, MaskOp::replace);
    state.doc.set_selection(selection);
}

}  // namespace

void select_press(AppState& state, Vec2 document_point) {
    if (state.doc.empty()) {
        return;
    }
    state.select = PointerSelection{};
    state.select.active = true;
    state.select.origin = document_point;
    // The tool table says what shape this tool draws; a `select` row without one is a table bug, so
    // the gesture simply has nothing to draw and says so in the log rather than guessing a shape.
    const std::optional<MaskShape> shape = tool_marquee(state.tool);
    if (!shape) {
        log::warn(std::string("tool '") + state.tool +
                  "' selects but draws no shape, so the marquee was dropped");
        state.select.active = false;
        return;
    }
    state.select.shape = *shape;
    state.select.rect = marquee(document_point, document_point, state.doc.size());
    apply_marquee(state);
}

void select_drag(AppState& state, Vec2 document_point) {
    if (!state.select.active) {
        return;
    }
    const Rect next = marquee(state.select.origin, document_point, state.doc.size());
    if (next == state.select.rect) {
        return;
    }
    state.select.rect = next;
    apply_marquee(state);
}

void select_release(AppState& state) {
    if (!state.select.active) {
        return;
    }
    if (state.select.rect.empty()) {
        if (state.coverage_target == CoverageTarget::mask) {
            // An empty drag clears the mask it was aimed at rather than reaching past it for the
            // marked area: a gesture does not change what it was pointed at on the way out.
            state.doc.paint_mask(Rect{0, 0, state.doc.size().w, state.doc.size().h},
                                 [](Mask& mask) { mask.fill(0); });
        } else {
            state.doc.set_selection(std::nullopt);
        }
    } else {
        apply_marquee(state);
    }
    state.select = PointerSelection{};
}

}  // namespace noise::app
