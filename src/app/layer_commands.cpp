// SPDX-License-Identifier: Apache-2.0
#include "app/command_groups.h"

#include "app/command_args.h"

#include "core/layer.h"
#include "core/log.h"

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

namespace noise::app {
namespace {

std::string range_error(const char* who, int index, std::size_t count) {
    return std::string(who) + ": 'index' " + std::to_string(index) +
           " is out of range, the document has " + std::to_string(count) + " layers";
}

}  // namespace

void register_layer_commands(AppState& state, CommandTable& table) {
    table.add("layer.list", "List the layers, bottom first", {},
              [&state](const json&) -> CommandResult {
        CommandResult guard = require_document(state);
        if (!guard.ok) {
            return guard;
        }
        return command_ok(json{{"active", state.doc.active()},
                               {"layers", layers_json(state, true)},
                               {"composite_checksum", state.doc.composite().checksum_hex()}});
    });

    table.add("layer.add", "Add a layer above the active one", {},
              [&state](const json& args) -> CommandResult {
        CommandResult guard = require_document(state);
        if (!guard.ok) {
            return guard;
        }
        std::string name;
        if (args.contains("name") && !arg_name(args, "name", name)) {
            return fail("layer.add", "'name' must be a string");
        }
        Rgba8 fill = kNewLayerFill;
        if (args.contains("fill")) {
            std::uint8_t channels[4] = {0, 0, 0, 0};
            if (!arg_rgba(args, "fill", channels)) {
                return fail("layer.add", "'fill' must be [r,g,b,a] with each channel 0..255");
            }
            fill = Rgba8{channels[0], channels[1], channels[2], channels[3]};
        }
        const int index = state.doc.add_layer(std::move(name), fill);
        if (index < 0) {
            return fail("layer.add", "the document has no canvas to size a layer from");
        }
        log::info("added " + state.doc.layer(static_cast<std::size_t>(index)).name);
        return command_ok(json{{"active", state.doc.active()},
                               {"layers", layers_json(state, true)},
                               {"revision", state.doc.revision()}});
    });

    table.add("layer.remove", "Delete a layer, the active one by default", {},
              [&state](const json& args) -> CommandResult {
        CommandResult guard = require_document(state);
        if (!guard.ok) {
            return guard;
        }
        int index = state.doc.active();
        if (args.contains("index") && !want_int(args, "index", index)) {
            return fail("layer.remove", "'index' must be an integer");
        }
        const std::size_t count = state.doc.layers().size();
        if (index < 0 || static_cast<std::size_t>(index) >= count) {
            return fail("layer.remove", range_error("layer.remove", index, count));
        }
        const std::string removed = state.doc.layer(static_cast<std::size_t>(index)).name;
        // The document owns the rule that one layer must survive; the range check above is
        // separate so each refusal can name its own reason.
        if (!state.doc.remove_layer(index)) {
            return fail("layer.remove",
                        "the last layer cannot be removed: a document needs a canvas");
        }
        log::info("removed " + removed);
        return command_ok(json{{"active", state.doc.active()},
                               {"layers", layers_json(state, true)},
                               {"revision", state.doc.revision()}});
    });

    table.add("layer.select", "Choose the layer strokes paint on", {},
              [&state](const json& args) -> CommandResult {
        CommandResult guard = require_document(state);
        if (!guard.ok) {
            return guard;
        }
        int index = 0;
        if (!want_int(args, "index", index)) {
            return fail("layer.select", "expected integer 'index'");
        }
        if (!state.doc.select_layer(index)) {
            return fail("layer.select",
                        range_error("layer.select", index, state.doc.layers().size()));
        }
        return command_ok(json{{"active", state.doc.active()}, {"layers", layers_json(state, true)}});
    });
}

}  // namespace noise::app
