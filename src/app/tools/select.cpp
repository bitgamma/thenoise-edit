// SPDX-License-Identifier: Apache-2.0
#include "app/tools/select.h"

#include "app/state.h"

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

// The marquee as a selection of the document's own size. Replaced rather than added to: the gesture
// draws one rectangle, and the modes that combine shapes are the command's (`select.rect`).
void apply_marquee(AppState& state) {
    Mask selection(state.doc.size());
    selection.op(state.select.rect, MaskOp::replace);
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
        state.doc.set_selection(std::nullopt);
    } else {
        apply_marquee(state);
    }
    state.select = PointerSelection{};
}

}  // namespace noise::app
