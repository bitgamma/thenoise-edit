// SPDX-License-Identifier: Apache-2.0
#include "app/commands.h"

#include "app/command_args.h"
#include "app/file_commands.h"
#include "app/command_groups.h"
#include "app/state.h"

#include "core/base64.h"
#include "core/log.h"
#include "io/flat.h"
#include "io/png.h"
#include "platform/platform.h"

#include <nlohmann/json.hpp>

#include <string>

namespace noise::app {

using nlohmann::json;

void register_commands(AppState& state, CommandTable& table, dbg::DebugWire& wire,
                       const HostHooks& hooks) {
    register_document_commands(state, table);
    register_ai_commands(state, table);
    register_ai_reference_commands(state, table);
    register_ai_job_commands(state, table);
    register_ai_intent_commands(state, table);
    register_ai_verb_commands(state, table);
    register_paint_commands(state, table);
    register_layer_commands(state, table);
    register_select_commands(state, table);
    register_verb_bar_commands(state, table);
    register_mask_commands(state, table);
    register_pointer_commands(state, table);
    register_view_commands(state, table);
    register_file_commands(state, table);

    table.add("app.state", "Report application, document and view state", {}, {},
              [&state, &wire](const json&) -> CommandResult {
        json option_controls = json::array();
        for (const ToolPaletteButton& control : state.tool_option_controls) {
            option_controls.push_back(json{{"id", control.id},
                                           {"screen", json::array({control.screen.x, control.screen.y,
                                                                   control.screen.w, control.screen.h})}});
        }
        json palette_buttons = json::array();
        for (const ToolPaletteButton& button : state.tool_palette_buttons) {
            palette_buttons.push_back(json{{"id", button.id},
                                           {"screen", json::array({button.screen.x, button.screen.y,
                                                                   button.screen.w, button.screen.h})}});
        }
        const auto screen_json = [](const ScreenRect& rect) {
            return json::array({rect.x, rect.y, rect.w, rect.h});
        };
        const auto choices_json = [&screen_json](const std::vector<ToolPaletteButton>& choices) {
            json result = json::array();
            for (const ToolPaletteButton& choice : choices) {
                result.push_back(json{{"id", choice.id}, {"screen", screen_json(choice.screen)}});
            }
            return result;
        };
        json doc = json::object();
        if (!state.doc.empty()) {
            doc = document_summary_json(state);
        }
        return command_ok(json{{"version", kAppVersion},
                               {"ui_theme", io::theme_name(state.ui_theme)},
                               {"pid", platform::pid()},
                               {"frame", state.frame},
                               {"frame_ms", state.frame_ms},
                               {"ui_vertices", state.ui_vertices},
                               {"ui_vertex_limit", state.ui_vertex_limit},
                               {"canvas", json::array({state.canvas_origin.x, state.canvas_origin.y,
                                                       state.viewport.w, state.viewport.h})},
                               {"framebuffer", json::array({state.framebuffer.w, state.framebuffer.h})},
                               {"document", doc},
                               {"front_door", front_door_json(state)},
                               // The question a driver has to answer before the app will leave.
                               {"quit_pending", state.quit_pending},
                               {"brush", brush_json(state)},
                               {"stroke", stroke_json(state)},
                               {"view", view_json(state)},
                               {"pointer", pointer_json(state)},
                               // Where the layer cards are, so a driver presses the one it means.
                               {"tool_palette", json{{"active", state.tool},
                                                      {"window", json::array({state.tool_palette_window.x,
                                                                               state.tool_palette_window.y,
                                                                               state.tool_palette_window.w,
                                                                               state.tool_palette_window.h})},
                                                      {"buttons", palette_buttons}}},
                               {"tool_options", json::array({state.tool_options_window.x,
                                                            state.tool_options_window.y,
                                                            state.tool_options_window.w,
                                                            state.tool_options_window.h})},
                               {"tool_option_controls", option_controls},
                               {"ai_pickers", json{{"prompt", screen_json(state.ai_prompt_input)},
                                                   {"aspect", screen_json(state.ai_aspect_picker)},
                                                   {"style", screen_json(state.ai_style_picker)},
                                                   {"aspect_options", choices_json(state.ai_aspect_options)},
                                                   {"style_options", choices_json(state.ai_style_options)}}},
                               {"layers_window", json::array({state.layers_window.x,
                                                              state.layers_window.y,
                                                              state.layers_window.w,
                                                              state.layers_window.h})},
                               {"layer_cards", layer_cards_json(state)},
                               {"layer_panel", layer_panel_json(state)},
                               {"ai_references_rect", json::array({state.ai_references_rect.x,
                                                                   state.ai_references_rect.y,
                                                                   state.ai_references_rect.w,
                                                                   state.ai_references_rect.h})},
                               {"reference_thumbs", json{{"rebuilds",
                                                           state.reference_thumb_rebuilds}}},
                               // How often the panel had to resample a picture: the caching rule as a
                               // number, so "dragging opacity rebuilds nothing" is a reading and not a
                               // promise.
                               {"layer_thumbs", json{{"rebuilds", state.layer_thumb_rebuilds}}},
                               // What the ant overlay drew, as much as what it was asked to draw:
                               // the rate is only checkable against the clock the frame used.
                               {"ants", json{{"phase_px", ant_drawn_phase(state.ants)},
                                             {"time_s", state.ants.time},
                                             {"dash_px", kAntDashPx},
                                             {"speed_px_per_second", kAntMarchPxPerSecond},
                                             {"frozen_phases", state.ants.phases}}},
                               {"wire_port", wire.port()}});
    });

    table.add("ui.theme", "Read or set the application theme", {}, {"theme"},
              [&state](const json& args) -> CommandResult {
        if (!args.contains("theme")) {
            return command_ok(json{{"theme", io::theme_name(state.ui_theme)}});
        }
        if (!args.at("theme").is_string()) {
            return fail("ui.theme", "'theme' must be 'dark' or 'light'");
        }
        const std::string name = args.at("theme").get<std::string>();
        io::Theme theme = state.ui_theme;
        if (!io::theme_from_name(name, theme)) {
            return fail("ui.theme", "unknown theme '" + name + "' (dark|light)");
        }
        std::string error;
        if (!io::save_theme(theme, error)) {
            return fail("ui.theme", "cannot save theme: " + error);
        }
        state.ui_theme = theme;
        return command_ok(json{{"theme", io::theme_name(state.ui_theme)}});
    });

    // A key does not throw away work. `Escape`, `Ctrl+Q` and the window manager's close all come
    // through here, so the guard is in the command rather than in either caller: with unsaved changes
    // the ask becomes a pending decision, which the window shows and a driver answers with `force` -
    // or withdraws by asking again, which is what a second `Escape` means.
    table.add("app.quit", "Ask the application to exit cleanly ('force' discards unsaved changes)",
              {"Escape", "Ctrl+Q"}, {"force"},
              [&state, hooks](const json& args) -> CommandResult {
        if (args.contains("force") && !args.at("force").is_boolean()) {
            return fail("app.quit", "'force' must be true or false");
        }
        const bool force = args.value("force", false);
        if (!force && state.doc.dirty()) {
            state.quit_pending = !state.quit_pending;
            return command_ok(json{{"quitting", false},
                                   {"pending", state.quit_pending},
                                   {"dirty", true},
                                   {"path", state.doc.path().empty()
                                                ? json(nullptr)
                                                : json(state.doc.path())}});
        }
        if (!hooks.quit) {
            return fail("app.quit", "no quit hook is attached");
        }
        state.quit_pending = false;
        hooks.quit();
        return command_ok(json{{"quitting", true}, {"pending", false}});
    });

    table.add("log.tail", "Return the most recent log lines", {}, {"count"}, [](const json& args) -> CommandResult {
        int count = 0;
        if (!want_int(args, "count", count) || count < 0) {
            return fail("log.tail", "expected non-negative integer 'count'");
        }
        return command_ok(json{{"lines", log::tail(static_cast<std::size_t>(count))}});
    });

    table.add("log.level", "Set the log level", {}, {"level"}, [](const json& args) -> CommandResult {
        if (!args.contains("level") || !args.at("level").is_string()) {
            return fail("log.level", "expected string 'level' (debug|info|warn|error)");
        }
        const std::string level = args.at("level").get<std::string>();
        if (level == "debug") { log::set_level(log::Level::Debug); }
        else if (level == "info") { log::set_level(log::Level::Info); }
        else if (level == "warn") { log::set_level(log::Level::Warn); }
        else if (level == "error") { log::set_level(log::Level::Error); }
        else { return fail("log.level", "unknown level '" + level + "'"); }
        return command_ok(json{{"level", level}});
    });

    table.add("command.list", "List every invokable command with its title, shortcuts and arguments",
              {}, {},
              [&table](const json&) -> CommandResult {
        json entries = json::array();
        for (const CommandInfo& info : table.list()) {
            entries.push_back(json{{"id", info.id},
                                   {"title", info.title},
                                   {"shortcuts", info.shortcuts},
                                   // What a command takes, so a caller can ask rather than guess and
                                   // have an argument refused.
                                   {"args", info.args}});
        }
        return command_ok(json{{"commands", entries}});
    });

    table.add("command.invoke", "Run another command through the same table the UI uses", {}, {"name", "args"},
              [&table](const json& args) -> CommandResult {
        if (!args.contains("name") || !args.at("name").is_string()) {
            return fail("command.invoke", "expected string 'name'");
        }
        json nested = json::object();
        if (args.contains("args")) {
            nested = args.at("args");
        }
        const std::string name = args.at("name").get<std::string>();
        if (name == "command.invoke") {
            return fail("command.invoke", "refusing to invoke itself");
        }
        const CommandResult inner = table.invoke(name, nested);
        if (!inner.ok) {
            return command_error(name + ": " + inner.error);
        }
        return command_ok(json{{"name", name}, {"result", inner.value}});
    });

    table.add("screenshot", "Capture framebuffer pixels to a PNG", {}, {"region", "name", "base64"},
              [&state, capture = hooks.capture](const json& args) -> CommandResult {
        if (!capture) {
            return fail("screenshot", "no framebuffer capture is attached");
        }
        Rect region{0, 0, state.framebuffer.w, state.framebuffer.h};
        if (!arg_rect(args, "region", region)) {
            return fail("screenshot", "'region' must be [x, y, width, height] of integers");
        }
        region = clamp_rect(region, state.framebuffer);
        if (region.empty()) {
            return fail("screenshot", "region is empty after clipping to the framebuffer");
        }
        Image frame = capture(state.framebuffer.w, state.framebuffer.h);
        Image crop = frame.region(region);
        const std::string sha = crop.checksum_hex();
        std::string name = "capture";
        if (!arg_name(args, "name", name)) {
            return fail("screenshot", "'name' must be a non-empty [A-Za-z0-9._-] string");
        }
        const std::string path = captures_dir(state) + "/" + name + "-" + sha.substr(0, 12) + ".png";
        std::string error;
        if (!io::save_png(path, crop, error)) {
            return fail("screenshot", error);
        }
        json result = image_json(crop, path);
        result["path"] = path;
        result["sha256"] = sha;
        bool as_base64 = false;
        if (!arg_bool(args, "base64", as_base64)) {
            return fail("screenshot", "'base64' must be a boolean");
        }
        if (as_base64) {
            result["png_base64"] = base64_encode(std::span<const std::uint8_t>(io::encode_png(crop)));
        }
        return command_ok(result);
    });

    table.add("pixels.compare", "Compare the pixels of two images", {}, {"a", "b"},
              [](const json& args) -> CommandResult {
        if (!args.contains("a") || !args.at("a").is_string() || !args.contains("b") ||
            !args.at("b").is_string()) {
            return fail("pixels.compare", "expected string 'a' and 'b' image paths");
        }
        const std::string path_a = args.at("a").get<std::string>();
        const std::string path_b = args.at("b").get<std::string>();
        std::string error;
        // Any flat format, decoded: what is compared is pixels, and a probe should not have to
        // convert a golden it was handed just to be allowed to compare it.
        const Image a = io::load_flat_image(path_a, error);
        if (a.empty()) {
            return fail("pixels.compare", error + " (" + path_a + ")");
        }
        const Image b = io::load_flat_image(path_b, error);
        if (b.empty()) {
            return fail("pixels.compare", error + " (" + path_b + ")");
        }
        if (a.size().w != b.size().w || a.size().h != b.size().h) {
            return command_ok(json{{"equal", false},
                                   {"reason", "size mismatch"},
                                   {"a", json::array({a.size().w, a.size().h})},
                                   {"b", json::array({b.size().w, b.size().h})}});
        }
        std::size_t differing = 0;
        int max_delta = 0;
        for (std::size_t i = 0; i < a.bytes(); ++i) {
            const int delta = std::abs(static_cast<int>(a.data()[i]) - static_cast<int>(b.data()[i]));
            if (delta != 0) {
                ++differing;
                max_delta = std::max(max_delta, delta);
            }
        }
        return command_ok(json{{"equal", differing == 0},
                               {"width", a.size().w},
                               {"height", a.size().h},
                               {"differing_bytes", differing},
                               {"max_channel_delta", max_delta}});
    });

    // The one place the ants' clock may be stopped. Animating them needs wall time and wall time is
    // the only input a screenshot cannot pin down, so a probe that wants to see the pattern - which
    // side of the boundary is lit at a given offset, whether the four sides agree - asks for the
    // phases to draw and gets the overlay frozen at those offsets. With an empty list the overlay
    // animates again.
    table.add("ants.freeze", "Draw the marching ants at fixed phases instead of animating",
              {}, {"phases"}, [&state](const json& args) -> CommandResult {
        state.ants.phases.clear();
        if (args.contains("phases")) {
            if (!args.at("phases").is_array()) {
                return fail("ants.freeze", "'phases' must be an array of numbers");
            }
            for (const json& value : args.at("phases")) {
                if (!value.is_number()) {
                    return fail("ants.freeze", "'phases' must be an array of numbers");
                }
                state.ants.phases.push_back(value.get<float>());
            }
        }
        return command_ok(json{{"phases", state.ants.phases}});
    });

    table.add("wire.info", "Report debug wire status", {}, {}, [&wire](const json&) -> CommandResult {
        return command_ok(json{{"port", wire.port()}, {"clients", wire.client_count()}});
    });
}

}  // namespace noise::app
