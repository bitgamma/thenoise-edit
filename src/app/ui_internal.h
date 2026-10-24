// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "app/ui.h"

#include <nlohmann/json.hpp>

namespace noise::app {

inline constexpr const char* kWindowCanvas = "Canvas";
inline constexpr const char* kWindowTool = "Tools";
inline constexpr const char* kWindowLayers = "Layers";
inline constexpr const char* kWindowLog = "Log";
inline constexpr const char* kWindowStatus = "Status";

void draw_canvas(AppState& state, CommandTable& commands, gfx::Texture& canvas_texture,
                 const gfx::Checker& checker, CoverageTint& tint);
void draw_tool_panel(AppState& state, CommandTable& commands, const nlohmann::json& intent);
void draw_ai_status_line(AppState& state, CommandTable& commands, const nlohmann::json& view);
void draw_layers_panel(AppState& state, CommandTable& commands, LayerThumbs& thumbs);
void draw_log_panel();
void draw_file_row(AppState& state, CommandTable& commands);
void draw_front_door(AppState& state, CommandTable& commands);
void draw_quit_guard(AppState& state, CommandTable& commands);

}
