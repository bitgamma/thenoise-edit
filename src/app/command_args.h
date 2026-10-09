// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "app/state.h"
#include "core/command.h"

#include <nlohmann/json.hpp>

#include <cstdint>
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
bool want_number(const json& args, const char* key, double& out);
bool arg_vec2(const json& args, const char* key, Vec2& out);
bool arg_rect(const json& args, const char* key, Rect& out);
bool arg_bool(const json& args, const char* key, bool& out);
bool arg_rgba(const json& args, const char* key, std::uint8_t out[4]);
bool arg_points(const json& args, const char* key, std::vector<Vec2>& out);
bool arg_name(const json& args, const char* key, std::string& out);
json view_json(const AppState& state);
json image_json(const Image& image, const std::string& label);
CommandResult require_document(const AppState& state);
json history_json(const Document& doc);
json document_summary_json(const AppState& state);
json document_json(const AppState& state);
json brush_json(const AppState& state);

}  // namespace noise::app
