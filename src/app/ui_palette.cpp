// SPDX-License-Identifier: Apache-2.0
#include "app/ui_internal.h"

#include "app/ui_theme.h"
#include "app/ui_icons.h"
#include "core/brush.h"
#include "imgui.h"

#include <array>
#include <string>

namespace noise::app {

namespace {

void draw_group(AppState& state, CommandTable& commands, ToolAction group) {
    const auto& colors = ui_theme::palette(state.ui_theme);
    for (const ToolInfo& tool : kTools) {
        if (tool.action != group) continue;
        const bool selected = state.tool == tool.id;
        if (selected) {
            ImGui::PushStyleColor(ImGuiCol_Button, colors.accent_soft);
            ImGui::PushStyleColor(ImGuiCol_Border, colors.accent);
            ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
        }
        ImGui::PushID(tool.id);
        const float width = ImGui::GetContentRegionAvail().x;
        const ui_icons::Icon icon = ui_icons::for_tool(tool.id).value();
        if (ui_icons::button(icon, tool.id, ImVec2(width, 39.0f), nullptr)) {
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
    if (ImGui::BeginChild("##tool_palette", ImVec2(62.0f, 0.0f), ImGuiChildFlags_Borders, flags)) {
        const ImVec2 pos = ImGui::GetWindowPos();
        const ImVec2 size = ImGui::GetWindowSize();
        state.tool_palette_window = ScreenRect{pos.x, pos.y, size.x, size.y};
        constexpr std::array groups{ToolAction::move, ToolAction::select,
                                    ToolAction::coverage, ToolAction::paint};
        for (const ToolAction group : groups) {
            if (group != groups.front()) ImGui::Separator();
            draw_group(state, commands, group);
        }
    }
    ImGui::EndChild();
}

} // namespace noise::app
