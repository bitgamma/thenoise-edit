// SPDX-License-Identifier: Apache-2.0
#include "app/tools/select.h"

#include "app/state.h"

#include "core/brush.h"
#include "core/log.h"
#include "core/maskops.h"

#include <algorithm>
#include <cmath>

namespace noise::app {
namespace {

constexpr double kLassoPointSpacingPx = 0.5;

std::optional<MaskShape> mask_shape(SelectionGesture gesture) {
    switch (gesture) {
        case SelectionGesture::rectangle:
            return MaskShape::rect;
        case SelectionGesture::ellipse:
            return MaskShape::ellipse;
        case SelectionGesture::lasso:
            return std::nullopt;
    }
    return std::nullopt;
}

Mask current_or_empty(const AppState& state) {
    return state.doc.selection() ? *state.doc.selection() : Mask(state.doc.size());
}

void apply_marquee_replace(AppState& state) {
    const std::optional<MaskShape> shape = mask_shape(state.select.gesture);
    if (!shape) {
        return;
    }
    Mask shaped(state.doc.size());
    op_shape(shaped, *shape, state.select.rect, MaskOp::replace);
    state.doc.set_selection(std::move(shaped));
}

void apply_marquee_operation(AppState& state) {
    const std::optional<MaskShape> shape = mask_shape(state.select.gesture);
    if (!shape) {
        return;
    }
    Mask selected = state.select.mode == MaskOp::replace ? Mask(state.doc.size())
                                                         : current_or_empty(state);
    op_shape(selected, *shape, state.select.rect, state.select.mode);
    state.doc.set_selection(std::move(selected));
}

void append_lasso_point(PointerSelection& selection, Vec2 point) {
    if (!std::isfinite(point.x) || !std::isfinite(point.y)) {
        return;
    }
    if (!selection.path.empty()) {
        const Vec2 previous = selection.path.back();
        if (std::hypot(point.x - previous.x, point.y - previous.y) < kLassoPointSpacingPx) {
            return;
        }
    }
    selection.path.push_back(point);
}

void finish_lasso(AppState& state) {
    const std::optional<MaskPatch> patch = rasterize_lasso(state.doc.size(), state.select.path);
    if (!patch) {
        select_cancel(state);
        return;
    }
    Mask selected = state.select.mode == MaskOp::replace ? Mask(state.doc.size())
                                                         : current_or_empty(state);
    selected.op(patch->area, patch->coverage, state.select.mode);
    state.doc.set_selection(std::move(selected));
    state.doc.commit_marked();
    state.select = PointerSelection{};
}

void finish_marquee(AppState& state) {
    if (state.select.rect.empty()) {
        if (state.select.mode == MaskOp::replace) {
            state.doc.set_selection(std::nullopt);
        }
    } else if (state.select.mode == MaskOp::replace) {
        apply_marquee_replace(state);
    } else {
        apply_marquee_operation(state);
    }
    state.doc.commit_marked();
    state.select = PointerSelection{};
}

}  // namespace

Rect marquee(Vec2 from, Vec2 to, Extent bounds) {
    const int left = static_cast<int>(std::floor(std::min(from.x, to.x)));
    const int top = static_cast<int>(std::floor(std::min(from.y, to.y)));
    const int right = static_cast<int>(std::ceil(std::max(from.x, to.x)));
    const int bottom = static_cast<int>(std::ceil(std::max(from.y, to.y)));
    return clamp_rect(Rect{left, top, right - left, bottom - top}, bounds);
}

bool select_press(AppState& state, Vec2 document_point, MaskOp mode) {
    if (state.doc.empty() || !std::isfinite(document_point.x) ||
        !std::isfinite(document_point.y)) {
        return false;
    }
    const std::optional<SelectionGesture> gesture = tool_selection_gesture(state.tool);
    if (!gesture) {
        log::warn(std::string("tool '") + state.tool +
                  "' selects but has no selection gesture, so the gesture was dropped");
        return false;
    }
    state.select = PointerSelection{};
    state.select.origin = document_point;
    state.select.gesture = *gesture;
    state.select.mode = mode;
    const bool begun = state.doc.begin_marked();
    if (!begun) {
        state.select = PointerSelection{};
        return false;
    }
    state.select.active = true;
    if (*gesture == SelectionGesture::lasso) {
        append_lasso_point(state.select, document_point);
        return true;
    }
    state.select.rect = marquee(document_point, document_point, state.doc.size());
    if (mode == MaskOp::replace) {
        apply_marquee_replace(state);
    }
    return true;
}

void select_drag(AppState& state, Vec2 document_point) {
    if (!state.select.active) {
        return;
    }
    if (state.select.gesture == SelectionGesture::lasso) {
        append_lasso_point(state.select, document_point);
        return;
    }
    const Rect next = marquee(state.select.origin, document_point, state.doc.size());
    if (next == state.select.rect) {
        return;
    }
    state.select.rect = next;
    if (state.select.mode == MaskOp::replace) {
        apply_marquee_replace(state);
    }
}

void select_release(AppState& state, std::optional<Vec2> document_point) {
    if (!state.select.active) {
        return;
    }
    if (state.select.gesture == SelectionGesture::lasso) {
        if (document_point) {
            append_lasso_point(state.select, *document_point);
        }
        finish_lasso(state);
        return;
    }
    finish_marquee(state);
}

void select_cancel(AppState& state) {
    if (!state.select.active) {
        return;
    }
    state.doc.cancel_marked();
    state.select = PointerSelection{};
}

}  // namespace noise::app
