// SPDX-License-Identifier: Apache-2.0
#include "app/ui.h"

#include "core/log.h"

#include "imgui.h"
#include "imgui_internal.h"  // DockBuilder*: the default dock layout has no public equivalent yet
#include "sokol_app.h"
#include "sokol_imgui.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace noise::app {

using nlohmann::json;

namespace {

constexpr ImU32 kCheckerLight = IM_COL32(204, 204, 204, 255);
constexpr ImU32 kCheckerDark = IM_COL32(153, 153, 153, 255);
constexpr ImU32 kBackdrop = IM_COL32(38, 38, 42, 255);
constexpr ImU32 kCanvasBorder = IM_COL32(90, 90, 100, 255);
constexpr int kCheckerWorldPx = 8;
constexpr float kCheckerMinTile = 4.0f;
constexpr float kCheckerMaxTile = 64.0f;
constexpr float kRightPanelFraction = 0.30f;
constexpr float kStatusBarHeightPx = 40.0f;

// Docked by name below and opened by the same constants here.
constexpr const char* kWindowCanvas = "Canvas";
constexpr const char* kWindowLayers = "Layers";
constexpr const char* kWindowLog = "Log";
constexpr const char* kWindowStatus = "Status";

void draw_canvas(AppState& state, CommandTable& commands, gfx::Texture& canvas_texture) {
    if (!ImGui::Begin(kWindowCanvas)) {
        ImGui::End();
        return;
    }
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    state.viewport.w = std::max(1, static_cast<int>(avail.x));
    state.viewport.h = std::max(1, static_cast<int>(avail.y));

    ImDrawList* list = ImGui::GetWindowDrawList();
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    state.canvas_origin = {origin.x, origin.y};
    list->AddRectFilled(origin, ImVec2(origin.x + avail.x, origin.y + avail.y), kBackdrop);

    ImGuiIO& io = ImGui::GetIO();
    const bool hovered = ImGui::IsItemHovered();

    if (!state.doc.image.empty()) {
        const Vec2 image_screen = state.view.world_to_view({0.0, 0.0});
        const ImVec2 canvas_min(origin.x + static_cast<float>(image_screen.x),
                                origin.y + static_cast<float>(image_screen.y));
        const ImVec2 canvas_max(canvas_min.x + static_cast<float>(state.view.image_width()),
                                canvas_min.y + static_cast<float>(state.view.image_height()));

        const float tile = std::clamp(static_cast<float>(kCheckerWorldPx) *
                                          static_cast<float>(state.view.scale),
                                      kCheckerMinTile, kCheckerMaxTile);
        const float first_x = canvas_min.x - std::fmod(canvas_min.x - origin.x, tile);
        const float first_y = canvas_min.y - std::fmod(canvas_min.y - origin.y, tile);
        for (float y = first_y; y < canvas_max.y; y += tile) {
            for (float x = first_x; x < canvas_max.x; x += tile) {
                const ImVec2 top_left(std::max(x, canvas_min.x), std::max(y, canvas_min.y));
                const ImVec2 bottom_right(std::min(x + tile, canvas_max.x),
                                          std::min(y + tile, canvas_max.y));
                if (bottom_right.x <= top_left.x || bottom_right.y <= top_left.y) {
                    continue;
                }
                const int col = static_cast<int>(std::floor((top_left.x - first_x) / tile));
                const int row = static_cast<int>(std::floor((top_left.y - first_y) / tile));
                list->AddRectFilled(top_left, bottom_right,
                                    ((col + row) % 2 == 0) ? kCheckerLight : kCheckerDark);
            }
        }

        canvas_texture.sync(state.doc.image, state.doc.revision);
        if (canvas_texture.valid()) {
            list->AddImage(simgui_imtextureid(canvas_texture.view), canvas_min, canvas_max);
        }
        list->AddRect(canvas_min, canvas_max, kCanvasBorder);

        if (hovered && ImGui::IsMouseDown(ImGuiMouseButton_Right)) {
            commands.invoke("view.pan", json{{"dx", static_cast<double>(io.MouseDelta.x)},
                                             {"dy", static_cast<double>(io.MouseDelta.y)}});
        }
        if (hovered && io.MouseWheel != 0.0f) {
            const double factor = io.MouseWheel > 0.0f ? kViewPolicy.wheel_step
                                                       : 1.0 / kViewPolicy.wheel_step;
            commands.invoke("view.zoom",
                            json{{"factor", factor},
                                 {"anchor", json::array({io.MousePos.x - origin.x,
                                                         io.MousePos.y - origin.y})}});
        }
    }
    ImGui::End();
}

void draw_layers_panel(const AppState& state) {
    ImGui::Begin(kWindowLayers);
    if (state.doc.image.empty()) {
        ImGui::TextUnformatted("no document");
    } else {
        ImGui::TextUnformatted("Layer 1");
        ImGui::SameLine();
        ImGui::TextDisabled("%d x %d  %.1f%%", state.doc.image.size().w, state.doc.image.size().h,
                            state.view.scale * 100.0);
    }
    ImGui::End();
}

void draw_log_panel() {
    ImGui::Begin(kWindowLog);
    static bool follow = true;
    ImGui::Checkbox("Follow", &follow);
    if (ImGui::BeginChild("log-lines")) {
        for (const std::string& line : log::tail(400)) {
            ImGui::TextUnformatted(line.c_str());
        }
        if (follow && ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 4.0f) {
            ImGui::SetScrollHereY(1.0f);
        }
    }
    ImGui::EndChild();
    ImGui::End();
}

// Default docking layout, applied once per process so a run that starts from a
// fresh imgui.ini always looks the same (the capture tests depend on that).
void build_default_dock(ImGuiID dockspace, const ImVec2& size) {
    ImGui::DockBuilderRemoveNode(dockspace);
    ImGui::DockBuilderAddNode(dockspace, ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderSetNodeSize(dockspace, size);

    ImGuiID right = 0;
    ImGuiID down = 0;
    ImGuiID main_area = dockspace;
    ImGui::DockBuilderSplitNode(main_area, ImGuiDir_Right, kRightPanelFraction, &right, &main_area);
    ImGui::DockBuilderSplitNode(main_area, ImGuiDir_Down,
                                std::clamp(kStatusBarHeightPx / std::max(size.y, 1.0f),
                                           0.02f, 0.25f),
                                &down, &main_area);

    ImGui::DockBuilderDockWindow(kWindowCanvas, main_area);
    ImGui::DockBuilderDockWindow(kWindowLayers, right);
    ImGui::DockBuilderDockWindow(kWindowLog, right);
    ImGui::DockBuilderDockWindow(kWindowStatus, down);
    ImGui::DockBuilderFinish(dockspace);
}

}

void draw_ui(AppState& state, CommandTable& commands, gfx::Texture& canvas_texture) {
    ImGuiViewport* viewport = ImGui::GetMainViewport();
    const ImGuiID dockspace = ImGui::DockSpaceOverViewport(0, viewport);
    static bool layout_built = false;
    if (!layout_built) {
        layout_built = true;
        build_default_dock(dockspace, viewport->WorkSize);
    }

    draw_canvas(state, commands, canvas_texture);
    draw_layers_panel(state);
    draw_log_panel();

    ImGui::Begin(kWindowStatus, nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                                         ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar);
    ImGui::Text("%.1f%%", state.view.scale * 100.0);
    ImGui::SameLine();
    ImGui::Text("%d x %d", state.doc.image.size().w, state.doc.image.size().h);
    ImGui::SameLine();
    if (!state.doc.path.empty()) {
        ImGui::TextUnformatted(state.doc.path.c_str());
    } else {
        ImGui::TextDisabled("unsaved");
    }
    ImGui::SameLine();
    ImGui::TextDisabled("%.1f ms", state.frame_ms);
    ImGui::SameLine();
    ImGui::TextDisabled("wire :%u", state.wire_port);
    ImGui::End();
}

}
