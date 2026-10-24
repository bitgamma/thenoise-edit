// SPDX-License-Identifier: Apache-2.0
#include "app/ui.h"

#include "app/ui_internal.h"
#include "app/ui_theme.h"
#include "app/verb_bar.h"

#include "imgui.h"
#include "imgui_internal.h"  // DockBuilder*: the default dock layout has no public equivalent yet
#include "sokol_app.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <string>

namespace noise::app {

using nlohmann::json;

namespace {

struct DockWidth {
    int minimum_width;
    float right_fraction;
};
constexpr std::array<DockWidth, 3> kDockWidths{{{1200, 0.30f}, {900, 0.34f}, {0, 0.40f}}};
// Two lines: the document's own facts, and beneath them the AI line rule F asks for. A bar with one
// line gives the AI line about a third of its width after the zoom, size, path and undo counts, and a
// clipped warning about a model swap is worse than no warning.
constexpr float kStatusBarHeightPx = 86.0f;
constexpr float kToolPanelFraction = 0.64f;
// Default docking layout, applied once per process so a run that starts from a
// fresh imgui.ini always looks the same (the capture tests depend on that).
void build_default_dock(ImGuiID dockspace, const ImVec2& size) {
    ImGui::DockBuilderRemoveNode(dockspace);
    ImGui::DockBuilderAddNode(dockspace, ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderSetNodeSize(dockspace, size);

    ImGuiID right = 0;
    ImGuiID down = 0;
    ImGuiID main_area = dockspace;
    float right_fraction = kDockWidths.back().right_fraction;
    for (const DockWidth& width : kDockWidths) {
        if (size.x >= static_cast<float>(width.minimum_width)) {
            right_fraction = width.right_fraction;
            break;
        }
    }
    ImGui::DockBuilderSplitNode(main_area, ImGuiDir_Right, right_fraction, &right, &main_area);
    ImGui::DockBuilderSplitNode(main_area, ImGuiDir_Down,
                                std::clamp(kStatusBarHeightPx / std::max(size.y, 1.0f),
                                           0.02f, 0.25f),
                                &down, &main_area);
    ImGui::DockBuilderGetNode(down)->LocalFlags |= ImGuiDockNodeFlags_NoTabBar;

    ImGuiID tool_slot = 0;
    ImGuiID lower_right = 0;
    ImGui::DockBuilderSplitNode(right, ImGuiDir_Up, kToolPanelFraction, &tool_slot, &lower_right);

    ImGui::DockBuilderDockWindow(kWindowCanvas, main_area);
    ImGui::DockBuilderDockWindow(kWindowTool, tool_slot);
    ImGui::DockBuilderDockWindow(kWindowLayers, lower_right);
    ImGui::DockBuilderDockWindow(kWindowLog, lower_right);
    ImGui::DockBuilderDockWindow(kWindowStatus, down);
    ImGui::DockBuilderFinish(dockspace);
}

void draw_status_bar(AppState& state, CommandTable& commands, const json& ai_view) {
    ImGui::Begin(kWindowStatus, nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                                         ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar);
    ImGui::Text("%.1f%%", state.view.scale * 100.0);
    ImGui::SameLine();
    ImGui::Text("%d x %d", state.doc.width(), state.doc.height());
    ImGui::SameLine();
    if (!state.doc.path().empty()) {
        ImGui::TextUnformatted(state.doc.path().c_str());
    } else {
        ImGui::TextDisabled("unsaved");
    }
    if (!state.pick_note.empty()) {
        ImGui::SameLine();
        ImGui::TextDisabled("%s", state.pick_note.c_str());
    }
    ImGui::SameLine();
    ImGui::TextDisabled("%s %.0f px", state.tool.c_str(), static_cast<double>(state.brush.size));
    if (state.doc.selection()) {
        const Rect bounds = state.doc.selection()->bounds();
        ImGui::SameLine();
        ImGui::TextDisabled("sel %d x %d", bounds.w, bounds.h);
    }
    if (state.stroke.active) {
        ImGui::SameLine();
        ImGui::TextDisabled("painting %zu dabs", state.stroke.dabs);
    }
    ImGui::SameLine();
    ImGui::TextDisabled("undo %d / redo %d", state.doc.history().undo_depth,
                        state.doc.history().redo_depth);
    char frame_text[32]{};
    std::snprintf(frame_text, sizeof(frame_text), "%.1f ms", state.frame_ms);
    const std::string wire_text = "wire :" + std::to_string(state.wire_port);
    const float spacing = ImGui::GetStyle().ItemSpacing.x;
    const float right_edge = ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x;
    const float needed_width = ImGui::CalcTextSize(frame_text).x +
                               ImGui::CalcTextSize(wire_text.c_str()).x + spacing * 2.0f;
    if (ImGui::GetItemRectMax().x + needed_width <= right_edge) {
        ImGui::SameLine();
        ImGui::TextDisabled("%s", frame_text);
        ImGui::SameLine();
        ImGui::TextDisabled("%s", wire_text.c_str());
    }
    ImGui::NewLine();
    draw_ai_status_line(state, commands, ai_view);
    ImGui::End();
}

}

void draw_ui(AppState& state, CommandTable& commands, gfx::Texture& canvas_texture,
             const gfx::Checker& checker, CoverageTint& tint, LayerThumbs& thumbs) {
    ui_theme::apply(state.ui_theme);
    // The door is opened here, before the menu bar is drawn, because a menu item's callback is the
    // frame that sets it — and `OpenPopup` from inside a menu attaches the popup to a menu window that
    // closes at the end of that same frame, which is an ImGui assertion rather than a dialog. One
    // frame later the menu is gone and the popup has somewhere to live.
    draw_front_door(state, commands);
    draw_quit_guard(state, commands);
    ImGuiViewport* viewport = ImGui::GetMainViewport();
    const ImGuiID dockspace = ImGui::DockSpaceOverViewport(0, viewport);
    static bool layout_built = false;
    if (!layout_built) {
        layout_built = true;
        build_default_dock(dockspace, viewport->WorkSize);
    }

    draw_canvas(state, commands, canvas_texture, checker, tint);
    // One read of the AI action per frame, shared by the verb rows and the status line: the answer is
    // what a press would do and what it costs, and asking twice would ask the settle rule to notice a
    // change in half a frame.
    const json ai_view = commands.invoke("ai.intent", json::object()).value;
    draw_tool_panel(state, commands, ai_view);
    draw_layers_panel(state, commands, thumbs);
    draw_log_panel();
    // Last, so the bar sits above the canvas it is anchored to and above the panels it mirrors.
    draw_verb_bar(state, commands);
    draw_status_bar(state, commands, ai_view);
}

}
