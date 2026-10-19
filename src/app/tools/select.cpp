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

// The canvas the marquee writes, in the document's own coordinates.
Rect canvas(const AppState& state) {
    return Rect{0, 0, state.doc.size().w, state.doc.size().h};
}

// One shape, replaced rather than added to - the modes that combine shapes are the commands'. Which
// coverage it lands on is the target's: the same drag marks the area or punches a layer mask, and the
// gesture is not two tools. Both lifetimes get the same treatment for the same reason: the shape is
// drawn onto the coverage the gesture found, over the whole canvas, so a drag that shrank leaves no
// trail of the box it used to be, and every event costs the same whether the pointer has moved far or
// little.
void apply_marquee(AppState& state) {
    Mask shaped(state.doc.size());
    op_shape(shaped, state.select.shape, state.select.rect, MaskOp::replace);
    if (state.select.coverage == CoverageTarget::mask) {
        state.doc.repaint_provisional_mask(canvas(state),
                                           [&shaped](Mask& mask, Rect) { mask = shaped; });
        return;
    }
    state.doc.set_selection(std::move(shaped));
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
    // The lifetime is held here too: the press opens one door, and a switch aimed elsewhere mid-drag
    // must not leave the other one open.
    state.select.coverage = state.coverage_target;
    state.select.rect = marquee(document_point, document_point, state.doc.size());
    // One gesture, one undo step, in whichever lifetime the switch aims it: the layer's mask holds its
    // bytes aside, the marked area holds the document's own coverage.
    if (state.select.coverage == CoverageTarget::mask) {
        state.doc.begin_provisional(canvas(state), LayerPlane::mask);
    } else {
        state.doc.begin_marked();
    }
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
        if (state.select.coverage == CoverageTarget::mask) {
            // An empty drag clears the mask it was aimed at rather than reaching past it for the
            // marked area: a gesture does not change what it was pointed at on the way out.
            state.doc.repaint_provisional_mask(canvas(state),
                                               [](Mask& mask, Rect) { mask.fill(0); });
        } else {
            state.doc.set_selection(std::nullopt);
        }
    } else {
        apply_marquee(state);
    }
    if (state.select.coverage == CoverageTarget::mask) {
        state.doc.commit_provisional();
    } else {
        state.doc.commit_marked();
    }
    state.select = PointerSelection{};
}

}  // namespace noise::app
