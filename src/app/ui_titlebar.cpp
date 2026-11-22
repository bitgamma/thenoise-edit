// SPDX-License-Identifier: Apache-2.0
#include "app/ui_internal.h"

#include "app/ui_theme.h"
#include "app/ui_text.h"
#include "platform/window_chrome.h"

#include "imgui.h"
#include "sokol_app.h"
#include "sokol_imgui.h"

#include <algorithm>
#include <string>
#include <string_view>

namespace noise::app {
namespace {

constexpr float kTitlebarActionWidthPx = 44.0f;
constexpr float kLogoHeightPx = 34.0f;
constexpr float kLogoMaxWidthPx = 168.0f;
constexpr float kLogoCompactMaxWidthPx = 106.0f;
constexpr float kLogoCompactWindowWidthPx = 940.0f;
constexpr float kTitleLabelPaddingPx = 12.0f;
constexpr float kTitlebarRightPaddingPx = 10.0f;
constexpr float kChromeIconHalfSizePx = 6.0f;
constexpr float kChromeStrokePx = 1.5f;

float logo_width(const gfx::Texture& logo, float height) {
    return logo.valid() && logo.size.h > 0
        ? height * static_cast<float>(logo.size.w) / static_cast<float>(logo.size.h)
        : 0.0f;
}

void chrome_icon(ImDrawList* draw, const ImVec2 center, int action, ImU32 color) {
    const float half = kChromeIconHalfSizePx;
    if (action == 0) {
        draw->AddLine(ImVec2(center.x - half, center.y),
                      ImVec2(center.x + half, center.y), color, kChromeStrokePx);
    } else if (action == 1) {
        draw->AddRect(ImVec2(center.x - half, center.y - half + 1.0f),
                      ImVec2(center.x + half, center.y + half - 1.0f),
                      color, 0.0f, 0, kChromeStrokePx);
    } else {
        draw->AddLine(ImVec2(center.x - half + 1.0f, center.y - half + 1.0f),
                      ImVec2(center.x + half - 1.0f, center.y + half - 1.0f),
                      color, kChromeStrokePx);
        draw->AddLine(ImVec2(center.x + half - 1.0f, center.y - half + 1.0f),
                      ImVec2(center.x - half + 1.0f, center.y + half - 1.0f),
                      color, kChromeStrokePx);
    }
}

bool chrome_button(const char* id, int action, const char* tooltip, const ImVec4& danger) {
    const ImVec2 size(kTitlebarActionWidthPx, kClientTitlebarHeightPx);
    ImGui::SetCursorPosY(0.0f);
    const bool pressed = ImGui::InvisibleButton(id, size);
    const bool hovered = ImGui::IsItemHovered();
    const bool active = ImGui::IsItemActive();
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec2 min = ImGui::GetItemRectMin();
    const ImVec2 max = ImGui::GetItemRectMax();
    if (active || hovered) {
        const ImU32 bg = action == 2 ? ImGui::ColorConvertFloat4ToU32(danger) :
            ImGui::GetColorU32(active ? ImGuiCol_ButtonActive : ImGuiCol_ButtonHovered);
        draw->AddRectFilled(min, max, bg);
    }
    const ImVec2 center((min.x + max.x) * 0.5f, (min.y + max.y) * 0.5f);
    chrome_icon(draw, center, action, ImGui::GetColorU32(ImGuiCol_Text));
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
        ImGui::SetTooltip("%s", tooltip);
    }
    return pressed;
}

} // namespace

float client_titlebar_controls_width() {
    return kTitlebarActionWidthPx * 3.0f + kClientTitlebarControlsGapPx +
           kTitlebarRightPaddingPx;
}

void draw_brand_mark(const gfx::Texture& logo, bool client_titlebar) {
    if (logo.valid()) {
        float height = client_titlebar ? kLogoHeightPx : ImGui::GetFrameHeight();
        if (client_titlebar) {
            const bool compact = ImGui::GetMainViewport()->WorkSize.x <
                                 kLogoCompactWindowWidthPx;
            const float max_width = compact ? kLogoCompactMaxWidthPx : kLogoMaxWidthPx;
            const float natural_width = logo_width(logo, height);
            if (natural_width > max_width) {
                height *= max_width / natural_width;
            }
            ImGui::SetCursorPosY((kClientTitlebarHeightPx - height) * 0.5f);
        }
        ImGui::Image(simgui_imtextureid(logo.view), ImVec2(logo_width(logo, height), height));
    } else {
        if (client_titlebar) {
            ImGui::SetCursorPosY((kClientTitlebarHeightPx - ImGui::GetTextLineHeight()) * 0.5f);
        }
        ImGui::TextUnformatted("TheNoise-Edit");
    }
}

void begin_client_titlebar() {
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(ImVec2(viewport->WorkSize.x, kClientTitlebarHeightPx));
    ImGui::SetNextWindowViewport(viewport->ID);

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    const float bar_padding = std::max(0.0f,
        (kClientTitlebarHeightPx - ImGui::GetFontSize()) * 0.5f);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(8.0f, bar_padding));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImGui::GetStyleColorVec4(ImGuiCol_MenuBarBg));
    ImGui::Begin("##client_titlebar", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                 ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoDocking |
                 ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar |
                 ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_MenuBar);
}

void end_client_titlebar() {
    const ImVec2 pos = ImGui::GetWindowPos();
    const float bottom = pos.y + kClientTitlebarHeightPx - 1.0f;
    const ImU32 border = ImGui::GetColorU32(ImGuiCol_Border);
    ImGui::GetWindowDrawList()->AddLine(
        ImVec2(pos.x, bottom),
        ImVec2(pos.x + ImGui::GetWindowWidth(), bottom), border);
    ImGui::End();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(4);
}

void draw_client_titlebar_drag(std::string_view filename, float width) {
    if (width < 1.0f) return;
    ImGui::SetCursorPosY(0.0f);
    const ImVec2 start = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##drag_title", ImVec2(width, kClientTitlebarHeightPx));
    if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
        platform::toggle_client_window_maximized();
    } else if (ImGui::IsItemActivated()) {
        platform::move_client_window();
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
        ImGui::SetTooltip("%.*s", static_cast<int>(filename.size()), filename.data());
    }
    const std::string shown = ui_text::truncate_filename(
        filename, std::max(0.0f, width - kTitleLabelPaddingPx * 2.0f));
    const ImVec2 text_size = ImGui::CalcTextSize(shown.c_str());
    const ImVec2 label_pos(start.x + std::max(kTitleLabelPaddingPx, (width - text_size.x) * 0.5f),
                           start.y + (kClientTitlebarHeightPx - text_size.y) * 0.5f);
    ImGui::GetWindowDrawList()->AddText(label_pos,
                                        ImGui::GetColorU32(ImGuiCol_TextDisabled),
                                        shown.c_str());
}

void draw_client_window_controls(AppState& state, CommandTable& commands) {
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ui_theme::palette(state.ui_theme).surface);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ui_theme::palette(state.ui_theme).raised);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, 0.0f));
    if (chrome_button("##minimize", 0, "Minimize", ui_theme::palette(state.ui_theme).danger)) {
        platform::minimize_client_window();
    }
    ImGui::SameLine(0.0f, 0.0f);
    if (chrome_button("##maximize", 1, "Maximize / Restore",
                      ui_theme::palette(state.ui_theme).danger)) {
        platform::toggle_client_window_maximized();
    }
    ImGui::SameLine(0.0f, 0.0f);
    if (chrome_button("##close", 2, "Close", ui_theme::palette(state.ui_theme).danger)) {
        commands.invoke("app.quit", nlohmann::json::object());
    }
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(2);
}

} // namespace noise::app
