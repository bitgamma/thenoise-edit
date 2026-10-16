// SPDX-License-Identifier: Apache-2.0
#include "app/verb_bar.h"

#include "imgui.h"
#include "imgui_internal.h"  // BringWindowToDisplayFront: the bar has to sit above the canvas
#include <nlohmann/json.hpp>

#include <algorithm>
#include <string>

#include "app/state.h"
#include "core/geom.h"

namespace noise::app {
namespace {

using nlohmann::json;

// The bar floats clear of the mark: its own edge must never cover the edge being fixed, and a bar over
// the boundary is unreadable exactly where the feather is.
constexpr double kBarGapPx = 10.0;
constexpr double kBarPadPx = 6.0;

}  // namespace

void draw_verb_bar(AppState& state, CommandTable& commands) {
    state.verb_bar = {};
    const std::optional<Rect> mark = verb_bar_mark(state);
    if (!mark) {
        return;
    }
    const json rows = verb_bar_rows(commands, nullptr);
    if (rows.empty()) {
        return;
    }

    const Vec2 top_left = state.view.world_to_view(
        {static_cast<double>(mark->x), static_cast<double>(mark->y)});
    const Vec2 bottom_right = state.view.world_to_view(
        {static_cast<double>(mark->x + mark->w), static_cast<double>(mark->y + mark->h)});
    const double anchor_x = state.canvas_origin.x + top_left.x;
    const double anchor_y = state.canvas_origin.y + top_left.y;
    const double far_x = state.canvas_origin.x + bottom_right.x;
    const double far_y = state.canvas_origin.y + bottom_right.y;

    // Measured here rather than in the command: text width is ImGui's to know, and the command has to
    // answer outside a frame too, where no font atlas is loaded.
    const ImGuiStyle& style = ImGui::GetStyle();
    double width = kBarPadPx * 2.0;
    double height = 0.0;
    for (const json& row : rows) {
        const ImVec2 text = ImGui::CalcTextSize(row["label"].get<std::string>().c_str());
        width += text.x + style.FramePadding.x * 2.0 + style.ItemSpacing.x;
        height = std::max(height, static_cast<double>(text.y + style.FramePadding.y * 2.0f));
    }
    width -= style.ItemSpacing.x;
    height += kBarPadPx * 2.0;

    // Above the mark when there is room, below when there is not, and always inside the canvas area so
    // the bar cannot hide behind the panel edge.
    double x = anchor_x + (far_x - anchor_x - width) * 0.5;
    double y = anchor_y - kBarGapPx - height;
    if (y < state.canvas_origin.y) {
        y = far_y + kBarGapPx;
    }
    const double limit_x = state.canvas_origin.x + state.viewport.w - width;
    const double limit_y = state.canvas_origin.y + state.viewport.h - height;
    x = std::min(std::max(x, state.canvas_origin.x), std::max(state.canvas_origin.x, limit_x));
    y = std::min(std::max(y, state.canvas_origin.y), std::max(state.canvas_origin.y, limit_y));

    state.verb_bar = {static_cast<float>(x), static_cast<float>(y), static_cast<float>(width),
                      static_cast<float>(height)};

    ImGui::SetNextWindowPos(ImVec2(state.verb_bar.x, state.verb_bar.y), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(state.verb_bar.w, state.verb_bar.h), ImGuiCond_Always);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, IM_COL32(22, 22, 26, 232));
    ImGui::PushStyleColor(ImGuiCol_Border, IM_COL32(120, 120, 132, 200));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(static_cast<float>(kBarPadPx),
                                                            static_cast<float>(kBarPadPx)));
    const bool began = ImGui::Begin("##verb_bar", nullptr,
                                    ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                                        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
                                        ImGuiWindowFlags_NoCollapse |
                                        ImGuiWindowFlags_NoSavedSettings |
                                        ImGuiWindowFlags_NoBringToFrontOnFocus);
    // Being submitted last is not enough: the canvas window keeps the display front, and a bar under
    // the mark it is anchored to is a bar nobody sees.
    if (began) {
        ImGui::BringWindowToDisplayFront(ImGui::GetCurrentWindow());
    }
    // Read back, so what the wire reports is where the bar is rather than where it was asked to be.
    state.verb_bar = {ImGui::GetWindowPos().x, ImGui::GetWindowPos().y, ImGui::GetWindowSize().x,
                      ImGui::GetWindowSize().y};
    // The panel keeps the full controls - the words a verb asks for, the style and the model warning.
    // The bar is the shortcut, and it presses the same command the panel button presses.
    for (std::size_t i = 0; i < rows.size(); ++i) {
        const json& row = rows[i];
        if (i > 0) {
            ImGui::SameLine();
        }
        ImGui::BeginDisabled(!row["ready"].get<bool>());
        if (ImGui::Button(row["label"].get<std::string>().c_str())) {
            commands.invoke(row["command"].get<std::string>(), json::object());
        }
        ImGui::EndDisabled();
        // Why a button is dim, where a person looks when a button does nothing. The panel states the
        // same reason beside its own copy; the bar has no room for it and no need to restate it.
        if (!row["ready"].get<bool>() && ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s", row["reason"].get<std::string>().c_str());
        }
    }
    ImGui::End();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(2);
}

}  // namespace noise::app
