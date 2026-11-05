// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "imgui.h"
#include "imgui_stdlib.h"

#include <algorithm>
#include <string>

namespace noise::app {

inline constexpr float kPromptMaxVisibleLines = 12.0f;
inline constexpr float kPromptMinVisibleLines = 3.0f;

inline float prompt_field_height(const std::string& text, float width, float max_height) {
    const ImGuiStyle& style = ImGui::GetStyle();
    const float line = ImGui::GetTextLineHeightWithSpacing();
    const float wrap_width = std::max(1.0f, width - style.FramePadding.x * 2.0f -
                                               style.ScrollbarSize);
    const float text_height = ImGui::CalcTextSize(text.c_str(), nullptr, false, wrap_width).y;
    return std::clamp(text_height + style.FramePadding.y * 2.0f + line,
                      kPromptMinVisibleLines * line,
                      std::max(kPromptMinVisibleLines * line, max_height));
}

inline bool prompt_input(const char* id, std::string& text,
                         float max_height, ImGuiInputTextFlags flags) {
    const float width = ImGui::GetContentRegionAvail().x;
    const float height = prompt_field_height(text, width, max_height);
    return ImGui::InputTextMultiline(id, &text, ImVec2(-1.0f, height),
                                     flags | ImGuiInputTextFlags_WordWrap);
}

}  // namespace noise::app
