// SPDX-License-Identifier: Apache-2.0
#include "app/command_groups.h"

#include "app/brush_steps.h"
#include "app/command_args.h"

#include "core/brush.h"
#include "app/coverage.h"
#include "app/tools/brush.h"
#include "app/tools/cover.h"
#include "app/tools/move.h"
#include "app/tools/select.h"
#include "core/log.h"

#include <nlohmann/json.hpp>

#include <cmath>
#include <optional>

namespace noise::app {

namespace {

// A brush field, its legal range and where it lives. `tool.stroke` demands every one of
// them and `tool.set` takes only the ones it is given, but both read this table, so a range
// cannot be stated twice.
struct BrushField {
    const char* key;
    double low;
    double high;
    double Brush::*member;
};
constexpr BrushField kBrushFields[]{
    {"size", kMinBrushSize, kMaxBrushSize, &Brush::size},
    {"hardness", 0.0, 1.0, &Brush::hardness},
    {"opacity", 0.0, 1.0, &Brush::opacity},
    {"flow", 0.0, 1.0, &Brush::flow},
    {"spacing", 0.0, 1.0, &Brush::spacing},
};

// The arguments a call may override the session's tool settings with: the tool, its colour, and
// every brush field - named from `kBrushFields`, so a field added there is accepted by both
// commands without being restated in a second place.
std::vector<std::string> brush_arg_names() {
    std::vector<std::string> names{"tool", "color"};
    for (const BrushField& field : kBrushFields) {
        names.push_back(field.key);
    }
    return names;
}

std::vector<std::string> stroke_arg_names() {
    std::vector<std::string> names = brush_arg_names();
    names.push_back("points");
    return names;
}

// The row of the brush table with this name, or null. A step command names the field it moves
// instead of repeating its range, so there is exactly one place a range exists.
const BrushField* brush_field(std::string_view name) {
    for (const BrushField& field : kBrushFields) {
        if (name == field.key) {
            return &field;
        }
    }
    return nullptr;
}

// One field of `brush`, from `args`, range-checked. The message names the field.
bool read_brush_field(const json& args,
                      const BrushField& field,
                      Brush& brush,
                      const char* who,
                      std::string& why) {
    const json& value = args.at(field.key);
    if (!value.is_number()) {
        why = std::string("'") + field.key + "' must be a number, in " + who;
        return false;
    }
    const double got = value.get<double>();
    if (got < field.low || got > field.high) {
        why = std::string("'") + field.key + "' must be between " + std::to_string(field.low) +
              " and " + std::to_string(field.high) + ", in " + who;
        return false;
    }
    brush.*(field.member) = got;
    return true;
}

// A stroke paints with the tool settings the app is on, and any field the call carries
// overrides one of them: the pointer has no fields to send, and a recorded stroke that does
// send them replays independently of session state.
CommandResult read_stroke(const json& args,
                          const char* who,
                          const AppState& current,
                          std::vector<Vec2>& path,
                          Brush& brush,
                          Rgba8& color,
                          std::string& tool) {
    if (!arg_points(args, "points", path) || path.empty()) {
        return fail(who, "'points' must be a non-empty array of [x,y] in document pixels");
    }
    tool = current.tool;
    if (args.contains("tool")) {
        if (!args.at("tool").is_string()) {
            return fail(who, "'tool' must be a string");
        }
        tool = args.at("tool").get<std::string>();
        if (!tool_action(tool)) {
            return fail(who, "unknown tool '" + tool + "' (" + tool_names() + ")");
        }
    }
    // Checked whatever way the tool was chosen: the session can be left on `move` or `select`, and a
    // stroke that then painted over the canvas would be the command doing something no tool asked it
    // to do.
    if (!tool_mode(tool)) {
        return fail(who, "tool '" + tool + "' paints nothing (" + paintable_tool_names() + ")");
    }
    brush = current.brush;
    for (const BrushField& field : kBrushFields) {
        if (!args.contains(field.key)) {
            continue;
        }
        std::string why;
        if (!read_brush_field(args, field, brush, who, why)) {
            return fail(who, why);
        }
    }
    color = current.color;
    if (args.contains("color")) {
        std::uint8_t channels[4] = {0, 0, 0, 255};
        if (!arg_rgba(args, "color", channels)) {
            return fail(who, "'color' must be [r,g,b,a] with each channel 0..255");
        }
        color = Rgba8{channels[0], channels[1], channels[2], channels[3]};
    }
    return command_ok(json::object());
}

}  // namespace

void register_paint_commands(AppState& state, CommandTable& table) {
    table.add("tool.stroke",
              "Paint a stroke along document-space points, overriding the current tool as given",
              {}, stroke_arg_names(),
              [&state](const json& args) -> CommandResult {
        CommandResult guard = require_document(state);
        if (!guard.ok) {
            return guard;
        }
        std::vector<Vec2> path;
        Brush brush;
        Rgba8 color;
        std::string tool;
        CommandResult parsed = read_stroke(args, "tool.stroke", state, path, brush, color, tool);
        if (!parsed.ok) {
            return parsed;
        }
        const PaintMode mode = tool_mode(tool).value_or(PaintMode::Over);
        const std::size_t dabs = stroke_dabs(path, brush).size();
        const Rect bounds = stroke_bounds(path, brush, state.doc.size());
        state.brush = brush;
        state.color = color;
        state.tool = tool;
        if (bounds.empty()) {
            return command_ok(json{{"tool", tool},
                                   {"dabs", dabs},
                                   {"touched", json::array()},
                                   {"revision", state.doc.revision()}});
        }
        const bool coverage_stroke = tool_action(tool) == ToolAction::coverage;
        if (coverage_stroke && !state.coverage_writable()) {
            return fail("tool.stroke", "the coverage target is '" +
                                           std::string(coverage_target_name(state.coverage_target)) +
                                           "' and layer '" + state.active_layer().name +
                                           "' has none (mask.add)");
        }
        std::optional<Rect> touched;
        if (coverage_stroke && state.coverage_target == CoverageTarget::mask) {
            state.doc.paint_mask(bounds, [&](Mask& mask) {
                touched = paint_stroke_mask(mask, path, brush, mode);
            });
        } else if (coverage_stroke) {
            // The marked area takes the same stroke through the same mask, and the whole polyline is
            // painted into one copy of it, so the command is one coat at the brush's own opacity and
            // `set_selection` records the coverage it replaces as one step - the same thing the pointer
            // gesture commits at release, which is what makes a freehand mark and a marquee alike.
            Mask marked = state.doc.selection().has_value() ? *state.doc.selection()
                                                            : Mask(state.doc.size());
            touched = paint_stroke_mask(marked, path, brush, mode);
            state.doc.set_selection(marked);
        } else {
            state.doc.paint(bounds, PaintFence::selection, [&](Image& image) {
                touched = paint_stroke(image, path, brush, color, mode);
            });
        }
        if (!touched) {
            return fail("tool.stroke", "the stroke painted nothing inside the document");
        }
        // No checksum here: hashing a 2048² document costs more than painting a stroke
        // across it, and a caller that wants one asks `doc.get`.
        return command_ok(json{{"tool", tool},
                               {"dabs", dabs},
                               {"coverage", coverage_target_name(state.coverage_target)},
                               {"touched",
                                json::array({touched->x, touched->y, touched->w, touched->h})},
                               {"revision", state.doc.revision()}});
    });

    table.add("tool.pointer",
              "Feed one pointer phase (press, drag, release, cancel) to the current tool", {},
              {"phase", "at", "mode"},
              [&state](const json& args) -> CommandResult {
        if (!args.contains("phase") || !args.at("phase").is_string()) {
            return fail("tool.pointer", "'phase' must be one of press, drag, release, cancel");
        }
        const std::string phase = args.at("phase").get<std::string>();
        if (phase != "press" && phase != "drag" && phase != "release" && phase != "cancel") {
            return fail("tool.pointer", "unknown phase '" + phase +
                                            "' (press, drag, release, cancel)");
        }
        std::optional<Vec2> point;
        if (args.contains("at")) {
            Vec2 parsed;
            if (!arg_vec2(args, "at", parsed) || !std::isfinite(parsed.x) ||
                !std::isfinite(parsed.y)) {
                return fail("tool.pointer", "'at' must be finite [x,y] document pixels");
            }
            point = parsed;
        }
        if ((phase == "press" || phase == "drag") && !point) {
            return fail("tool.pointer", "'at' must be [x,y] in document pixels");
        }
        if (args.contains("mode") && phase != "press") {
            return fail("tool.pointer", "'mode' is only valid on a selection press");
        }
        // Which function runs is the tool table's answer, not a name compared here: a tool that is
        // in the table is reachable through this command, and one that is not has no command either.
        const std::optional<ToolAction> action = state.select.active
                                                     ? std::optional<ToolAction>(ToolAction::select)
                                                     : tool_action(state.tool);
        if (!action) {
            return fail("tool.pointer", "tool '" + state.tool + "' is not in the tool table (" +
                                            tool_names() + ")");
        }
        if (phase == "cancel") {
            if (!state.select.active) {
                return fail("tool.pointer", "no selection gesture is active to cancel");
            }
            select_cancel(state);
            return command_ok(stroke_json(state));
        }
        if (phase == "press" && state.gesturing()) {
            return fail("tool.pointer", "a pointer gesture is already active");
        }
        MaskOp mode = kDefaultSelectionMode;
        if (args.contains("mode")) {
            const std::optional<SelectionGesture> gesture = tool_selection_gesture(state.tool);
            if (*action != ToolAction::select || !gesture ||
                *gesture != SelectionGesture::lasso) {
                return fail("tool.pointer", "'mode' is only accepted by the lasso tool");
            }
            if (!args.at("mode").is_string()) {
                return fail("tool.pointer", "'mode' must be a mask operation");
            }
            const std::optional<MaskOp> parsed =
                mask_op_from_name(args.at("mode").get<std::string>());
            if (!parsed) {
                return fail("tool.pointer", "'mode' must be one of " + mask_op_names());
            }
            mode = *parsed;
        }
        switch (*action) {
            case ToolAction::coverage: {
                if (phase == "press" && !state.coverage_writable()) {
                    return fail("tool.pointer", "the coverage target is '" +
                                                    std::string(coverage_target_name(
                                                        state.coverage_target)) +
                                                    "' and layer '" + state.active_layer().name +
                                                    "' has none (mask.add)");
                }
                if (phase == "press") {
                    cover_press(state, *point);
                } else if (phase == "drag") {
                    cover_drag(state, *point);
                } else if (phase == "release") {
                    cover_release(state);
                } else {
                    return fail("tool.pointer", "unknown phase '" + phase +
                                                    "' (press, drag, release)");
                }
                break;
            }
            case ToolAction::paint:
                if (phase == "press") {
                    brush_press(state, *point);
                } else if (phase == "drag") {
                    brush_drag(state, *point);
                } else if (phase == "release") {
                    brush_release(state);
                } else {
                    return fail("tool.pointer", "unknown phase '" + phase +
                                                    "' (press, drag, release)");
                }
                break;
            case ToolAction::select:
                if (phase == "press") {
                    if (!select_press(state, *point, mode)) {
                        if (state.coverage_target == CoverageTarget::mask &&
                            !state.coverage_writable()) {
                            return fail("tool.pointer", "the coverage target is 'mask' and layer '" +
                                                             state.active_layer().name +
                                                             "' has none (mask.add)");
                        }
                        return fail("tool.pointer", "the selection gesture could not start");
                    }
                } else if (phase == "drag") {
                    select_drag(state, *point);
                } else if (phase == "release") {
                    select_release(state, point);
                } else {
                    return fail("tool.pointer", "unknown phase '" + phase +
                                                    "' (press, drag, release)");
                }
                break;
            case ToolAction::move:
                if (phase == "press") {
                    move_press(state, *point);
                } else if (phase == "drag") {
                    move_drag(state, *point);
                } else if (phase == "release") {
                    move_release(state);
                } else {
                    return fail("tool.pointer", "unknown phase '" + phase +
                                                    "' (press, drag, release)");
                }
                break;
        }
        return command_ok(stroke_json(state));
    });

    table.add("tool.set", "Set the tool, brush fields or colour the next stroke uses", {}, brush_arg_names(),
              [&state](const json& args) -> CommandResult {
        Brush next = state.brush;
        std::string next_tool = state.tool;
        Rgba8 next_color = state.color;
        if (args.contains("tool")) {
            if (!args.at("tool").is_string()) {
                return fail("tool.set", "'tool' must be a string");
            }
            next_tool = args.at("tool").get<std::string>();
            if (!tool_action(next_tool)) {
                return fail("tool.set", "unknown tool '" + next_tool + "' (" + tool_names() + ")");
            }
        }
        for (const BrushField& field : kBrushFields) {
            if (!args.contains(field.key)) {
                continue;
            }
            std::string why;
            if (!read_brush_field(args, field, next, "tool.set", why)) {
                return fail("tool.set", why);
            }
        }
        if (args.contains("color")) {
            std::uint8_t channels[4] = {0, 0, 0, 255};
            if (!arg_rgba(args, "color", channels)) {
                return fail("tool.set", "'color' must be [r,g,b,a] with each channel 0..255");
            }
            next_color = Rgba8{channels[0], channels[1], channels[2], channels[3]};
        }
        state.brush = next;
        state.tool = next_tool;
        state.color = next_color;
        return command_ok(brush_json(state));
    });

    // One select command per row of the tool table: adding a tool to kTools adds both its
    // command and its key here, and nothing in between has to be told.
    for (const ToolInfo& tool : kTools) {
        const std::string id = std::string("tool.") + tool.id;
        table.add(id, tool.title, {tool.select_key}, {}, [&state, id](const json&) -> CommandResult {
            state.tool = id.substr(std::string("tool.").size());
            return command_ok(brush_json(state));
        });
    }

    table.add("brush.larger", "Widen the brush one step", {"]"}, {},
              [&state](const json&) -> CommandResult {
        state.brush.size = clamp_brush_size(state.brush.size * kBrushSizeStep);
        return command_ok(brush_json(state));
    });

    table.add("brush.smaller", "Narrow the brush one step", {"["}, {},
              [&state](const json&) -> CommandResult {
        state.brush.size = clamp_brush_size(state.brush.size / kBrushSizeStep);
        return command_ok(brush_json(state));
    });

    // The step keys, one row of `kBrushSteps` each. The range they clamp to is the brush field's
    // own, so a step can never widen the range the slider and `tool.set` agree on.
    for (const BrushStep& step : kBrushSteps) {
        const BrushField* field = brush_field(step.field);
        if (field == nullptr) {
            log::error("brush step " + std::string(step.id) + " names no brush field: '" +
                       step.field + "' is not in the brush table");
            continue;
        }
        std::vector<std::string> bindings;
        if (step.shortcut[0] != '\0') {
            bindings.push_back(step.shortcut);
        }
        table.add(step.id, step.title, std::move(bindings), {},
                  [&state, field, direction = step.direction](const json&) -> CommandResult {
            double& target = state.brush.*(field->member);
            target = step_brush_fraction(target, direction, field->low, field->high);
            return command_ok(brush_json(state));
        });
    }

    table.add("edit.undo", "Undo the last change to the pixels", {"Ctrl+Z"}, {},
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

    table.add("edit.redo", "Redo the change that undo took back", {"Ctrl+Y"}, {},
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
