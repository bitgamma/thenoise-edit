// SPDX-License-Identifier: Apache-2.0
#include "app/ui_internal.h"

#include "app/ui_theme.h"
#include "app/ui_icons.h"
#include "core/brush.h"
#include "imgui.h"

#include <algorithm>
#include <array>
#include <string>

namespace noise::app {

namespace {
constexpr float kToolRailWidthPx = 52.0f;
constexpr float kToolButtonHeightPx = 34.0f;

constexpr std::array kToolGroups{
    ToolAction::move,
    ToolAction::select,
    ToolAction::coverage,
    ToolAction::paint,
};

void draw_group(AppState& state, CommandTable& commands, ToolAction group) {
    const auto& colors = ui_theme::palette(state.ui_theme);
    for (const ToolInfo& tool : kTools) {
        if (tool.action != group) continue;
        const bool selected = state.tool == tool.id;
        if (selected) {
            ImGui::PushStyleColor(ImGuiCol_Button, colors.surface);
            ImGui::PushStyleColor(ImGuiCol_Border, colors.accent);
            ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
        }
        ImGui::PushID(tool.id);
        const float width = ImGui::GetContentRegionAvail().x;
        const ui_icons::Icon icon = ui_icons::for_tool(tool.id).value();
        if (ui_icons::button(icon, tool.id, ImVec2(width, kToolButtonHeightPx), nullptr)) {
            commands.invoke(std::string("tool.") + tool.id, nlohmann::json::object());
        }
        const ImVec2 min = ImGui::GetItemRectMin();
        const ImVec2 max = ImGui::GetItemRectMax();
        state.tool_palette_buttons.push_back(ToolPaletteButton{
            tool.id, ScreenRect{min.x, min.y, max.x - min.x, max.y - min.y}});
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
            ImGui::BeginTooltip();
            ImGui::TextUnformatted(tool.title);
            ImGui::TextDisabled("Shortcut: %s", tool.select_key);
            if (tool.action == ToolAction::coverage) {
                ImGui::TextDisabled("Edits the AI selection, not a layer mask");
            } else if (tool.action == ToolAction::paint && state.editing_mask()) {
                ImGui::TextDisabled("Target: active layer mask");
            }
            ImGui::EndTooltip();
        }
        ImGui::PopID();
        if (selected) {
            ImGui::PopStyleVar();
            ImGui::PopStyleColor(2);
        }
    }
}

} // namespace

void draw_tool_palette(AppState& state, CommandTable& commands) {
    state.tool_palette_buttons.clear();
    state.tool_palette_window = ScreenRect{};
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoScrollbar |
                                   ImGuiWindowFlags_NoScrollWithMouse;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(5.0f, 7.0f));
    if (ImGui::BeginChild("##tool_palette", ImVec2(kToolRailWidthPx, 0.0f),
                          ImGuiChildFlags_Borders, flags)) {
        const ImVec2 pos = ImGui::GetWindowPos();
        const ImVec2 size = ImGui::GetWindowSize();
        state.tool_palette_window = ScreenRect{pos.x, pos.y, size.x, size.y};
        for (std::size_t index = 0; index < kToolGroups.size(); ++index) {
            if (index > 0) ImGui::Separator();
            draw_group(state, commands, kToolGroups[index]);
        }
    }
    ImGui::EndChild();
    ImGui::PopStyleVar();
}

} // namespace noise::app
