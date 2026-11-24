// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "app/ui.h"
#include "imgui.h"

#include <nlohmann/json.hpp>
#include <string>
#include <string_view>

namespace noise::app {

inline constexpr const char* kWindowCanvas = "Canvas";
inline constexpr const char* kWindowLayers = "Layers";
inline constexpr const char* kWindowLog = "Log";
inline constexpr const char* kWindowAiWorkspace = "AI Workspace";
inline constexpr const char* kWindowStatus = "Status";
inline constexpr float kDialogInputWidthPx = 116.0f;
inline constexpr float kClientTitlebarHeightPx = 48.0f;
inline constexpr float kClientTitlebarControlsGapPx = 12.0f;

enum class ImageDialog { image_size, upscale, canvas_size, crop };

void draw_canvas(AppState& state, CommandTable& commands, gfx::Texture& canvas_texture,
                 const gfx::Checker& checker, CoverageTint& tint, CoverageTint& mask_tint);
void draw_tool_palette(AppState& state, CommandTable& commands);
void draw_tool_settings(AppState& state, CommandTable& commands);
void draw_mask_edge_controls(AppState& state, CommandTable& commands);
void request_mask_edge_controls_open();
void draw_canvas_size_dialog(AppState& state, CommandTable& commands);
void draw_crop_dialog(AppState& state, CommandTable& commands);
void reset_canvas_dialog(AppState& state);
void close_image_dialog();
void draw_ai_workspace(AppState& state, CommandTable& commands, const nlohmann::json& intent,
                       ReferenceThumbs& thumbs, ImGuiID default_dock);
void request_image_dialog(ImageDialog dialog);
void draw_image_dialogs(AppState& state, CommandTable& commands);
void draw_ai_references(AppState& state, CommandTable& commands, ReferenceThumbs& thumbs,
                        const nlohmann::json& intent);
void draw_ai_status_line(AppState& state, CommandTable& commands, const nlohmann::json& view);
void draw_layers_panel(AppState& state, CommandTable& commands, LayerThumbs& thumbs);
float layer_panel_upper_height(float panes_height, double upper_ratio);
void draw_layer_panel_splitter(double& upper_ratio, float width, float thickness,
                               float& upper, float& lower);
void draw_log_panel();
void draw_brand_mark(const gfx::Texture& logo, bool client_titlebar);
void draw_recent_files_menu(CommandTable& commands);
void draw_recent_files_list(CommandTable& commands, std::string& open_error);
void begin_client_titlebar();
void end_client_titlebar();
float client_titlebar_controls_width();
void draw_client_titlebar_drag(std::string_view filename, float width);
void draw_client_window_controls(AppState& state, CommandTable& commands);
void draw_front_door(AppState& state, CommandTable& commands);
void draw_quit_guard(AppState& state, CommandTable& commands);

}
