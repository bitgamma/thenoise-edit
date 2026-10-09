// SPDX-License-Identifier: Apache-2.0
#include "app/command_groups.h"

#include "app/command_args.h"

#include "app/commands.h"

#include <nlohmann/json.hpp>


namespace noise::app {

void register_view_commands(AppState& state, CommandTable& table) {
    table.add("view.fit", "Fit the document into the viewport", {"F"}, {},
              [&state](const json&) -> CommandResult {
        CommandResult guard = require_document(state);
        if (!guard.ok) {
            return guard;
        }
        request_fit(state);
        return command_ok(view_json(state));
    });

    table.add("view.set", "Set scale and origin explicitly", {}, {"scale", "origin"}, [&state](const json& args) -> CommandResult {
        if (!args.contains("scale") || !args.at("scale").is_number()) {
            return fail("view.set", "expected number 'scale'");
        }
        Vec2 origin{state.view.origin.x, state.view.origin.y};
        if (!arg_vec2(args, "origin", origin)) {
            return fail("view.set", "expected 'origin' as [x,y]");
        }
        if (!state.doc.empty()) {
            state.view.image = state.doc.size();
        }
        const ViewLimits limits = scale_limits(
            state.view.image, state.viewport, kViewPolicy.min_fit_fraction, kViewPolicy.max_scale);
        state.view.scale = clamp_scale(args.at("scale").get<double>(), limits);
        state.view.origin = origin;
        state.view = clamp_origin(state.view, state.viewport, kViewPolicy.overpan_px);
        return command_ok(view_json(state));
    });

    table.add("view.pan", "Pan by a viewport-space delta", {}, {"dx", "dy"}, [&state](const json& args) -> CommandResult {
        double dx = 0.0;
        double dy = 0.0;
        if (!want_number(args, "dx", dx) || !want_number(args, "dy", dy)) {
            return fail("view.pan", "expected number 'dx' and 'dy'");
        }
        state.view.origin.x -= dx / state.view.scale;
        state.view.origin.y -= dy / state.view.scale;
        state.view = clamp_origin(state.view, state.viewport, kViewPolicy.overpan_px);
        return command_ok(view_json(state));
    });

    table.add("view.zoom", "Zoom by a factor around a viewport-space anchor", {}, {"factor", "anchor"},
              [&state](const json& args) -> CommandResult {
        if (!args.contains("factor") || !args.at("factor").is_number()) {
            return fail("view.zoom", "expected number 'factor'");
        }
        Vec2 anchor{static_cast<double>(state.viewport.w) * 0.5,
                    static_cast<double>(state.viewport.h) * 0.5};
        if (args.contains("anchor") && !arg_vec2(args, "anchor", anchor)) {
            return fail("view.zoom", "expected 'anchor' as [x,y]");
        }
        const ViewLimits limits = scale_limits(
            state.view.image, state.viewport, kViewPolicy.min_fit_fraction, kViewPolicy.max_scale);
        state.view = zoom_at(state.view, anchor, args.at("factor").get<double>(), limits);
        state.view = clamp_origin(state.view, state.viewport, kViewPolicy.overpan_px);
        return command_ok(view_json(state));
    });
}

// A viewport this small cannot hold a document with the fit margin on both axes, so a fit
// measured against it is not a fit: the request waits for the next frame instead.
bool viewport_holds_fit(const Extent& viewport) {
    const int margins = 2 * kViewPolicy.fit_margin_px;
    return viewport.w > margins && viewport.h > margins;
}

void request_fit(AppState& state) {
    state.fit_pending = !state.doc.empty();
    apply_pending_fit(state);
}

void apply_pending_fit(AppState& state) {
    if (!state.fit_pending || state.doc.empty() || !state.viewport_published ||
        !viewport_holds_fit(state.viewport)) {
        return;
    }
    state.view = fit_view(state.doc.size(), state.viewport, kViewPolicy.fit_margin_px,
                          kViewPolicy.max_scale);
    state.fit_pending = false;
}

}  // namespace noise::app
