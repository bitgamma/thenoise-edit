// SPDX-License-Identifier: Apache-2.0
#include "app/ui_internal.h"

#include "imgui.h"

#include <algorithm>
#include <string>

namespace noise::app {

using nlohmann::json;

namespace {

constexpr float kEdgeButtonPx = 24.0f;
constexpr float kEdgeSliderMinimumPx = 68.0f;
constexpr float kEdgeSliderMaximumPx = 230.0f;
constexpr float kEdgeValueFieldPx = 61.0f;
constexpr float kEdgeSliderReservedPx = 128.0f;
bool edge_controls_open_requested = false;

void draw_edge_slider(CommandTable& commands, const char* title, const char* id,
                      const char* key, int value, int minimum, int maximum,
                      int scale_minimum, int scale_maximum, std::string& error) {
    ImGui::TextUnformatted(title);
    ImGui::PushID(id);
    int next = value;
    ImGui::BeginDisabled(value <= minimum);
    if (ImGui::Button("-", ImVec2(kEdgeButtonPx, 0.0f))) next = value - 1;
    ImGui::EndDisabled();
    ImGui::SameLine();
    const float slider_width = std::clamp(ImGui::GetContentRegionAvail().x -
                                          kEdgeSliderReservedPx,
                                          kEdgeSliderMinimumPx, kEdgeSliderMaximumPx);
    ImGui::SetNextItemWidth(slider_width);
    ImGui::BeginDisabled(minimum == maximum);
    const bool moved = ImGui::SliderInt("##edge", &next, scale_minimum, scale_maximum,
                                        "");
    const bool dragging = ImGui::IsItemActive();
    const bool released = ImGui::IsItemDeactivatedAfterEdit();
    ImGui::EndDisabled();
    ImGui::SameLine();
    int typed = value;
    ImGui::SetNextItemWidth(kEdgeValueFieldPx);
    if (ImGui::InputInt("##value", &typed, 0, 0, ImGuiInputTextFlags_EnterReturnsTrue)) {
        next = std::clamp(typed, minimum, maximum);
    }
    ImGui::SameLine();
    ImGui::TextDisabled("px");
    ImGui::SameLine();
    ImGui::BeginDisabled(value >= maximum);
    if (ImGui::Button("+", ImVec2(kEdgeButtonPx, 0.0f))) next = value + 1;
    ImGui::EndDisabled();
    ImGui::PopID();

    if (next != value && (moved || !dragging)) {
        const CommandResult preview = commands.invoke("mask.edge.preview", json{{key, next}});
        error = preview.ok ? "" : preview.error;
        if (!preview.ok) return;
    }
    if ((!dragging && next != value) || released) {
        const CommandResult finish = commands.invoke("mask.edge.finish", json::object());
        error = finish.ok ? "" : finish.error;
    }
}

}

void draw_mask_edge_controls(AppState& state, CommandTable& commands) {
    CanvasForm& form = state.canvas_form;
    if (form.edge_preview_layer != 0 &&
        (state.doc.empty() || state.active_layer().id != form.edge_preview_layer)) {
        commands.invoke("mask.edge.finish", json::object());
    }
    if (!state.editing_mask()) return;
    const bool open_requested = edge_controls_open_requested;
    edge_controls_open_requested = false;
    ImGui::SetNextItemOpen(open_requested, open_requested ? ImGuiCond_Always : ImGuiCond_Once);
    if (!ImGui::CollapsingHeader("Adjust Layer Edge")) {
        if (form.edge_preview_layer != 0) {
            commands.invoke("mask.edge.finish", json::object());
        }
        return;
    }
    const CommandResult current = commands.invoke("mask.edge.state", json::object());
    if (!current.ok) {
        ImGui::TextWrapped("%s", current.error.c_str());
        return;
    }
    const json& edge = current.value;
    draw_edge_slider(commands, "Grow / shrink", "size", "shift",
                     edge.at("shift").get<int>(), -edge.at("shift_limit").get<int>(),
                     edge.at("shift_limit").get<int>(), edge.at("shift_min").get<int>(),
                     edge.at("shift_max").get<int>(), form.edge_error);
    draw_edge_slider(commands, "Feather", "feather", "feather",
                     edge.at("feather").get<int>(), 0,
                     edge.at("feather_limit").get<int>(), edge.at("feather_min").get<int>(),
                     edge.at("feather_max").get<int>(), form.edge_error);
    ImGui::TextDisabled("Reversing a value restores the original edge.");
    if (!form.edge_error.empty()) ImGui::TextWrapped("%s", form.edge_error.c_str());
}

void request_mask_edge_controls_open() {
    edge_controls_open_requested = true;
}

}
