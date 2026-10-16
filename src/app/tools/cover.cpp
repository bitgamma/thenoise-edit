// SPDX-License-Identifier: Apache-2.0
#include "app/tools/cover.h"

#include "app/state.h"
#include "core/brush.h"
#include "core/log.h"
#include "core/mask.h"

#include <vector>

namespace noise::app {
namespace {

MaskInk ink_of(const AppState& state) {
    const std::optional<PaintMode> mode = tool_mode(state.tool);
    if (!mode) {
        // Only a tool table edited at runtime gets here; writing coverage with the wrong ink would
        // silently mark or unmark pixels, so the stroke is dropped instead.
        log::error("tool '" + state.tool + "' is not in the tool table; the stroke is dropped");
        return MaskInk::reveal;
    }
    return mask_ink(*mode);
}

void add_dabs(AppState& state, const std::vector<Vec2>& path) {
    const std::vector<Vec2> dabs = walk_dabs(path, state.brush, state.stroke.walk);
    if (dabs.empty()) {
        return;
    }
    const Rect touched = scratch_stamp(state.stroke.scratch, dabs, state.brush, state.doc.size());
    state.stroke.dabs += dabs.size();
    if (state.stroke.coverage != CoverageTarget::mask) {
        // The marked area is not a layer's bytes, so there is no provisional door and no undo step:
        // the selection is replaced whole per event exactly as the marquee replaces it, which is what
        // makes a freehand circle and a rectangle the same kind of thing to press a verb against.
        Mask marked = state.doc.selection().has_value() ? *state.doc.selection()
                                                        : Mask(state.doc.size());
        marked.paint(touched, scratch_coverage(state.stroke.scratch, touched, state.brush.opacity),
                     ink_of(state));
        state.doc.set_selection(marked);
        return;
    }
    state.doc.begin_provisional(touched, LayerPlane::mask);
    state.doc.repaint_provisional_mask(touched, [&state](Mask& mask, Rect region) {
        mask.paint(region, scratch_coverage(state.stroke.scratch, region, state.brush.opacity),
                   ink_of(state));
    });
}

}  // namespace

void cover_press(AppState& state, Vec2 document_point) {
    if (state.doc.empty() || state.stroke.active || !state.coverage_writable()) {
        return;
    }
    state.stroke = PointerStroke{};
    state.stroke.coverage = state.coverage_target;
    state.stroke.plane = state.coverage_target == CoverageTarget::mask ? LayerPlane::mask
                                                                      : LayerPlane::pixels;
    state.stroke.active = true;
    state.stroke.last = document_point;
    add_dabs(state, {document_point});
}

void cover_drag(AppState& state, Vec2 document_point) {
    if (!state.stroke.active) {
        return;
    }
    add_dabs(state, {state.stroke.last, document_point});
    state.stroke.last = document_point;
}

void cover_release(AppState& state) {
    if (!state.stroke.active) {
        return;
    }
    if (state.stroke.coverage == CoverageTarget::mask) {
        state.doc.commit_provisional();
    }
    state.stroke = PointerStroke{};
}

}  // namespace noise::app
