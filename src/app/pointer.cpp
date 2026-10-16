// SPDX-License-Identifier: Apache-2.0
#include "app/pointer.h"

#include "app/command_args.h"
#include "app/state.h"

#include <nlohmann/json.hpp>

namespace noise::app {

PointerView resolve_pointer(const AppState& state, std::optional<Vec2> over_canvas,
                            std::optional<bool> footprint_forced) {
    PointerView view;
    view.over_canvas = over_canvas.has_value();
    view.at = over_canvas;
    view.shape = tool_cursor(state.tool).value_or(PointerShape::arrow);
    // `Brush::size` is a diameter in document pixels, so this is the only place the zoom enters it.
    view.footprint_px = state.brush.size * state.view.scale;
    view.core_px = view.footprint_px * state.brush.hardness;
    view.drawn = view.over_canvas &&
                 footprint_forced.value_or(view.shape == PointerShape::brush);
    return view;
}

PointerRequest pointer_request(const PointerView& view) {
    if (!view.over_canvas) {
        return PointerRequest::leave_alone;
    }
    if (view.drawn && view.shape == PointerShape::brush) {
        return PointerRequest::hidden;
    }
    switch (view.shape) {
        case PointerShape::arrow:
            return PointerRequest::arrow;
        case PointerShape::crosshair:
            return PointerRequest::crosshair;
        case PointerShape::brush:
            // The ring is off: a crosshair keeps the landing point visible.
            return PointerRequest::crosshair;
        case PointerShape::move:
            return PointerRequest::move;
    }
    return PointerRequest::arrow;
}

json pointer_json(const AppState& state) {
    const PointerView& view = state.pointer;
    json at = nullptr;
    if (view.at) {
        at = json::array({view.at->x, view.at->y});
    }
    return json{{"shape", pointer_shape_name(view.shape)},
                {"over_canvas", view.over_canvas},
                {"drawn", view.drawn},
                {"footprint_px", view.footprint_px},
                {"core_px", view.core_px},
                {"hardness", state.brush.hardness},
                {"at", at},
                {"forced", state.footprint_forced.has_value()
                               ? json(*state.footprint_forced)
                               : json(nullptr)},
                {"cursor", [&view] {
                    switch (pointer_request(view)) {
                        case PointerRequest::leave_alone:
                            return "left alone";
                        case PointerRequest::arrow:
                            return "arrow";
                        case PointerRequest::crosshair:
                            return "crosshair";
                        case PointerRequest::move:
                            return "move";
                        case PointerRequest::hidden:
                            return "hidden";
                    }
                    return "arrow";
                }()}};
}

void register_pointer_commands(AppState& state, CommandTable& table) {
    // The ring a probe measures has to be forceable, because the tool alone would draw it only where a
    // pointer actually is - and a wire client has no pointer.
    table.add("pointer.footprint",
              "Read the pointer overlay, or force the brush ring on and off ('show': true, false, null)",
              {}, {"show"}, [&state](const json& args) -> CommandResult {
        if (args.contains("show")) {
            const json& show = args.at("show");
            if (show.is_null()) {
                state.footprint_forced = std::nullopt;
            } else if (show.is_boolean()) {
                state.footprint_forced = show.get<bool>();
            } else {
                return fail("pointer.footprint", "expected 'show' as true, false or null");
            }
        }
        return command_ok(pointer_json(state));
    });
}

}  // namespace noise::app
