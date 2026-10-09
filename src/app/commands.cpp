// SPDX-License-Identifier: Apache-2.0
#include "app/commands.h"

#include "core/base64.h"
#include "core/brush.h"
#include "core/log.h"
#include "platform/platform.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace noise::app {

using nlohmann::json;

namespace {

std::string captures_dir(const AppState& state) {
    std::filesystem::path dir = std::filesystem::path(state.runtime_dir) / "captures";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    return dir.string();
}

CommandResult fail(const std::string& what, const std::string& detail) {
    return command_error(what + ": " + detail);
}

bool want_int(const json& args, const char* key, int& out) {
    if (!args.contains(key) || !args.at(key).is_number_integer()) {
        return false;
    }
    out = args.at(key).get<int>();
    return true;
}

bool want_number(const json& args, const char* key, double& out) {
    if (!args.contains(key) || !args.at(key).is_number()) {
        return false;
    }
    out = args.at(key).get<double>();
    return true;
}

bool arg_vec2(const json& args, const char* key, Vec2& out) {
    if (!args.contains(key)) {
        return false;
    }
    const json& v = args.at(key);
    if (!v.is_array() || v.size() != 2 || !v[0].is_number() || !v[1].is_number()) {
        return false;
    }
    out = {v[0].get<double>(), v[1].get<double>()};
    return true;
}

// Absent leaves `out` untouched and succeeds; present-but-malformed fails, so a bad
// field is an error naming itself rather than a silent fallback.
bool arg_rect(const json& args, const char* key, Rect& out) {
    if (!args.contains(key)) {
        return true;
    }
    const json& v = args.at(key);
    if (!v.is_array() || v.size() != 4u ||
        !std::all_of(v.begin(), v.end(), [](const json& e) { return e.is_number_integer(); })) {
        return false;
    }
    out = Rect{v[0].get<int>(), v[1].get<int>(), v[2].get<int>(), v[3].get<int>()};
    return true;
}

bool arg_bool(const json& args, const char* key, bool& out) {
    out = false;
    if (!args.contains(key)) {
        return true;
    }
    if (!args.at(key).is_boolean()) {
        return false;
    }
    out = args.at(key).get<bool>();
    return true;
}

bool arg_rgba(const json& args, const char* key, std::uint8_t out[4]) {
    if (!args.contains(key)) {
        return true;
    }
    const json& v = args.at(key);
    if (!v.is_array() || v.size() != 4u) {
        return false;
    }
    for (std::size_t i = 0; i < 4u; ++i) {
        if (!v[i].is_number_integer() || v[i].get<int>() < 0 || v[i].get<int>() > 255) {
            return false;
        }
        out[i] = static_cast<std::uint8_t>(v[i].get<int>());
    }
    return true;
}

bool arg_points(const json& args, const char* key, std::vector<Vec2>& out) {
    if (!args.contains(key) || !args.at(key).is_array()) {
        return false;
    }
    for (const json& point : args.at(key)) {
        if (!point.is_array() || point.size() != 2u || !point[0].is_number() ||
            !point[1].is_number()) {
            return false;
        }
        out.push_back({point[0].get<double>(), point[1].get<double>()});
    }
    return true;
}

// Capture names become file names, so they carry no separators.
bool arg_name(const json& args, const char* key, std::string& out) {
    out = "capture";
    if (!args.contains(key)) {
        return true;
    }
    if (!args.at(key).is_string()) {
        return false;
    }
    out = args.at(key).get<std::string>();
    return !out.empty() && std::all_of(out.begin(), out.end(), [](char c) {
        return c == '.' || c == '_' || c == '-' ||
               (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
    });
}

json view_json(const AppState& state) {
    return json{{"scale", state.view.scale},
                {"origin", json::array({state.view.origin.x, state.view.origin.y})},
                {"image", json::array({state.view.image.w, state.view.image.h})},
                {"fit_pending", state.fit_pending}};
}

// A viewport this small cannot hold a document with the fit margin on both axes, so a fit
// measured against it is not a fit: the request waits for the next frame instead.
bool viewport_holds_fit(const Extent& viewport) {
    const int margins = 2 * kViewPolicy.fit_margin_px;
    return viewport.w > margins && viewport.h > margins;
}

json image_json(const Image& image, const std::string& label) {
    return json{{"label", label},
                {"width", image.size().w},
                {"height", image.size().h},
                {"checksum", image.checksum_hex()}};
}

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

CommandResult require_document(const AppState& state) {
    if (state.doc.empty()) {
        return fail("no document", "open or create one first (doc.open / doc.new)");
    }
    return command_ok(json::object());
}

json history_json(const Document& doc) {
    const HistoryStats history = doc.history();
    return json{{"undo_depth", history.undo_depth},
                {"redo_depth", history.redo_depth},
                {"stored_bytes", history.bytes},
                // What the budget drops whole steps at; a document whose history is full
                // loses its oldest step to the next stroke.
                {"budget_bytes", kHistoryBudgetBytes}};
}

// app.state is polled every frame by the driver, so it reports the document without hashing
// its pixels; `doc.get` is the call that pays for a checksum.
json document_summary_json(const AppState& state) {
    return json{{"path", state.doc.path()},
                {"width", state.doc.width()},
                {"height", state.doc.height()},
                {"revision", state.doc.revision()},
                {"history", history_json(state.doc)}};
}

json document_json(const AppState& state) {
    json doc = document_summary_json(state);
    doc["checksum"] = state.doc.checksum();
    return doc;
}

json brush_json(const AppState& state) {
    return json{{"tool", state.tool},
                {"size", state.brush.size},
                {"hardness", state.brush.hardness},
                {"opacity", state.brush.opacity},
                {"flow", state.brush.flow},
                {"spacing", state.brush.spacing}};
}

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

void register_commands(AppState& state, CommandTable& table, dbg::DebugWire& wire,
                       const HostHooks& hooks) {
    table.add("app.state", "Report application, document and view state", {},
              [&state, &wire](const json&) -> CommandResult {
        json doc = json::object();
        if (!state.doc.empty()) {
            doc = document_summary_json(state);
        }
        return command_ok(json{{"version", kAppVersion},
                               {"pid", platform::pid()},
                               {"frame", state.frame},
                               {"frame_ms", state.frame_ms},
                               {"ui_vertices", state.ui_vertices},
                               {"ui_vertex_limit", state.ui_vertex_limit},
                               {"canvas", json::array({state.canvas_origin.x, state.canvas_origin.y,
                                                       state.viewport.w, state.viewport.h})},
                               {"framebuffer", json::array({state.framebuffer.w, state.framebuffer.h})},
                               {"document", doc},
                               {"brush", brush_json(state)},
                               {"view", view_json(state)},
                               {"wire_port", wire.port()}});
    });

    table.add("app.quit", "Ask the application to exit cleanly", {"Escape", "Ctrl+Q"},
              [hooks](const json&) -> CommandResult {
        if (!hooks.quit) {
            return fail("app.quit", "no quit hook is attached");
        }
        hooks.quit();
        return command_ok(json{{"quitting", true}});
    });

    table.add("log.tail", "Return the most recent log lines", {}, [](const json& args) -> CommandResult {
        int count = 0;
        if (!want_int(args, "count", count) || count < 0) {
            return fail("log.tail", "expected non-negative integer 'count'");
        }
        return command_ok(json{{"lines", log::tail(static_cast<std::size_t>(count))}});
    });

    table.add("log.level", "Set the log level", {}, [](const json& args) -> CommandResult {
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

    table.add("doc.get", "Report the document (path, size, checksum, revision, history)", {},
              [&state](const json&) -> CommandResult {
        CommandResult guard = require_document(state);
        if (!guard.ok) {
            return guard;
        }
        return command_ok(document_json(state));
    });

    table.add("command.list", "List every invokable command with its title and shortcuts", {},
              [&table](const json&) -> CommandResult {
        json entries = json::array();
        for (const CommandInfo& info : table.list()) {
            entries.push_back(json{{"id", info.id},
                                   {"title", info.title},
                                   {"shortcuts", info.shortcuts}});
        }
        return command_ok(json{{"commands", entries}});
    });

    table.add("command.invoke", "Run another command through the same table the UI uses", {},
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

    table.add("doc.new", "Create an empty document", {}, [&state](const json& args) -> CommandResult {
        int width = 0;
        int height = 0;
        if (!want_int(args, "width", width) || !want_int(args, "height", height)) {
            return fail("doc.new", "expected integer 'width' and 'height'");
        }
        if (width <= 0 || height <= 0) {
            return fail("doc.new", "width and height must be positive");
        }
        std::uint8_t fill[4] = {0, 0, 0, 0};
        if (!arg_rgba(args, "fill", fill)) {
            return fail("doc.new", "'fill' must be [r,g,b,a] with each channel 0..255");
        }
        state.doc.reset(Image::filled(Extent{width, height}, fill[0], fill[1], fill[2], fill[3]),
                          std::string());
        request_fit(state);
        log::info("created image " + std::to_string(width) + "x" + std::to_string(height));
        return command_ok(document_json(state));
    });

    table.add("doc.open", "Load a PNG into the document", {}, [&state](const json& args) -> CommandResult {
        if (!args.contains("path") || !args.at("path").is_string()) {
            return fail("doc.open", "expected string 'path'");
        }
        const std::string path = args.at("path").get<std::string>();
        std::string error;
        Image loaded = Image::load_png(path, error);
        if (loaded.empty()) {
            return fail("doc.open", error + " (" + path + ")");
        }
        state.doc.reset(std::move(loaded), path);
        request_fit(state);
        log::info("loaded " + path);
        return command_ok(document_json(state));
    });

    table.add("doc.save", "Save the document (defaults to its current path)", {"Ctrl+S"},
              [&state](const json& args) -> CommandResult {
        CommandResult guard = require_document(state);
        if (!guard.ok) {
            return guard;
        }
        std::string path = state.doc.path();
        if (args.contains("path")) {
            if (!args.at("path").is_string()) {
                return fail("doc.save", "'path' must be a string");
            }
            path = args.at("path").get<std::string>();
        }
        if (path.empty()) {
            return fail("doc.save", "document has no path and none was given");
        }
        std::string error;
        if (!state.doc.pixels().save_png(path, error)) {
            return fail("doc.save", error);
        }
        state.doc.set_path(path);
        return command_ok(document_json(state));
    });

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
                                   {"revision", state.doc.revision()},
                                   {"checksum", state.doc.checksum()}});
        }
        std::optional<Rect> touched;
        state.doc.paint(bounds, [&](Image& image) { touched = paint_stroke(image, path, brush, color, mode); });
        if (!touched) {
            return fail("tool.stroke", "the stroke painted nothing inside the document");
        }
        return command_ok(json{{"tool", tool},
                               {"dabs", dabs},
                               {"touched",
                                json::array({touched->x, touched->y, touched->w, touched->h})},
                               {"revision", state.doc.revision()},
                               {"checksum", state.doc.checksum()}});
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

    table.add("image.get", "Return document pixels (whole image or a region)", {},
              [&state](const json& args) -> CommandResult {
        CommandResult guard = require_document(state);
        if (!guard.ok) {
            return guard;
        }
        Rect region{0, 0, state.doc.width(), state.doc.height()};
        if (!arg_rect(args, "region", region)) {
            return fail("image.get", "'region' must be [x, y, width, height] of integers");
        }
        Image crop = state.doc.pixels().region(region);
        if (crop.empty()) {
            return fail("image.get", "region is empty after clipping to the image bounds");
        }
        const std::vector<std::uint8_t> png = crop.encode_png();
        json result = image_json(crop, "region");
        result["region"] = json::array({region.x, region.y, crop.size().w, crop.size().h});
        bool as_base64 = false;
        if (!arg_bool(args, "base64", as_base64)) {
            return fail("image.get", "'base64' must be a boolean");
        }
        if (as_base64) {
            result["png_base64"] = base64_encode(std::span<const std::uint8_t>(png));
        } else {
            const std::string path = captures_dir(state) + "/image-" + crop.checksum_hex().substr(0, 12) + ".png";
            std::string error;
            if (!crop.save_png(path, error)) {
                return fail("image.get", error);
            }
            result["png_path"] = path;
        }
        return command_ok(result);
    });

    table.add("view.fit", "Fit the document into the viewport", {"F"},
              [&state](const json&) -> CommandResult {
        CommandResult guard = require_document(state);
        if (!guard.ok) {
            return guard;
        }
        request_fit(state);
        return command_ok(view_json(state));
    });

    table.add("view.set", "Set scale and origin explicitly", {}, [&state](const json& args) -> CommandResult {
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

    table.add("view.pan", "Pan by a viewport-space delta", {}, [&state](const json& args) -> CommandResult {
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

    table.add("view.zoom", "Zoom by a factor around a viewport-space anchor", {},
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

    table.add("screenshot", "Capture framebuffer pixels to a PNG", {},
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
        std::string name;
        if (!arg_name(args, "name", name)) {
            return fail("screenshot", "'name' must be a non-empty [A-Za-z0-9._-] string");
        }
        const std::string path = captures_dir(state) + "/" + name + "-" + sha.substr(0, 12) + ".png";
        std::string error;
        if (!crop.save_png(path, error)) {
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
            result["png_base64"] = base64_encode(std::span<const std::uint8_t>(crop.encode_png()));
        }
        return command_ok(result);
    });

    table.add("pixels.compare", "Compare two PNGs byte for byte", {}, [](const json& args) -> CommandResult {
        if (!args.contains("a") || !args.at("a").is_string() || !args.contains("b") ||
            !args.at("b").is_string()) {
            return fail("pixels.compare", "expected string 'a' and 'b' PNG paths");
        }
        const std::string path_a = args.at("a").get<std::string>();
        const std::string path_b = args.at("b").get<std::string>();
        std::string error;
        const Image a = Image::load_png(path_a, error);
        if (a.empty()) {
            return fail("pixels.compare", error + " (" + path_a + ")");
        }
        const Image b = Image::load_png(path_b, error);
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

    table.add("wire.info", "Report debug wire status", {}, [&wire](const json&) -> CommandResult {
        return command_ok(json{{"port", wire.port()}, {"clients", wire.client_count()}});
    });
}

}
