// SPDX-License-Identifier: Apache-2.0
#include "app/command_groups.h"

#include "app/command_args.h"

#include "core/mask.h"

#include <nlohmann/json.hpp>

#include <string>

namespace noise::app {

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
}

}  // namespace noise::app
