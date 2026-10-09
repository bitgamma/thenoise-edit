// SPDX-License-Identifier: Apache-2.0
#include "app/command_args.h"

#include "io/file.h"
#include "io/project.h"


namespace noise::app {

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

json image_json(const Image& image, const std::string& label) {
    return json{{"label", label},
                {"width", image.size().w},
                {"height", image.size().h},
                {"checksum", image.checksum_hex()}};
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
// Derived from the path, so there is no second fact about the file to keep in step: the app's
// own container, a flat image, or nothing yet.
const char* document_kind(const std::string& path) {
    if (path.empty()) {
        return "none";
    }
    return io::extension_of(path) == io::kProjectExtension ? "noise" : "image";
}

// The rows, hashing nothing. `app.state` is polled every frame and the layer commands answer
// clicks and slider drags.
json layers_json(const AppState& state) {
    json rows = json::array();
    const std::span<const Layer> layers = state.doc.layers();
    for (std::size_t index = 0; index < layers.size(); ++index) {
        rows.push_back(json{{"index", static_cast<int>(index)},
                            {"name", layers[index].name},
                            {"active", static_cast<int>(index) == state.doc.active()},
                            {"visible", layers[index].visible},
                            {"opacity", layers[index].opacity},
                            {"mode", std::string(blend_mode_name(layers[index].mode))}});
    }
    return rows;
}

// The same rows plus what each layer holds, for the one command that asks on purpose. Measured
// here at 2048² with three layers: ~480 ms of SHA-256, which is what made switching layers and
// dragging an opacity slider feel stuck - they are interactive, and a document-sized hash is not
// something a click may cost.
json layers_json_with_checksums(const AppState& state) {
    json rows = layers_json(state);
    const std::span<const Layer> layers = state.doc.layers();
    for (std::size_t index = 0; index < rows.size(); ++index) {
        rows[index]["checksum"] = layers[index].pixels.checksum_hex();
    }
    return rows;
}

json document_summary_json(const AppState& state) {
    return json{{"path", state.doc.path()},
                {"kind", document_kind(state.doc.path())},
                {"dirty", state.doc.dirty()},
                {"active", state.doc.active()},
                {"layers", layers_json(state)},
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

json stroke_json(const AppState& state) {
    return json{{"active", state.stroke.active},
                {"dabs", state.stroke.dabs},
                {"provisional_area",
                 json::array({state.doc.provisional_area().x, state.doc.provisional_area().y,
                              state.doc.provisional_area().w, state.doc.provisional_area().h})}};
}

json brush_json(const AppState& state) {
    return json{{"tool", state.tool},
                {"color", json::array({state.color.r, state.color.g, state.color.b,
                                       state.color.a})},
                {"size", state.brush.size},
                {"hardness", state.brush.hardness},
                {"opacity", state.brush.opacity},
                {"flow", state.brush.flow},
                {"spacing", state.brush.spacing}};
}

}  // namespace noise::app
