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
    state.doc.begin_marked();
    state.doc.repaint_marked(touched, [&state](Mask& mask, Rect region) {
        mask.paint(region, scratch_coverage(state.stroke.scratch, region, state.brush.opacity),
                   ink_of(state));
    });
}

}  // namespace

void cover_press(AppState& state, Vec2 document_point) {
    if (state.doc.empty() || state.stroke.active) {
        return;
    }
    state.stroke = PointerStroke{};
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
    state.doc.commit_marked();
    state.stroke = PointerStroke{};
}

}  // namespace noise::app
