// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "app/state.h"
#include "core/command.h"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace noise::app {

using nlohmann::json;

// Argument readers shared by every command group. Each one leaves the destination untouched
// and returns false on a malformed field, so a bad argument is an error naming itself rather
// than a silent fallback.

std::string captures_dir(const AppState& state);
CommandResult fail(const std::string& what, const std::string& detail);
bool want_int(const json& args, const char* key, int& out);
// `want_*` demands the key; `arg_*` treats an absent key as "leave what the caller had", so a default
// belongs to the caller's variable and to nowhere else.
bool arg_int(const json& args, const char* key, int& out);
bool want_number(const json& args, const char* key, double& out);
bool arg_vec2(const json& args, const char* key, Vec2& out);
bool arg_rect(const json& args, const char* key, Rect& out);
bool arg_bool(const json& args, const char* key, bool& out);
bool arg_rgba(const json& args, const char* key, std::uint8_t out[4]);
bool arg_points(const json& args, const char* key, std::vector<Vec2>& out);
bool arg_name(const json& args, const char* key, std::string& out);
// Free text: a prompt, a dialog title, a path. Unlike `arg_name` it takes spaces and accepts an
// empty value, because "clear the prompt" and "no title" are things a person does.
bool arg_text(const json& args, const char* key, std::string& out);

// An aspect choice: an "W:H" string, or `null` to put it back. Absent leaves what the caller held, so a
// key press that carries nothing keeps what the panel chose. Whether the recipe offers that ratio is the
// door's question, not this reader's.
bool arg_aspect(const json& args, const char* key, std::optional<AspectRatio>& out);
json view_json(const AppState& state);
json image_json(const Image& image, const std::string& label);
CommandResult require_document(const AppState& state);
json history_json(const Document& doc);
// The layer rows, hashing nothing: these answer clicks and slider drags.
// A layer's mask as `layer.list` reports it: null for no mask, otherwise what it covers.
json layer_mask_json(const Layer& layer);
json layers_json(const AppState& state);
// The same rows with what each layer holds. Only the diagnostic that asks for it pays; hashing a
// stack to answer a click costs a document-sized SHA-256 per event.
json layers_json_with_checksums(const AppState& state);
json document_summary_json(const AppState& state);
json document_json(const AppState& state);
json brush_json(const AppState& state);
json stroke_json(const AppState& state);
// The selection: whether there is one, what it covers and where. Reading it costs the mask's derived
// facts, which the mask caches, so only the first call after a change scans.
json selection_json(const AppState& state);

}  // namespace noise::app
