// SPDX-License-Identifier: Apache-2.0
#include "app/command_groups.h"

#include "app/command_args.h"

#include "core/layer.h"
#include "core/log.h"
#include "core/mask.h"
#include "core/maskops.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <string>
#include <string_view>

namespace noise::app {

namespace {

// What a fill leaves behind. A row per end of the coverage scale, so the panel and the wire ask for
// the same two things and neither restates what the byte is.
struct MaskFill {
    const char* name;
    std::uint8_t coverage;
};
constexpr MaskFill kMaskFills[]{
    {"show", kMaskIn},
    {"hide", kMaskOut},
};

std::string mask_fill_names() {
    std::string names;
    for (const MaskFill& fill : kMaskFills) {
        if (!names.empty()) {
            names += ", ";
        }
        names += fill.name;
    }
    return names;
}

// The canvas the whole-plane commands write, in the document's own coordinates.
Rect canvas(const AppState& state) {
    return Rect{0, 0, state.doc.width(), state.doc.height()};
}

// The active layer, or a refusal naming the layer that had no mask to act on. The write itself goes
// through `Document::paint_mask`, because that is the door which records the step, repairs the
// composite and asks the selection how far the write may reach.
const Layer* masked_layer(AppState& state, const char* who, CommandResult& refusal) {
    const Layer& layer = state.doc.layer(static_cast<std::size_t>(state.doc.active()));
    if (!layer.mask) {
        refusal = fail(who, "layer '" + layer.name + "' has no mask (mask.add puts one down)");
        return nullptr;
    }
    return &layer;
}

struct EdgeOptions {
    int shift = 0;
    int feather = 0;
    bool hard = false;
};

bool read_edge_options(const json& args, EdgeOptions& options) {
    return arg_int(args, "shift", options.shift) &&
           arg_int(args, "feather", options.feather) &&
           arg_bool(args, "hard", options.hard) &&
           options.shift >= -kMaxEdgeShiftPx && options.shift <= kMaxEdgeShiftPx &&
           options.feather >= 0 && options.feather <= kMaxEdgeFeatherPx;
}

CommandResult invalid_edge_options(const char* name) {
    return fail(name, "shift must be -" + std::to_string(kMaxEdgeShiftPx) + ".." +
                          std::to_string(kMaxEdgeShiftPx) + " px, feather 0.." +
                          std::to_string(kMaxEdgeFeatherPx) + " px, and hard a boolean");
}

// The scale follows the selected value, but only after a drag has finished. Moving
// the range while ImGui owns the pointer would jump the slider under the cursor.
void scroll_range(int value, int low_limit, int high_limit, int& low, int& high) {
    if (value >= high && high < high_limit) {
        const int by = std::min({std::max(1, (high - low) / 2), high_limit - high,
                                 value - low});
        low += by;
        high += by;
    } else if (value <= low && low > low_limit) {
        const int by = std::min({std::max(1, (high - low) / 2), low - low_limit,
                                 high - value});
        low -= by;
        high -= by;
    }
}

int shift_limit(Extent size) {
    return std::max(size.w, size.h);
}

int feather_limit(Extent size, int default_feather) {
    return std::max(default_feather, std::min(size.w, size.h) / 2);
}

void start_edge_values(EdgeAdjustment& edge, const Mask& mask, std::uint64_t revision,
                       int default_feather) {
    edge = EdgeAdjustment{};
    edge.baseline = mask;
    edge.known_revision = revision;
    edge.default_feather = std::max(0, default_feather);
    edge.feather = edge.default_feather;
    const int max_shift = shift_limit(mask.size());
    edge.shift_min = std::max(-max_shift, -50);
    edge.shift_max = std::min(max_shift, 50);
    const int max_feather = feather_limit(mask.size(), edge.default_feather);
    const int half = edge.default_feather > 0 ? std::min(50, edge.default_feather) : 50;
    edge.feather_min = std::max(0, edge.default_feather - half);
    edge.feather_max = std::min(max_feather, edge.default_feather + half);
}

void scroll_edge_values(EdgeAdjustment& edge) {
    scroll_range(edge.shift, -shift_limit(edge.baseline.size()),
                 shift_limit(edge.baseline.size()), edge.shift_min, edge.shift_max);
    scroll_range(edge.feather, 0, feather_limit(edge.baseline.size(), edge.default_feather),
                 edge.feather_min, edge.feather_max);
}

void sync_edge_epoch(AppState& state) {
    CanvasForm& form = state.canvas_form;
    if (form.edge_epoch == state.document_epoch) {
        return;
    }
    if (form.edge_preview_layer != 0 && state.doc.in_provisional()) {
        state.doc.commit_provisional();
    }
    form.edge_preview_layer = 0;
    form.edge_adjustments.clear();
    form.edge_epoch = state.document_epoch;
}

EdgeAdjustment& current_edge(AppState& state, const Layer& layer) {
    sync_edge_epoch(state);
    CanvasForm& form = state.canvas_form;
    if (form.edge_preview_layer != 0 &&
        (form.edge_preview_layer != layer.id || !state.doc.in_provisional())) {
        if (state.doc.in_provisional()) {
            state.doc.commit_provisional();
        }
        form.edge_preview_layer = 0;
    }
    EdgeAdjustment& edge = form.edge_adjustments[layer.id];
    if (!edge.baseline.sized() || edge.baseline.size() != layer.mask->size() ||
        edge.known_revision != layer.content_revision) {
        // A brush, Undo/Redo, mask replacement or geometry edit made the old reference stale.
        // Rebase on the actual mask; never overwrite a user's independent mask correction.
        const int start_feather = edge.baseline.sized() ? edge.feather : 0;
        start_edge_values(edge, *layer.mask, layer.content_revision, start_feather);
    }
    return edge;
}

json edge_values(const EdgeAdjustment& edge) {
    return json{{"shift", edge.shift}, {"feather", edge.feather},
                {"default_feather", edge.default_feather},
                {"shift_min", edge.shift_min}, {"shift_max", edge.shift_max},
                {"feather_min", edge.feather_min}, {"feather_max", edge.feather_max},
                {"shift_limit", shift_limit(edge.baseline.size())},
                {"feather_limit", feather_limit(edge.baseline.size(), edge.default_feather)}};
}

}  // namespace

void remember_editable_edge(AppState& state, const Layer& layer, int feather_px) {
    if (!layer.mask) {
        return;
    }
    sync_edge_epoch(state);
    start_edge_values(state.canvas_form.edge_adjustments[layer.id], *layer.mask,
                      layer.content_revision, feather_px);
}

void register_mask_commands(AppState& state, CommandTable& table) {
    // The two directions between a selection and a layer mask. Both are copies of the same
    // `Mask`, which is the reason they exist at all: a selection-shaped type and a mask-shaped type
    // would need a conversion with rules, and rules are where "the mask moved by a pixel" comes from.
    table.add("mask.from_selection", "Hide the active layer where the selection does not reach",
              {}, {}, [&state](const json&) -> CommandResult {
        CommandResult guard = require_document(state);
        if (!guard.ok) {
            return guard;
        }
        if (!state.doc.selection()) {
            return fail("mask.from_selection",
                        "the document has no selection - select.rect or select.all first, because "
                        "no selection means every pixel is writable and is not the same claim as a "
                        "mask that covers nothing");
        }
        if (!state.doc.set_layer_mask(state.doc.active(), state.doc.selection())) {
            return fail("mask.from_selection", "the selection does not fit the layer");
        }
        state.edit_plane = LayerPlane::mask;
        return command_ok(layer_mask_json(state.doc.layer(static_cast<std::size_t>(
            state.doc.active()))));
    });

    table.add("select.from_mask", "Select what the active layer's mask shows", {}, {},
              [&state](const json&) -> CommandResult {
        CommandResult guard = require_document(state);
        if (!guard.ok) {
            return guard;
        }
        const Layer& layer = state.doc.layer(static_cast<std::size_t>(state.doc.active()));
        if (!layer.mask) {
            return fail("select.from_mask",
                        "layer '" + layer.name + "' has no mask; mask.from_selection puts one down "
                                                "from the current selection");
        }
        state.doc.set_selection(layer.mask);
        return command_ok(selection_json(state));
    });

    // The mask itself, as an object the panel can put on and take off. Adding one replaces one the
    // layer already has, which is what the panel's single button does and why it says `Add` only
    // when there is nothing there.
    table.add("mask.add", "Put a fully shown mask on the active layer, replacing one it has", {}, {},
              [&state](const json&) -> CommandResult {
        CommandResult guard = require_document(state);
        if (!guard.ok) {
            return guard;
        }
        Mask mask(state.doc.size());
        mask.fill(kMaskIn);
        if (!state.doc.set_layer_mask(state.doc.active(), mask)) {
            return fail("mask.add", "the mask did not fit the document");
        }
        state.edit_plane = LayerPlane::mask;
        return command_ok(layer_mask_json(state.doc.layer(static_cast<std::size_t>(state.doc.active()))));
    });

    table.add("mask.remove", "Take the active layer's mask off", {}, {},
              [&state](const json&) -> CommandResult {
        CommandResult guard = require_document(state);
        if (!guard.ok) {
            return guard;
        }
        if (!state.doc.set_layer_mask(state.doc.active(), std::nullopt)) {
            return fail("mask.remove", "the layer could not be changed");
        }
        state.edit_plane = LayerPlane::pixels;
        return command_ok(layer_mask_json(state.doc.layer(static_cast<std::size_t>(state.doc.active()))));
    });

    table.add("mask.fill", "Fill the active layer's mask with full or no coverage", {}, {"to"},
              [&state](const json& args) -> CommandResult {
        CommandResult guard = require_document(state);
        if (!guard.ok) {
            return guard;
        }
        if (!args.contains("to") || !args.at("to").is_string()) {
            return fail("mask.fill", "'to' must be one of " + mask_fill_names());
        }
        const std::string_view to = args.at("to").get<std::string_view>();
        for (const MaskFill& fill : kMaskFills) {
            if (to != fill.name) {
                continue;
            }
            CommandResult refusal;
            if (masked_layer(state, "mask.fill", refusal) == nullptr) {
                return refusal;
            }
            const std::uint8_t coverage = fill.coverage;
            state.doc.paint_mask(canvas(state),
                                 [coverage](Mask& mask) { mask.fill(coverage); });
            return command_ok(layer_mask_json(state.doc.layer(static_cast<std::size_t>(state.doc.active()))));
        }
        return fail("mask.fill", "'to' must be one of " + mask_fill_names());
    });

    table.add("mask.edge", "Adjust an editable result edge: harden, grow/shrink, feather", {},
              {"shift", "hard", "feather"}, [&state](const json& args) -> CommandResult {
        CommandResult guard = require_document(state);
        if (!guard.ok) {
            return guard;
        }
        CommandResult refusal;
        const Layer* layer = masked_layer(state, "mask.edge", refusal);
        if (layer == nullptr) {
            return refusal;
        }
        if (state.canvas_form.edge_preview_layer != 0) {
            return fail("mask.edge", "finish the live edge adjustment before a new mask edit");
        }
        EdgeOptions options;
        if (!read_edge_options(args, options)) {
            return invalid_edge_options("mask.edge");
        }
        const Mask next = adjusted_edge(*layer->mask, options.shift, options.hard,
                                        options.feather);
        state.doc.set_layer_mask(state.doc.active(), next);
        return command_ok(layer_mask_json(state.doc.layer(static_cast<std::size_t>(
            state.doc.active()))));
    });

    table.add("mask.edge.state", "Read the persistent absolute edge sliders and their visible ranges",
              {}, {}, [&state](const json&) -> CommandResult {
        CommandResult guard = require_document(state);
        if (!guard.ok) {
            return guard;
        }
        CommandResult refusal;
        const Layer* layer = masked_layer(state, "mask.edge.state", refusal);
        if (layer == nullptr) {
            return refusal;
        }
        return command_ok(edge_values(current_edge(state, *layer)));
    });

    table.add("mask.edge.preview", "Set absolute edge size and feather with an editable live preview",
              {}, {"shift", "feather"}, [&state](const json& args) -> CommandResult {
        CommandResult guard = require_document(state);
        if (!guard.ok) {
            return guard;
        }
        CommandResult refusal;
        const Layer* layer = masked_layer(state, "mask.edge.preview", refusal);
        if (layer == nullptr) {
            return refusal;
        }
        EdgeAdjustment& edge = current_edge(state, *layer);
        int shift = edge.shift;
        int feather = edge.feather;
        if (!arg_int(args, "shift", shift) || !arg_int(args, "feather", feather) ||
            shift < -shift_limit(layer->mask->size()) ||
            shift > shift_limit(layer->mask->size()) || feather < 0 ||
            feather > feather_limit(layer->mask->size(), edge.default_feather)) {
            return fail("mask.edge.preview", "shift or feather exceeds the canvas size");
        }
        CanvasForm& form = state.canvas_form;
        if (shift == edge.shift && feather == edge.feather) {
            return command_ok(json{{"shift", shift}, {"feather", feather}});
        }
        // Only pixels in either the previous or new footprint can have changed.
        // Snapshotting and recompositing the entire canvas per one-pixel click was
        // far more expensive than the edge operation itself on an outpainted crop.
        const Rect before = adjusted_edge_bounds(edge.baseline, edge.shift, edge.feather);
        const Rect after = adjusted_edge_bounds(edge.baseline, shift, feather);
        // An entirely empty mask has no footprint, but its sliders still need a
        // regular, inexpensive gesture/finish cycle.
        const Rect combined = unite(before, after);
        const Rect whole = canvas(state);
        const bool uniform = edge.baseline.covers_all(whole) ||
                             edge.baseline.covers_nothing(whole);
        const Rect dirty = combined.empty() || uniform ? Rect{0, 0, 1, 1} : combined;
        if ((form.edge_preview_layer == 0 && state.doc.in_provisional()) ||
            !state.doc.begin_provisional(dirty, LayerPlane::mask)) {
            return fail("mask.edge.preview", "another paint gesture is active");
        }
        if (form.edge_preview_layer == 0) {
            form.edge_preview_layer = layer->id;
            edge.drag_start_shift = edge.shift;
            edge.drag_start_feather = edge.feather;
        }
        // The reference mask never changes during an edge gesture, even though the
        // displayed mask is updated on every pixel step.
        const Mask next = (shift == 0 && feather == edge.default_feather)
            ? edge.baseline.region(dirty)
            : adjusted_edge_region(edge.baseline, shift, /*hard=*/true, feather, dirty);
        state.doc.repaint_provisional_mask(dirty, PaintFence::own_coverage,
            [&next, dirty](Mask& mask, Rect) { mask.op(dirty, next.plane(), MaskOp::replace); });
        edge.shift = shift;
        edge.feather = feather;
        edge.known_revision = state.doc.layer(static_cast<std::size_t>(state.doc.active())).content_revision;
        return command_ok(json{{"shift", shift}, {"feather", feather},
                               {"revision", state.doc.revision()}});
    });

    table.add("mask.edge.finish", "Keep an absolute edge change as one optional Undo step", {}, {},
              [&state](const json&) -> CommandResult {
        CanvasForm& form = state.canvas_form;
        if (form.edge_preview_layer == 0) {
            return fail("mask.edge.finish", "no edge adjustment is being previewed");
        }
        if (!state.doc.in_provisional()) {
            form.edge_preview_layer = 0;
            return fail("mask.edge.finish", "the preview was interrupted");
        }
        // A layer switch during a drag must not silently discard the adjustment.
        const std::uint64_t layer_id = form.edge_preview_layer;
        state.doc.commit_provisional();
        form.edge_preview_layer = 0;
        auto found = form.edge_adjustments.find(layer_id);
        if (found != form.edge_adjustments.end()) {
            for (const Layer& layer : state.doc.layers()) {
                if (layer.id == layer_id) {
                    found->second.known_revision = layer.content_revision;
                    scroll_edge_values(found->second);
                    return command_ok(layer_mask_json(layer));
                }
            }
        }
        return command_ok(json::object());
    });

    table.add("mask.edge.cancel", "Discard an uncommitted edge gesture", {}, {},
              [&state](const json&) -> CommandResult {
        CanvasForm& form = state.canvas_form;
        if (form.edge_preview_layer == 0) {
            return fail("mask.edge.cancel", "no edge adjustment is being previewed");
        }
        const std::uint64_t layer_id = form.edge_preview_layer;
        if (state.doc.in_provisional()) {
            state.doc.discard_provisional();
        }
        form.edge_preview_layer = 0;
        if (auto found = form.edge_adjustments.find(layer_id);
            found != form.edge_adjustments.end()) {
            found->second.shift = found->second.drag_start_shift;
            found->second.feather = found->second.drag_start_feather;
            if (!state.doc.empty() && state.active_layer().id == layer_id) {
                found->second.known_revision = state.active_layer().content_revision;
            }
        }
        return command_ok(json::object());
    });

    table.add("mask.invert", "Turn what the active layer's mask shows into what it hides", {}, {},
              [&state](const json&) -> CommandResult {
        CommandResult guard = require_document(state);
        if (!guard.ok) {
            return guard;
        }
        CommandResult refusal;
        if (masked_layer(state, "mask.invert", refusal) == nullptr) {
            return refusal;
        }
        state.doc.paint_mask(canvas(state), [](Mask& mask) { mask.invert(); });
        return command_ok(layer_mask_json(state.doc.layer(static_cast<std::size_t>(state.doc.active()))));
    });

    table.add("mask.get", "Report the active layer's mask", {}, {},
              [&state](const json&) -> CommandResult {
        CommandResult guard = require_document(state);
        if (!guard.ok) {
            return guard;
        }
        return command_ok(json{{"mask", layer_mask_json(state.doc.layer(
                                            static_cast<std::size_t>(state.doc.active())))}});
    });
}

}  // namespace noise::app
