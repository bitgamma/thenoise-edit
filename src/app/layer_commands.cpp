// SPDX-License-Identifier: Apache-2.0
#include "app/command_groups.h"

#include "app/command_args.h"

#include "core/layer.h"
#include "core/log.h"
#include "io/flat.h"

#include <nlohmann/json.hpp>

#include <cmath>
#include <filesystem>
#include <string>
#include <vector>

namespace noise::app {
namespace {

// A dropped file has to be named somewhere, and the name a person gave it is the only one they
// recognise: `streetlight.png` becomes the layer `streetlight`.
std::string import_name(const std::string& path) {
    const std::string stem = std::filesystem::path(path).stem().string();
    return stem.empty() ? std::string("imported") : stem;
}

std::string range_error(const char* who, int index, std::size_t count) {
    return std::string(who) + ": 'index' " + std::to_string(index) +
           " is out of range, the document has " + std::to_string(count) + " layers";
}

}  // namespace

void register_layer_commands(AppState& state, CommandTable& table) {
    table.add("layer.list", "List the layers, bottom first", {}, {},
              [&state](const json&) -> CommandResult {
        CommandResult guard = require_document(state);
        if (!guard.ok) {
            return guard;
        }
        return command_ok(json{{"active", state.doc.active()},
                               {"layers", layers_json_with_checksums(state)},
                               {"composite_checksum", state.doc.composite().checksum_hex()}});
    });

    table.add("layer.add", "Add a layer above the active one", {}, {"name", "fill"},
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
                               {"layers", layers_json(state)},
                               {"revision", state.doc.revision()}});
    });

    table.add("layer.remove", "Delete a layer, the active one by default", {}, {"index"},
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
                               {"layers", layers_json(state)},
                               {"revision", state.doc.revision()}});
    });

    // One command for the layer's non-pixel state, taking only the fields it is given, because
    // that is how `tool.set` works and a panel row should not have to know a verb per field.
    table.add("layer.set", "Change a layer's visibility, opacity or blend mode",
              {}, {"index", "visible", "opacity", "mode"},
              [&state](const json& args) -> CommandResult {
        CommandResult guard = require_document(state);
        if (!guard.ok) {
            return guard;
        }
        int index = state.doc.active();
        if (args.contains("index") && !want_int(args, "index", index)) {
            return fail("layer.set", "'index' must be an integer");
        }
        const std::size_t count = state.doc.layers().size();
        if (index < 0 || static_cast<std::size_t>(index) >= count) {
            return fail("layer.set", range_error("layer.set", index, count));
        }
        bool changed = false;
        if (args.contains("visible")) {
            if (!args.at("visible").is_boolean()) {
                return fail("layer.set", "'visible' must be a boolean");
            }
            state.doc.set_layer_visible(index, args.at("visible").get<bool>());
            changed = true;
        }
        if (args.contains("opacity")) {
            double opacity = 0.0;
            if (!want_number(args, "opacity", opacity)) {
                return fail("layer.set", "'opacity' must be a number between 0.0 and 1.0");
            }
            if (!state.doc.set_layer_opacity(index, opacity)) {
                return fail("layer.set", "'opacity' must be between 0.0 and 1.0 (got " +
                                    std::to_string(opacity) + ")");
            }
            changed = true;
        }
        if (args.contains("mode")) {
            if (!args.at("mode").is_string()) {
                return fail("layer.set", "'mode' must be a mode name, one of: " +
                                    blend_mode_names());
            }
            const std::string wanted = args.at("mode").get<std::string>();
            const std::optional<BlendMode> mode = blend_mode_from_name(wanted);
            if (!mode) {
                return fail("layer.set", "no blend mode named '" + wanted + "', this build has: " +
                                    blend_mode_names());
            }
            state.doc.set_layer_mode(index, *mode);
            changed = true;
        }
        if (!changed) {
            return fail("layer.set",
                        "nothing to change: 'visible', 'opacity' and/or 'mode'");
        }
        return command_ok(json{{"active", state.doc.active()},
                               {"layers", layers_json(state)},
                               {"revision", state.doc.revision()}});
    });

    // The command a desktop drop runs, and the one `file.pick` will one day feed: the picture lands
    // as it is on a layer of its own, its centre where the pointer was, so the gesture is "put this
    // here" rather than "put this at 0,0 and go find it". `at` is in document pixels and is explicit
    // because a drop off the edge of the canvas is a thing that happens and must be answered.
    table.add("layer.import", "Land an image file on a new layer, centred on 'at'", {},
              {"path", "at"},
              [&state](const json& args) -> CommandResult {
        CommandResult guard = require_document(state);
        if (!guard.ok) {
            return guard;
        }
        std::string path;
        if (!arg_text(args, "path", path) || path.empty()) {
            return fail("layer.import", "expected a non-empty string 'path'");
        }
        // Absent is not the origin here: a picture silently landing in a corner because a caller
        // forgot the point would look like a painting bug.
        Vec2 at{};
        if (!args.contains("at") || !arg_vec2(args, "at", at)) {
            return fail("layer.import", "expected 'at' as [x, y] in document pixels");
        }
        std::string error;
        const Image imported = io::load_flat_image(path, error);
        if (imported.empty()) {
            return fail("layer.import", path + ": " + error);
        }
        const Rect placed{static_cast<int>(std::lround(at.x)) - imported.size().w / 2,
                          static_cast<int>(std::lround(at.y)) - imported.size().h / 2,
                          imported.size().w, imported.size().h};
        const Rect inside = intersect(placed, Rect{0, 0, state.doc.width(), state.doc.height()});
        if (inside.empty()) {
            return fail("layer.import",
                        path + " landed whole outside the " + std::to_string(state.doc.width()) +
                            "x" + std::to_string(state.doc.height()) + " canvas at " +
                            std::to_string(placed.x) + "," + std::to_string(placed.y));
        }
        const std::string name = import_name(path);
        const int index = state.doc.add_layer_written(name, inside, [&](Image& layer) {
            layer.blit(imported, placed.x, placed.y);
            return true;
        });
        if (index < 0) {
            return fail("layer.import", path + ": the document refused the new layer");
        }
        log::info("imported " + name + " (" + std::to_string(imported.size().w) + "x" +
                  std::to_string(imported.size().h) + ") at " + std::to_string(placed.x) + "," +
                  std::to_string(placed.y));
        return command_ok(json{{"active", index},
                               {"name", name},
                               {"size", json::array({imported.size().w, imported.size().h})},
                               {"placed", json::array({placed.x, placed.y})},
                               {"visible", json::array({inside.x, inside.y, inside.w, inside.h})}});
    });

    table.add("layer.select", "Choose the layer strokes paint on", {}, {"index"},
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
        return command_ok(json{{"active", state.doc.active()}, {"layers", layers_json(state)}});
    });

    // The stack's order, moved one step at a time by the arrows on a card and to any index by a driver.
    // No pixel is written: what the canvas shows changes because the layers stack differently.
    table.add("layer.move", "Move a layer to another position in the stack", {},
              {"index", "to"}, [&state](const json& args) -> CommandResult {
        CommandResult guard = require_document(state);
        if (!guard.ok) {
            return guard;
        }
        int index = state.doc.active();
        if (args.contains("index") && !want_int(args, "index", index)) {
            return fail("layer.move", "'index' must be an integer");
        }
        int to = 0;
        if (!want_int(args, "to", to)) {
            return fail("layer.move", "expected integer 'to'");
        }
        const std::size_t count = state.doc.layers().size();
        if (index < 0 || static_cast<std::size_t>(index) >= count) {
            return fail("layer.move", range_error("layer.move", index, count));
        }
        if (to < 0 || static_cast<std::size_t>(to) >= count) {
            return fail("layer.move", range_error("layer.move", to, count));
        }
        if (!state.doc.move_layer(index, to)) {
            return fail("layer.move", "the document refused to move '" +
                                    state.doc.layer(static_cast<std::size_t>(index)).name + "'");
        }
        return command_ok(layers_json(state));
    });

    // The whole stack into the one layer the canvas shows, as one step: the person who flattened by
    // mistake gets every layer back by name, mask and opacity intact.
    table.add("layer.flatten", "Flatten the whole stack into one layer", {}, {},
              [&state](const json&) -> CommandResult {
        CommandResult guard = require_document(state);
        if (!guard.ok) {
            return guard;
        }
        if (!state.doc.flatten_layers()) {
            return fail("layer.flatten",
                        "'" + state.doc.layer(0).name + "' is the only layer: there is nothing to flatten");
        }
        return command_ok(layers_json(state));
    });

    // `F2` names the active layer, the way it names a file in every other tool. The field itself belongs
    // to the panel and is reported by `app.state.layer_cards[].renaming`; this only opens it, because the
    // name is not a fact about the document until `Enter` puts it there.
    table.add("layer.rename.begin", "Put a layer's name field on its card", {"F2"}, {"index"},
              [&state](const json& args) -> CommandResult {
        CommandResult guard = require_document(state);
        if (!guard.ok) {
            return guard;
        }
        int index = state.doc.active();
        if (args.contains("index") && !want_int(args, "index", index)) {
            return fail("layer.rename.begin", "'index' must be an integer");
        }
        const std::size_t count = state.doc.layers().size();
        if (index < 0 || static_cast<std::size_t>(index) >= count) {
            return fail("layer.rename.begin", range_error("layer.rename.begin", index, count));
        }
        begin_layer_rename(state, index);
        return command_ok(json{{"renaming", index}});
    });

    // The layer's name, which is how a person refers to it afterwards. Not part of `layer.set`: that one
    // is about what the composite is made of, and a name changes nothing the canvas shows.
    table.add("layer.rename", "Give a layer the name the panel shows", {}, {"index", "name"},
              [&state](const json& args) -> CommandResult {
        CommandResult guard = require_document(state);
        if (!guard.ok) {
            return guard;
        }
        int index = state.doc.active();
        if (args.contains("index") && !want_int(args, "index", index)) {
            return fail("layer.rename", "'index' must be an integer");
        }
        const std::size_t count = state.doc.layers().size();
        if (index < 0 || static_cast<std::size_t>(index) >= count) {
            return fail("layer.rename", range_error("layer.rename", index, count));
        }
        if (!args.contains("name") || !args.at("name").is_string()) {
            return fail("layer.rename", "'name' must be a string");
        }
        const std::string wanted = args.at("name").get<std::string>();
        if (!state.doc.rename_layer(index, wanted)) {
            return fail("layer.rename", "'" + wanted + "' is not a name a layer can carry: " +
                                            layer_name_rule());
        }
        return command_ok(layers_json(state));
    });

    table.add("layer.merge", "Merge a layer down onto the one under it", {}, {"index"},
              [&state](const json& args) -> CommandResult {
        CommandResult guard = require_document(state);
        if (!guard.ok) {
            return guard;
        }
        int index = state.doc.active();
        if (args.contains("index") && !want_int(args, "index", index)) {
            return fail("layer.merge", "'index' must be an integer");
        }
        const std::size_t count = state.doc.layers().size();
        if (index < 0 || static_cast<std::size_t>(index) >= count) {
            return fail("layer.merge", range_error("layer.merge", index, count));
        }
        if (index == 0) {
            return fail("layer.merge",
                        "'" + state.doc.layer(0).name +
                            "' is the bottom layer: there is nothing under it to merge onto");
        }
        if (!state.doc.merge_down(index)) {
            return fail("layer.merge", "the document refused to merge the layer down");
        }
        return command_ok(layers_json(state));
    });
}

}  // namespace noise::app
