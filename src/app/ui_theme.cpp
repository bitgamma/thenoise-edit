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
    {0.055f, 0.055f, 0.043f, 1.0f},
    {0.102f, 0.094f, 0.075f, 1.0f},
    {0.122f, 0.114f, 0.090f, 1.0f},
    {0.176f, 0.161f, 0.133f, 1.0f},
    {1.0f, 1.0f, 1.0f, 0.10f},
    {0.988f, 0.847f, 0.275f, 0.24f},
    {0.988f, 0.847f, 0.275f, 1.0f},
    {0.980f, 0.976f, 0.447f, 1.0f},
    {0.988f, 0.847f, 0.275f, 0.14f},
    {0.988f, 0.847f, 0.275f, 0.24f},
    {0.035f, 0.031f, 0.020f, 1.0f},
    {0.471f, 0.659f, 0.420f, 1.0f},
    {0.949f, 0.725f, 0.310f, 1.0f},
    {0.937f, 0.553f, 0.498f, 1.0f},
    {0.533f, 0.741f, 0.914f, 1.0f},
    {0.039f, 0.039f, 0.035f, 1.0f},
    {1.0f, 1.0f, 1.0f, 0.16f},
    {0.122f, 0.114f, 0.090f, 1.0f},
    {0.176f, 0.161f, 0.133f, 1.0f},
    {1.0f, 1.0f, 1.0f, 0.10f},
    {0.988f, 0.847f, 0.275f, 0.45f},
    {0.122f, 0.114f, 0.090f, 0.98f},
    {0.988f, 0.847f, 0.275f, 0.30f},
    {0.055f, 0.055f, 0.043f, 0.78f},
};

constexpr Palette kLight{
    {0.102f, 0.094f, 0.075f, 1.0f},
    {0.176f, 0.184f, 0.184f, 1.0f},
    {0.420f, 0.400f, 0.365f, 1.0f},
    {0.992f, 0.984f, 0.965f, 1.0f},
    {0.937f, 0.922f, 0.882f, 1.0f},
    {0.961f, 0.949f, 0.925f, 1.0f},
    {1.0f, 0.996f, 0.980f, 1.0f},
    {0.0f, 0.0f, 0.0f, 0.15f},
    {0.361f, 0.294f, 0.0f, 0.30f},
    {0.988f, 0.847f, 0.275f, 1.0f},
    {0.976f, 0.788f, 0.196f, 1.0f},
    {0.988f, 0.847f, 0.275f, 0.20f},
    {0.988f, 0.847f, 0.275f, 0.36f},
    {0.220f, 0.176f, 0.0f, 1.0f},
    {0.235f, 0.510f, 0.235f, 1.0f},
    {0.580f, 0.357f, 0.0f, 1.0f},
    {0.690f, 0.196f, 0.153f, 1.0f},
    {0.078f, 0.420f, 0.690f, 1.0f},
    {0.839f, 0.827f, 0.788f, 1.0f},
    {0.176f, 0.184f, 0.184f, 0.24f},
    {0.961f, 0.949f, 0.925f, 1.0f},
    {0.984f, 0.957f, 0.835f, 1.0f},
    {0.0f, 0.0f, 0.0f, 0.14f},
    {0.361f, 0.294f, 0.0f, 0.38f},
    {0.992f, 0.984f, 0.965f, 0.98f},
    {0.361f, 0.294f, 0.0f, 0.32f},
    {0.102f, 0.094f, 0.075f, 0.38f},
};

struct Dimensions {
    ImVec2 window_padding;
    float window_rounding;
    float child_rounding;
    float popup_rounding;
    float frame_rounding;
    ImVec2 frame_padding;
    ImVec2 item_spacing;
    ImVec2 inner_spacing;
    ImVec2 cell_padding;
    float scrollbar_size;
    float scrollbar_rounding;
    float grab_min_size;
    float grab_rounding;
    float tab_rounding;
    float window_border;
    float child_border;
    float popup_border;
    float frame_border;
    float indent_spacing;
    float columns_min_spacing;
    float tab_border;
    float tab_bar_border;
    float disabled_alpha;
};

constexpr Dimensions kDimensions{
    {10.0f, 8.0f}, 9.0f, 8.0f, 9.0f, 6.0f, {9.0f, 6.0f}, {8.0f, 7.0f},
    {6.0f, 4.0f}, {6.0f, 4.0f}, 12.0f, 8.0f, 12.0f, 6.0f, 7.0f,
    1.0f, 1.0f, 1.0f, 0.0f, 18.0f, 6.0f, 0.0f, 1.0f, 0.62f};

std::optional<io::Theme> applied_theme;

void apply_surface_colors(ImVec4* colors, const Palette& p) {
    colors[ImGuiCol_Text] = p.text;
    colors[ImGuiCol_TextDisabled] = p.text_disabled;
    colors[ImGuiCol_WindowBg] = p.base;
    colors[ImGuiCol_ChildBg] = p.panel;
    colors[ImGuiCol_PopupBg] = p.panel;
    colors[ImGuiCol_Border] = p.border;
    colors[ImGuiCol_BorderShadow] = {0.0f, 0.0f, 0.0f, 0.0f};
    colors[ImGuiCol_FrameBg] = p.surface;
    colors[ImGuiCol_FrameBgHovered] = p.raised;
    colors[ImGuiCol_FrameBgActive] = p.raised;
    colors[ImGuiCol_TitleBg] = p.panel;
    colors[ImGuiCol_TitleBgActive] = p.surface;
    colors[ImGuiCol_TitleBgCollapsed] = p.panel;
    colors[ImGuiCol_MenuBarBg] = p.panel;
    colors[ImGuiCol_ScrollbarBg] = p.base;
    colors[ImGuiCol_ScrollbarGrab] = p.raised;
    colors[ImGuiCol_ScrollbarGrabHovered] = p.accent_soft;
    colors[ImGuiCol_ScrollbarGrabActive] = p.accent_strong;
}

void apply_widget_colors(ImVec4* colors, const Palette& p) {
    colors[ImGuiCol_CheckMark] = p.accent_ink;
    colors[ImGuiCol_CheckboxSelectedBg] = p.accent_soft;
    colors[ImGuiCol_SliderGrab] = p.accent;
    colors[ImGuiCol_SliderGrabActive] = p.accent_hover;
    colors[ImGuiCol_Button] = p.surface;
    colors[ImGuiCol_ButtonHovered] = p.accent_soft;
    colors[ImGuiCol_ButtonActive] = p.accent_strong;
    colors[ImGuiCol_Header] = p.accent_soft;
    colors[ImGuiCol_HeaderHovered] = p.accent_strong;
    colors[ImGuiCol_HeaderActive] = p.accent_strong;
    colors[ImGuiCol_Separator] = p.border;
    colors[ImGuiCol_SeparatorHovered] = p.accent;
    colors[ImGuiCol_SeparatorActive] = p.accent_hover;
    colors[ImGuiCol_ResizeGrip] = p.accent_soft;
    colors[ImGuiCol_ResizeGripHovered] = p.accent;
    colors[ImGuiCol_ResizeGripActive] = p.accent_hover;
    colors[ImGuiCol_InputTextCursor] = p.accent_ink;
    colors[ImGuiCol_TabHovered] = p.accent_soft;
    colors[ImGuiCol_Tab] = p.panel;
    colors[ImGuiCol_TabSelected] = p.surface;
    colors[ImGuiCol_TabSelectedOverline] = p.accent;
    colors[ImGuiCol_TabDimmed] = p.panel;
    colors[ImGuiCol_TabDimmedSelected] = p.surface;
    colors[ImGuiCol_TabDimmedSelectedOverline] = p.accent_soft;
    colors[ImGuiCol_DockingPreview] = p.accent_soft;
    colors[ImGuiCol_DockingEmptyBg] = p.base;
}

void apply_feedback_colors(ImVec4* colors, const Palette& p) {
    colors[ImGuiCol_PlotLines] = p.info;
    colors[ImGuiCol_PlotLinesHovered] = p.accent;
    colors[ImGuiCol_PlotHistogram] = p.accent;
    colors[ImGuiCol_PlotHistogramHovered] = p.accent_hover;
    colors[ImGuiCol_TableHeaderBg] = p.panel;
    colors[ImGuiCol_TableBorderStrong] = p.border;
    colors[ImGuiCol_TableBorderLight] = p.border;
    colors[ImGuiCol_TableRowBg] = {0.0f, 0.0f, 0.0f, 0.0f};
    colors[ImGuiCol_TableRowBgAlt] = p.surface;
    colors[ImGuiCol_TextLink] = p.accent_ink;
    colors[ImGuiCol_TextSelectedBg] = p.accent_strong;
    colors[ImGuiCol_TreeLines] = p.border;
    colors[ImGuiCol_DragDropTarget] = p.accent;
    colors[ImGuiCol_DragDropTargetBg] = p.accent_soft;
    colors[ImGuiCol_UnsavedMarker] = p.warning;
    colors[ImGuiCol_NavCursor] = p.accent;
    colors[ImGuiCol_NavWindowingHighlight] = p.accent;
    colors[ImGuiCol_NavWindowingDimBg] = p.modal_dim;
    colors[ImGuiCol_ModalWindowDimBg] = p.modal_dim;
}

void apply_colors(const Palette& p) {
    ImVec4* colors = ImGui::GetStyle().Colors;
    apply_surface_colors(colors, p);
    apply_widget_colors(colors, p);
    apply_feedback_colors(colors, p);
}

}

const Palette& palette(io::Theme theme) {
    switch (theme) {
        case io::Theme::dark:
            return kDark;
        case io::Theme::light:
            return kLight;
    }
    std::abort();
}

void apply(io::Theme theme) {
    if (applied_theme == theme) {
        return;
    }
    switch (theme) {
        case io::Theme::dark:
            ImGui::StyleColorsDark();
            break;
        case io::Theme::light:
            ImGui::StyleColorsLight();
            break;
    }
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowPadding = kDimensions.window_padding;
    style.WindowRounding = kDimensions.window_rounding;
    style.WindowBorderSize = kDimensions.window_border;
    style.ChildRounding = kDimensions.child_rounding;
    style.ChildBorderSize = kDimensions.child_border;
    style.PopupRounding = kDimensions.popup_rounding;
    style.PopupBorderSize = kDimensions.popup_border;
    style.FramePadding = kDimensions.frame_padding;
    style.FrameRounding = kDimensions.frame_rounding;
    style.FrameBorderSize = kDimensions.frame_border;
    style.ItemSpacing = kDimensions.item_spacing;
    style.ItemInnerSpacing = kDimensions.inner_spacing;
    style.CellPadding = kDimensions.cell_padding;
    style.IndentSpacing = kDimensions.indent_spacing;
    style.ColumnsMinSpacing = kDimensions.columns_min_spacing;
    style.ScrollbarSize = kDimensions.scrollbar_size;
    style.ScrollbarRounding = kDimensions.scrollbar_rounding;
    style.GrabMinSize = kDimensions.grab_min_size;
    style.GrabRounding = kDimensions.grab_rounding;
    style.TabRounding = kDimensions.tab_rounding;
    style.TabBorderSize = kDimensions.tab_border;
    style.TabBarBorderSize = kDimensions.tab_bar_border;
    style.DisabledAlpha = kDimensions.disabled_alpha;
    apply_colors(palette(theme));
    applied_theme = theme;
}

}
