// SPDX-License-Identifier: Apache-2.0
#include "app/command_groups.h"

#include "app/command_args.h"

#include "core/brush.h"
#include "core/log.h"

#include <nlohmann/json.hpp>

namespace noise::app {

namespace {

// A stroke is painted exactly as it is described: every brush field must be present, so one
// recorded from the wire replays without consulting what the UI happens to have selected.
CommandResult read_stroke(const json& args,
                          const char* who,
                          std::vector<Vec2>& path,
                          Brush& brush,
                          Rgba8& color,
                          std::string& tool) {
    if (!arg_points(args, "points", path) || path.empty()) {
        return fail(who, "'points' must be a non-empty array of [x,y] in document pixels");
    }
    tool = kDefaultTool;
    if (args.contains("tool")) {
        if (!args.at("tool").is_string()) {
            return fail(who, "'tool' must be a string");
        }
        tool = args.at("tool").get<std::string>();
        if (tool != "brush" && tool != "eraser") {
            return fail(who, "unknown tool '" + tool + "' (brush, eraser)");
        }
    }
    struct Field {
        const char* key;
        double low;
        double high;
        double Brush::*member;
    };
    const Field fields[] = {{"size", kMinBrushSize, kMaxBrushSize, &Brush::size},
                            {"hardness", 0.0, 1.0, &Brush::hardness},
                            {"opacity", 0.0, 1.0, &Brush::opacity},
                            {"flow", 0.0, 1.0, &Brush::flow},
                            {"spacing", 0.0, 1.0, &Brush::spacing}};
    brush = {0.0, 0.0, 0.0, 0.0, 0.0};
    for (const Field& field : fields) {
        if (!args.contains(field.key)) {
            return fail(who, std::string("missing '") + field.key + "'");
        }
        const json& value = args.at(field.key);
        if (!value.is_number()) {
            return fail(who, std::string("'") + field.key + "' must be a number");
        }
        double& target = brush.*(field.member);
        target = value.get<double>();
        if (target < field.low || target > field.high) {
            return fail(who, std::string("'") + field.key + "' must be between " +
                            std::to_string(field.low) + " and " + std::to_string(field.high));
        }
    }
    if (!args.contains("color")) {
        return fail(who, "missing 'color'");
    }
    std::uint8_t channels[4] = {0, 0, 0, 255};
    if (!arg_rgba(args, "color", channels)) {
        return fail(who, "'color' must be [r,g,b,a] with each channel 0..255");
    }
    color = Rgba8{channels[0], channels[1], channels[2], channels[3]};
    return command_ok(json::object());
}

}  // namespace

void register_paint_commands(AppState& state, CommandTable& table) {
    table.add("tool.stroke",
              "Paint a stroke along document-space points with an explicit brush", {},
              [&state](const json& args) -> CommandResult {
        CommandResult guard = require_document(state);
        if (!guard.ok) {
            return guard;
        }
        std::vector<Vec2> path;
        Brush brush;
        Rgba8 color;
        std::string tool;
        CommandResult parsed = read_stroke(args, "tool.stroke", path, brush, color, tool);
        if (!parsed.ok) {
            return parsed;
        }
        const PaintMode mode = tool == "eraser" ? PaintMode::Erase : PaintMode::Over;
        const std::size_t dabs = stroke_dabs(path, brush).size();
        const Rect bounds = stroke_bounds(path, brush, state.doc.size());
        state.brush = brush;
        state.tool = tool;
        if (bounds.empty()) {
            return command_ok(json{{"tool", tool},
                                   {"dabs", dabs},
                                   {"touched", json::array()},
                                   {"revision", state.doc.revision()}});
        }
        std::optional<Rect> touched;
        state.doc.paint(bounds, [&](Image& image) { touched = paint_stroke(image, path, brush, color, mode); });
        if (!touched) {
            return fail("tool.stroke", "the stroke painted nothing inside the document");
        }
        // No checksum here: hashing a 2048² document costs more than painting a stroke
        // across it, and a caller that wants one asks `doc.get`.
        return command_ok(json{{"tool", tool},
                               {"dabs", dabs},
                               {"touched",
                                json::array({touched->x, touched->y, touched->w, touched->h})},
                               {"revision", state.doc.revision()}});
    });

    table.add("edit.undo", "Undo the last change to the pixels", {"Ctrl+Z"},
              [&state](const json&) -> CommandResult {
        CommandResult guard = require_document(state);
        if (!guard.ok) {
            return guard;
        }
        if (!state.doc.can_undo()) {
            return fail("edit.undo", "nothing to undo");
        }
        if (!state.doc.undo()) {
            return fail("edit.undo", "the recorded change no longer fits the document");
        }
        return command_ok(history_json(state.doc));
    });

    table.add("edit.redo", "Redo the change that undo took back", {"Ctrl+Y"},
              [&state](const json&) -> CommandResult {
        CommandResult guard = require_document(state);
        if (!guard.ok) {
            return guard;
        }
        if (!state.doc.can_redo()) {
            return fail("edit.redo", "nothing to redo");
        }
        if (!state.doc.redo()) {
            return fail("edit.redo", "the recorded change no longer fits the document");
        }
        return command_ok(history_json(state.doc));
    });
}

}  // namespace noise::app
