// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "app/ui_internal.h"

#include "imgui.h"

namespace noise::app::dialog_fields {

inline constexpr float kLabelWidthPx = 72.0f;
inline constexpr float kUnitWidthPx = 26.0f;

inline bool begin_dimensions(const char* id) {
    constexpr ImGuiTableFlags flags = ImGuiTableFlags_SizingFixedFit |
                                      ImGuiTableFlags_NoSavedSettings |
                                      ImGuiTableFlags_NoPadOuterX;
    if (!ImGui::BeginTable(id, 3, flags)) return false;
    ImGui::TableSetupColumn("Label", ImGuiTableColumnFlags_WidthFixed, kLabelWidthPx);
    ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthFixed, kDialogInputWidthPx);
    ImGui::TableSetupColumn("Unit", ImGuiTableColumnFlags_WidthFixed, kUnitWidthPx);
    return true;
}

inline bool dimension_input(const char* label, const char* id, int& value) {
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    ImGui::TableSetColumnIndex(1);
    ImGui::SetNextItemWidth(-1.0f);
    const bool changed = ImGui::InputInt(id, &value);
    ImGui::TableSetColumnIndex(2);
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("px");
    return changed;
}

} // namespace noise::app::dialog_fields
