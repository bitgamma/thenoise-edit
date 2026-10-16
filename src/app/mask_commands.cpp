// SPDX-License-Identifier: Apache-2.0
#include "app/command_groups.h"

#include "app/command_args.h"

#include "core/layer.h"
#include "core/log.h"
#include "core/mask.h"

#include <nlohmann/json.hpp>

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

}  // namespace

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
