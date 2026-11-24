// SPDX-License-Identifier: Apache-2.0
#include "app/ui_internal.h"

#include "app/brush_steps.h"
#include "app/ui_theme.h"

#include "imgui.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <string>
#include <string_view>

namespace noise::app {

using nlohmann::json;

namespace {

constexpr float kToolSettingsLabelWidthPx = 58.0f;
constexpr float kToolSettingsValueWidthPx = 104.0f;
constexpr float kToolSettingsValueFraction = 0.42f;
constexpr float kToolSettingsRowHeightPx = 22.0f;
constexpr double kBrushSizeInputStep = 1.0;
constexpr double kBrushSizeInputFastStep = 10.0;
constexpr double kBrushFractionInputFastStep = 5.0 * kBrushFractionStep;

struct BrushControl {
    const char* name;
    const char* label;
    double Brush::*member;
    double low;
    double high;
    double step;
    double fast_step;
    const char* format;
    bool logarithmic;
};

constexpr std::array<BrushControl, 5> kBrushControls{{
    {"size", "Size", &Brush::size, kMinBrushSize, kMaxBrushSize,
     kBrushSizeInputStep, kBrushSizeInputFastStep, "%.1f", true},
    {"hardness", "Hardness", &Brush::hardness, 0.0, 1.0,
     kBrushFractionStep, kBrushFractionInputFastStep, "%.2f", false},
    {"opacity", "Opacity", &Brush::opacity, 0.0, 1.0,
     kBrushFractionStep, kBrushFractionInputFastStep, "%.2f", false},
    {"flow", "Flow", &Brush::flow, 0.0, 1.0,
     kBrushFractionStep, kBrushFractionInputFastStep, "%.2f", false},
    {"spacing", "Spacing", &Brush::spacing, 0.0, 1.0,
     kBrushFractionStep, kBrushFractionInputFastStep, "%.2f", false},
}};

ScreenRect item_rect() {
    const ImVec2 min = ImGui::GetItemRectMin();
    const ImVec2 max = ImGui::GetItemRectMax();
    return ScreenRect{min.x, min.y, max.x - min.x, max.y - min.y};
}

ScreenRect union_rect(ScreenRect a, ScreenRect b) {
    if (a.empty()) return b;
    if (b.empty()) return a;
    const float left = std::min(a.x, b.x);
    const float top = std::min(a.y, b.y);
    const float right = std::max(a.x + a.w, b.x + b.w);
    const float bottom = std::max(a.y + a.h, b.y + b.h);
    return ScreenRect{left, top, right - left, bottom - top};
}

std::string brush_step_hint(const CommandTable& commands, const char* field) {
    std::string hint;
    for (const BrushStep& step : kBrushSteps) {
        if (std::string_view(step.field) != field) continue;
        for (const std::string& binding : commands.shortcuts_for(step.id)) {
            if (!hint.empty()) hint += "\n";
            hint += binding;
        }
    }
    return hint;
}

void store_control(AppState& state, const BrushControl& control, ScreenRect rect) {
    state.tool_option_controls.push_back(ToolPaletteButton{control.name, rect});
}

void draw_brush_control(AppState& state, CommandTable& commands,
                        const BrushControl& control) {
    const double current = state.brush.*(control.member);
    float slider_value = static_cast<float>(current);
    const float low = static_cast<float>(control.low);
    const float high = static_cast<float>(control.high);
    ImGui::PushID(control.name);
    ImGui::TableNextRow(ImGuiTableRowFlags_None, kToolSettingsRowHeightPx);
    ImGui::TableNextColumn();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(control.label);
    ImGui::TableNextColumn();
    const ImGuiSliderFlags flags = control.logarithmic ? ImGuiSliderFlags_Logarithmic
                                                        : ImGuiSliderFlags_None;
    ImGui::SetNextItemWidth(-1.0f);
    const bool slider_changed = ImGui::SliderFloat("##track", &slider_value, low, high,
                                                    "", flags);
    const ScreenRect slider_rect = item_rect();
    ImGui::TableNextColumn();
    ImGui::SetNextItemWidth(-1.0f);
    double numeric_value = slider_changed ? static_cast<double>(slider_value) : current;
    const bool input_changed = ImGui::InputDouble("##value", &numeric_value,
                                                   control.step, control.fast_step,
                                                   control.format);
    const ScreenRect input_rect = item_rect();
    store_control(state, control, union_rect(slider_rect, input_rect));
    if (slider_changed || input_changed) {
        commands.invoke("tool.set", json{{control.name, numeric_value}});
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
        const std::string hint = brush_step_hint(commands, control.name);
        if (!hint.empty()) ImGui::SetTooltip("step keys\n%s", hint.c_str());
    }
    ImGui::PopID();
}

void draw_brush_color(AppState& state, CommandTable& commands) {
    ImGui::TextUnformatted("Color");
    ImGui::SameLine();
    float rgba[4] = {static_cast<float>(state.color.r) / 255.0f,
                     static_cast<float>(state.color.g) / 255.0f,
                     static_cast<float>(state.color.b) / 255.0f,
                     static_cast<float>(state.color.a) / 255.0f};
    const bool changed = ImGui::ColorEdit4("##brush_color", rgba,
        ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel |
        ImGuiColorEditFlags_AlphaBar);
    const ScreenRect bounds = item_rect();
    state.tool_option_controls.push_back(ToolPaletteButton{"color", bounds});
    if (!changed) return;
    const auto channel = [](float value) {
        return static_cast<std::int64_t>(std::lround(std::clamp(value, 0.0f, 1.0f) * 255.0f));
    };
    commands.invoke("tool.set", json{{"color", json::array({channel(rgba[0]), channel(rgba[1]),
                                                                channel(rgba[2]), channel(rgba[3])})}});
}

void draw_brush_settings(AppState& state, CommandTable& commands, bool has_color) {
    ImGui::TextDisabled("%s · %s", state.tool.c_str(),
                        state.editing_mask() ? "layer mask" :
                        tool_action(state.tool) == ToolAction::coverage ? "AI selection" :
                        "layer pixels");
    constexpr ImGuiTableFlags kTableFlags = ImGuiTableFlags_SizingStretchProp |
                                             ImGuiTableFlags_NoSavedSettings;
    if (ImGui::BeginTable("##tool_settings", 3, kTableFlags)) {
        const float value_width = std::min(kToolSettingsValueWidthPx,
            ImGui::GetContentRegionAvail().x * kToolSettingsValueFraction);
        ImGui::TableSetupColumn("label", ImGuiTableColumnFlags_WidthFixed,
                                kToolSettingsLabelWidthPx);
        ImGui::TableSetupColumn("track", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("value", ImGuiTableColumnFlags_WidthFixed,
                                value_width);
        for (const BrushControl& control : kBrushControls) {
            draw_brush_control(state, commands, control);
        }
        ImGui::EndTable();
    }
    if (has_color) draw_brush_color(state, commands);
}

void draw_tool_description(const AppState& state) {
    for (const ToolInfo& tool : kTools) {
        if (state.tool == tool.id) {
            ImGui::TextDisabled("%s", tool.title);
            return;
        }
    }
}

}

void draw_tool_settings(AppState& state, CommandTable& commands) {
    state.tool_option_controls.clear();
    state.tool_options_window = ScreenRect{};
    ImGui::SetNextItemOpen(false, ImGuiCond_Once);
    const bool open = ImGui::CollapsingHeader("Tool Settings");
    ScreenRect bounds = item_rect();
    if (open) {
        const std::optional<ToolAction> action = tool_action(state.tool);
        const bool paints = action == ToolAction::paint || action == ToolAction::coverage;
        const bool has_color = action == ToolAction::paint &&
                               tool_mode(state.tool) == PaintMode::Over &&
                               !state.editing_mask();
        if (paints) {
            draw_brush_settings(state, commands, has_color);
        } else {
            draw_tool_description(state);
            if (action == ToolAction::select) {
                ImGui::TextDisabled("Selection commands are in Select");
            }
        }
        bounds = union_rect(bounds, item_rect());
    }
    state.tool_options_window = bounds;
}

}
