// SPDX-License-Identifier: Apache-2.0
#include "app/command_groups.h"

#include "app/command_args.h"

#include "core/mask.h"

#include <nlohmann/json.hpp>

#include <optional>
#include <string>

namespace noise::app {
namespace {

// What `select.rect` does when the caller does not say: the marquee the pointer draws replaces, and a
// client that means to combine shapes says so.
constexpr MaskOp kSelectOpDefault = MaskOp::replace;

// The operation to apply, from `args`. A name the table does not have is refused with the names it
// does have, because "mode": "intersect" reads as if it were obvious.
bool want_op(const json& args, MaskOp& op, std::string& why) {
    op = kSelectOpDefault;
    if (!args.contains("mode")) {
        return true;
    }
    if (!args.at("mode").is_string()) {
        why = "'mode' must be a string";
        return false;
    }
    const std::string name = args.at("mode").get<std::string>();
    const std::optional<MaskOp> parsed = mask_op_from_name(name);
    if (!parsed) {
        why = "'" + name + "' is not a mask operation (" + mask_op_names() + ")";
        return false;
    }
    op = *parsed;
    return true;
}

// The mask a command starts from: the document's, or an empty one where none is set. Size is always
// the canvas's, which is what `Document::set_selection` insists on.
Mask current_or_empty(const AppState& state) {
    if (state.doc.selection()) {
        return *state.doc.selection();
    }
    return Mask(state.doc.size());
}

}  // namespace

void register_select_commands(AppState& state, CommandTable& table) {
    table.add("select.rect", "Set, add to or subtract from the selection with a rectangle", {},
              {"rect", "mode"}, [&state](const json& args) -> CommandResult {
        CommandResult guard = require_document(state);
        if (!guard.ok) {
            return guard;
        }
        Rect rect;
        if (!arg_rect(args, "rect", rect)) {
            return fail("select.rect", "'rect' must be [x,y,w,h] in document pixels");
        }
        if (rect.w <= 0 || rect.h <= 0) {
            return fail("select.rect",
                        "'rect' must have a positive width and height, got " +
                            std::to_string(rect.w) + "x" + std::to_string(rect.h));
        }
        MaskOp op = kSelectOpDefault;
        std::string why;
        if (!want_op(args, op, why)) {
            return fail("select.rect", why);
        }
        // `replace` starts from nothing so a rectangle that lands wholly outside the canvas leaves
        // nothing selected, which is what the caller described; the combining operations start from
        // what is selected now.
        Mask selection = op == MaskOp::replace ? Mask(state.doc.size()) : current_or_empty(state);
        selection.op(rect, op);
        if (!state.doc.set_selection(selection)) {
            return fail("select.rect", "a selection of " + std::to_string(selection.size().w) +
                                           "x" + std::to_string(selection.size().h) +
                                           " does not fit a document of " +
                                           std::to_string(state.doc.size().w) + "x" +
                                           std::to_string(state.doc.size().h));
        }
        return command_ok(selection_json(state));
    });

    table.add("select.all", "Select the whole canvas", {}, {}, [&state](const json&) -> CommandResult {
        CommandResult guard = require_document(state);
        if (!guard.ok) {
            return guard;
        }
        Mask selection(state.doc.size());
        selection.fill(255);
        state.doc.set_selection(selection);
        return command_ok(selection_json(state));
    });

    table.add("select.clear", "Put the selection down", {"Ctrl+D"}, {},
              [&state](const json&) -> CommandResult {
        CommandResult guard = require_document(state);
        if (!guard.ok) {
            return guard;
        }
        state.doc.set_selection(std::nullopt);
        return command_ok(selection_json(state));
    });

    // No selection means "everything is writable", which inverted is "nothing is" - so inverting an
    // absent selection selects the canvas, the way an editor that treats the empty marquee as the
    // empty set does.
    table.add("select.invert", "Select what is not selected", {"Ctrl+I"}, {},
              [&state](const json&) -> CommandResult {
        CommandResult guard = require_document(state);
        if (!guard.ok) {
            return guard;
        }
        Mask selection = current_or_empty(state);
        selection.invert();
        state.doc.set_selection(selection);
        return command_ok(selection_json(state));
    });

    table.add("select.get", "Report the selection", {}, {}, [&state](const json&) -> CommandResult {
        return command_ok(selection_json(state));
    });
}

}  // namespace noise::app
