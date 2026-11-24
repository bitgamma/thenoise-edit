// SPDX-License-Identifier: Apache-2.0
#include "app/ui_internal.h"

#include "imgui.h"
#include "imgui_internal.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

namespace noise::app {
namespace {

constexpr char kSettingsType[] = "LayerPanel";
constexpr char kSettingsEntry[] = "Layers";
constexpr char kRatioKey[] = "UpperRatio=";
constexpr float kUpperMinimumPx = 130.0f;
constexpr float kLowerMinimumPx = 120.0f;
double* upper_ratio = nullptr;

void clear_split_settings(ImGuiContext*, ImGuiSettingsHandler*) {
    if (upper_ratio != nullptr) {
        *upper_ratio = kDefaultLayerPanelUpperRatio;
    }
}

void* open_split_settings(ImGuiContext*, ImGuiSettingsHandler*, const char* name) {
    return std::strcmp(name, kSettingsEntry) == 0 ? upper_ratio : nullptr;
}

void read_split_setting(ImGuiContext*, ImGuiSettingsHandler*, void* entry, const char* line) {
    if (upper_ratio == nullptr || entry != upper_ratio ||
        std::strncmp(line, kRatioKey, sizeof(kRatioKey) - 1) != 0) {
        return;
    }
    const char* value = line + sizeof(kRatioKey) - 1;
    char* end = nullptr;
    const double parsed = std::strtod(value, &end);
    if (end != value && *end == '\0' && std::isfinite(parsed) && parsed > 0.0f && parsed < 1.0f) {
        *upper_ratio = parsed;
    }
}

void write_split_settings(ImGuiContext*, ImGuiSettingsHandler*, ImGuiTextBuffer* output) {
    if (upper_ratio != nullptr) {
        output->appendf("[LayerPanel][Layers]\nUpperRatio=%.6f\n\n", *upper_ratio);
    }
}

float minimum_upper(float panes_height) {
    return std::min(kUpperMinimumPx, panes_height * 0.5f);
}

float minimum_lower(float panes_height, float upper_minimum) {
    return std::min(kLowerMinimumPx, std::max(0.0f, panes_height - upper_minimum));
}

}

void register_layer_panel_settings(double& ratio) {
    upper_ratio = &ratio;
    if (ImGui::FindSettingsHandler(kSettingsType) != nullptr) {
        return;
    }
    ImGuiSettingsHandler handler{};
    handler.TypeName = kSettingsType;
    handler.TypeHash = ImHashStr(kSettingsType);
    handler.ClearAllFn = clear_split_settings;
    handler.ReadOpenFn = open_split_settings;
    handler.ReadLineFn = read_split_setting;
    handler.WriteAllFn = write_split_settings;
    ImGui::AddSettingsHandler(&handler);
}

float layer_panel_upper_height(float panes_height, double ratio) {
    const float available = std::max(0.0f, panes_height);
    const float upper_min = minimum_upper(available);
    const float lower_min = minimum_lower(available, upper_min);
    const float upper_max = std::max(upper_min, available - lower_min);
    return std::clamp(available * static_cast<float>(ratio), upper_min, upper_max);
}

void draw_layer_panel_splitter(double& ratio, float width, float thickness,
                               float& upper, float& lower) {
    const ImVec2 position = ImGui::GetCursorScreenPos();
    const ImRect bounds(position, ImVec2(position.x + width, position.y + thickness));
    const float upper_before = upper;
    const float panes_height = upper + lower;
    const float upper_min = minimum_upper(panes_height);
    const float lower_min = minimum_lower(panes_height, upper_min);
    const bool changed = ImGui::SplitterBehavior(
        bounds, ImGui::GetID("##layer_panel_splitter"), ImGuiAxis_Y, &upper, &lower,
        upper_min, lower_min, 2.0f, 0.0f, ImGui::GetColorU32(ImGuiCol_WindowBg));
    if (changed && panes_height > 0.0f) {
        ratio = static_cast<double>(upper / panes_height);
        ImGui::MarkIniSettingsDirty(ImGui::GetCurrentWindow());
    }
    const float moved = upper - upper_before;
    const ImVec2 grip_center(position.x + width * 0.5f,
                             position.y + moved + thickness * 0.5f);
    const ImU32 grip = ImGui::GetColorU32(ImGuiCol_TextDisabled);
    ImGui::GetWindowDrawList()->AddLine(
        ImVec2(grip_center.x - 7.0f, grip_center.y - 1.0f),
        ImVec2(grip_center.x + 7.0f, grip_center.y - 1.0f), grip, 1.0f);
    ImGui::GetWindowDrawList()->AddLine(
        ImVec2(grip_center.x - 7.0f, grip_center.y + 2.0f),
        ImVec2(grip_center.x + 7.0f, grip_center.y + 2.0f), grip, 1.0f);
    ImGui::Dummy(ImVec2(width, thickness));
}

}
