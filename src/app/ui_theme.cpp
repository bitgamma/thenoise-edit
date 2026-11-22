// SPDX-License-Identifier: Apache-2.0
#include "app/ui_theme.h"

#include <cstdlib>
#include <optional>

namespace noise::app::ui_theme {
namespace {

constexpr Palette kDark{
    {0.949f, 0.937f, 0.898f, 1.0f},
    {0.780f, 0.761f, 0.710f, 1.0f},
    {0.659f, 0.639f, 0.604f, 1.0f},
    {0.055f, 0.055f, 0.051f, 1.0f},
    {0.098f, 0.094f, 0.086f, 1.0f},
    {0.129f, 0.122f, 0.110f, 1.0f},
    {0.184f, 0.169f, 0.145f, 1.0f},
    {1.0f, 1.0f, 1.0f, 0.10f},
    {0.988f, 0.847f, 0.275f, 0.24f},
    {0.79f, 0.49f, 0.16f, 1.0f},
    {0.93f, 0.62f, 0.25f, 1.0f},
    {0.36f, 0.23f, 0.10f, 1.0f},
    {0.88f, 0.51f, 0.14f, 1.0f},
    {0.949f, 0.949f, 0.95f, 1.0f},
    {0.48f, 0.65f, 0.43f, 1.0f},
    {0.86f, 0.63f, 0.25f, 1.0f},
    {0.78f, 0.33f, 0.28f, 1.0f},
    {0.67f, 0.63f, 0.55f, 1.0f},
    {0.125f, 0.122f, 0.114f, 1.0f},
    {0.19f, 0.16f, 0.12f, 1.0f},
    {0.22f, 0.20f, 0.17f, 1.0f},
    {0.15f, 0.14f, 0.12f, 1.0f},
    {0.30f, 0.27f, 0.22f, 1.0f},
    {0.56f, 0.38f, 0.18f, 0.78f},
    {0.086f, 0.082f, 0.075f, 0.95f},
    {0.52f, 0.43f, 0.30f, 0.78f},
    {0.055f, 0.055f, 0.051f, 0.78f},
};

constexpr Palette kLight{
    {0.102f, 0.094f, 0.075f, 1.0f},
    {0.29f, 0.27f, 0.23f, 1.0f},
    {0.43f, 0.39f, 0.31f, 1.0f},
    {0.992f, 0.984f, 0.965f, 1.0f},
    {0.925f, 0.910f, 0.875f, 1.0f},
    {0.961f, 0.949f, 0.925f, 1.0f},
    {1.0f, 0.996f, 0.980f, 1.0f},
    {0.0f, 0.0f, 0.0f, 0.15f},
    {0.361f, 0.294f, 0.0f, 0.30f},
    {0.68f, 0.39f, 0.09f, 1.0f},
    {0.82f, 0.50f, 0.16f, 1.0f},
    {0.96f, 0.84f, 0.65f, 1.0f},
    {0.72f, 0.42f, 0.10f, 1.0f},
    {0.16f, 0.14f, 0.11f, 1.0f},
    {0.31f, 0.49f, 0.27f, 1.0f},
    {0.69f, 0.45f, 0.12f, 1.0f},
    {0.70f, 0.25f, 0.20f, 1.0f},
    {0.39f, 0.36f, 0.30f, 1.0f},
    {0.149f, 0.149f, 0.165f, 1.0f},
    {0.353f, 0.353f, 0.392f, 1.0f},
    {0.92f, 0.92f, 0.94f, 1.0f},
    {0.96f, 0.95f, 0.92f, 1.0f},
    {0.63f, 0.57f, 0.45f, 1.0f},
    {0.56f, 0.38f, 0.18f, 0.78f},
    {0.94f, 0.94f, 0.96f, 0.95f},
    {0.45f, 0.45f, 0.50f, 0.78f},
    {0.102f, 0.094f, 0.075f, 0.38f},
};

std::optional<io::Theme> applied_theme;

}  // namespace

const Palette& palette(io::Theme theme) {
    switch (theme) {
        case io::Theme::dark: return kDark;
        case io::Theme::light: return kLight;
    }
    std::abort();
}

void apply(io::Theme theme) {
    if (applied_theme == theme) return;
    switch (theme) {
        case io::Theme::dark: ImGui::StyleColorsDark(); break;
        case io::Theme::light: ImGui::StyleColorsLight(); break;
    }
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 2.0f;
    style.ChildRounding = 2.0f;
    style.FrameRounding = 2.0f;
    style.PopupRounding = 2.0f;
    style.ScrollbarRounding = 2.0f;
    style.GrabRounding = 2.0f;
    style.TabRounding = 2.0f;
    style.WindowBorderSize = 1.0f;
    style.ChildBorderSize = 1.0f;
    style.PopupBorderSize = 1.0f;
    style.FrameBorderSize = 1.0f;
    style.WindowPadding = ImVec2(9.0f, 7.0f);
    style.FramePadding = ImVec2(6.0f, 4.0f);
    style.ItemSpacing = ImVec2(7.0f, 5.0f);
    style.ItemInnerSpacing = ImVec2(5.0f, 4.0f);
    style.IndentSpacing = 16.0f;
    style.GrabMinSize = 12.0f;
    style.ScrollbarSize = 12.0f;
    const Palette& colors = palette(theme);
    ImVec4* ui = ImGui::GetStyle().Colors;
    ui[ImGuiCol_Text] = colors.text;
    ui[ImGuiCol_TextDisabled] = colors.text_disabled;
    ui[ImGuiCol_WindowBg] = colors.base;
    ui[ImGuiCol_ChildBg] = colors.panel;
    ui[ImGuiCol_PopupBg] = colors.raised;
    ui[ImGuiCol_Border] = colors.border;
    ui[ImGuiCol_BorderShadow] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    ui[ImGuiCol_FrameBg] = colors.surface;
    ui[ImGuiCol_FrameBgHovered] = colors.raised;
    ui[ImGuiCol_FrameBgActive] = colors.raised;
    ui[ImGuiCol_TitleBg] = colors.panel;
    ui[ImGuiCol_TitleBgActive] = colors.panel;
    ui[ImGuiCol_MenuBarBg] = colors.panel;
    ui[ImGuiCol_ScrollbarBg] = colors.base;
    ui[ImGuiCol_ScrollbarGrab] = colors.border_strong;
    ui[ImGuiCol_ScrollbarGrabHovered] = colors.accent;
    ui[ImGuiCol_ScrollbarGrabActive] = colors.accent_hover;
    ui[ImGuiCol_CheckMark] = colors.accent;
    ui[ImGuiCol_SliderGrab] = colors.accent;
    ui[ImGuiCol_SliderGrabActive] = colors.accent_hover;
    ui[ImGuiCol_Button] = colors.raised;
    ui[ImGuiCol_ButtonHovered] = colors.accent_soft;
    ui[ImGuiCol_ButtonActive] = colors.accent_strong;
    ui[ImGuiCol_Header] = colors.raised;
    ui[ImGuiCol_HeaderHovered] = colors.surface;
    ui[ImGuiCol_HeaderActive] = colors.accent_soft;
    ui[ImGuiCol_Separator] = colors.border;
    ui[ImGuiCol_SeparatorHovered] = colors.accent;
    ui[ImGuiCol_SeparatorActive] = colors.accent_hover;
    ui[ImGuiCol_ResizeGrip] = colors.accent_soft;
    ui[ImGuiCol_ResizeGripHovered] = colors.accent;
    ui[ImGuiCol_ResizeGripActive] = colors.accent_hover;
    ui[ImGuiCol_Tab] = colors.panel;
    ui[ImGuiCol_TabHovered] = colors.raised;
    ui[ImGuiCol_TabSelected] = colors.accent_soft;
    ui[ImGuiCol_TabSelectedOverline] = colors.accent;
    ui[ImGuiCol_TabDimmed] = colors.panel;
    ui[ImGuiCol_TabDimmedSelected] = colors.surface;
    ui[ImGuiCol_TabDimmedSelectedOverline] = colors.accent_soft;
    ui[ImGuiCol_NavHighlight] = colors.accent;
    ui[ImGuiCol_DragDropTarget] = colors.accent_hover;
    ui[ImGuiCol_TableHeaderBg] = colors.panel;
    ui[ImGuiCol_TableBorderStrong] = colors.border_strong;
    ui[ImGuiCol_TableBorderLight] = colors.border;
    ui[ImGuiCol_TableRowBg] = colors.base;
    ui[ImGuiCol_TableRowBgAlt] = colors.surface;
    applied_theme = theme;
}

}  // namespace noise::app::ui_theme
