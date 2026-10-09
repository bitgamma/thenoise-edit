// SPDX-License-Identifier: Apache-2.0
#include "app/tools/brush.h"

#include "core/brush.h"
#include "core/log.h"

#include <cmath>

namespace noise::app {

namespace {

PaintMode mode_of(const AppState& state) {
    const std::optional<PaintMode> mode = tool_mode(state.tool);
    if (!mode) {
        // Only a tool table edited at runtime could get here; painting with the wrong mode
        // would silently change pixels, so the stroke is dropped instead.
        log::error("tool '" + state.tool + "' is not in the tool table; the stroke is dropped");
        return PaintMode::Over;
    }
    return *mode;
}

void add_dabs(AppState& state, const std::vector<Vec2>& path) {
    const std::vector<Vec2> dabs = walk_dabs(path, state.brush, state.stroke.walk);
    if (dabs.empty()) {
        return;
    }
    scratch_stamp(state.stroke.scratch, dabs, state.brush, state.doc.size());
    state.doc.begin_provisional(state.stroke.scratch.rect);
    state.stroke.dabs += dabs.size();
    state.doc.repaint_provisional([&state](Image& image) {
        scratch_composite(image, state.stroke.scratch, state.color, state.brush, mode_of(state));
    });
}

}  // namespace

void brush_press(AppState& state, Vec2 document_point) {
    if (state.doc.empty() || state.stroke.active) {
        return;
    }
    state.stroke = PointerStroke{};
    state.stroke.active = true;
    state.stroke.last = document_point;
    // The held-aside pixels are taken by `add_dabs`, over the rectangle the dabs actually
    // reach: a snapshot of the whole canvas would cost the same for a 4 px dot as for a
    // screen-wide drag, and would be what undo has to store.
    add_dabs(state, {document_point});
}

void brush_drag(AppState& state, Vec2 document_point) {
    if (!state.stroke.active) {
        return;
    }
    add_dabs(state, {state.stroke.last, document_point});
    state.stroke.last = document_point;
}

void brush_release(AppState& state) {
    if (!state.stroke.active) {
        return;
    }
    state.doc.commit_provisional();
    state.stroke = PointerStroke{};
}

}  // namespace noise::app
