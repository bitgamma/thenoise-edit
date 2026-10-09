// SPDX-License-Identifier: Apache-2.0
#include "app/ui.h"

#include "core/log.h"
#include "gfx/checker.h"

#include "imgui.h"
#include "imgui_internal.h"  // DockBuilder*: the default dock layout has no public equivalent yet
#include "sokol_app.h"
#include "sokol_imgui.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace noise::app {

using nlohmann::json;

namespace {

constexpr ImU32 kBackdrop = IM_COL32(38, 38, 42, 255);
constexpr ImU32 kCanvasBorder = IM_COL32(90, 90, 100, 255);
constexpr float kRightPanelFraction = 0.30f;
constexpr float kStatusBarHeightPx = 40.0f;
constexpr float kToolPanelFraction = 0.36f;

// Docked by name below and opened by the same constants here.
constexpr const char* kWindowCanvas = "Canvas";
constexpr const char* kWindowTool = "Brush";
constexpr const char* kWindowLayers = "Layers";
constexpr const char* kWindowLog = "Log";
constexpr const char* kWindowStatus = "Status";

void draw_canvas(AppState& state, CommandTable& commands, gfx::Texture& canvas_texture,
                 const gfx::Checker& checker) {
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
    // The canvas window submits no items, so there is no item of this frame to test:
    // IsItemHovered() would report whatever widget another window submitted last.
    const bool hovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);

    if (!state.doc.empty()) {
        const Vec2 image_screen = state.view.world_to_view({0.0, 0.0});
        const ImVec2 canvas_min(origin.x + static_cast<float>(image_screen.x),
                                origin.y + static_cast<float>(image_screen.y));
        const ImVec2 canvas_max(canvas_min.x + static_cast<float>(state.view.image_width()),
                                canvas_min.y + static_cast<float>(state.view.image_height()));

        const ImVec2 check_min(std::max(canvas_min.x, origin.x), std::max(canvas_min.y, origin.y));
        const ImVec2 check_max(std::min(canvas_max.x, origin.x + avail.x),
                               std::min(canvas_max.y, origin.y + avail.y));
        if (checker.valid() && check_max.x > check_min.x && check_max.y > check_min.y) {
            const gfx::CheckerUv uv = gfx::checker_uv(
                {check_min.x, check_min.y},
                {check_max.x - check_min.x, check_max.y - check_min.y},
                {canvas_min.x, canvas_min.y}, state.view.scale);
            list->AddImage(simgui_imtextureid_with_sampler(checker.tile, checker.sampler),
                           check_min, check_max, ImVec2(uv.min_x, uv.min_y),
                           ImVec2(uv.max_x, uv.max_y));
        }

        canvas_texture.sync(state.doc.pixels(), state.doc.revision());
        if (canvas_texture.valid()) {
            list->AddImage(simgui_imtextureid(canvas_texture.view), canvas_min, canvas_max);
        }
        list->AddRect(canvas_min, canvas_max, kCanvasBorder);

        // Left button paints, and goes through the same table the wire drives, so a
        // recorded gesture and a hand-made one are the same code path.
        if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            const Vec2 at = state.view.view_to_world(
                {io.MousePos.x - origin.x, io.MousePos.y - origin.y});
            commands.invoke("tool.pointer",
                            json{{"phase", "press"},
                                 {"at", json::array({at.x, at.y})}});
        } else if (hovered && ImGui::IsMouseDown(ImGuiMouseButton_Left) &&
                   (io.MouseDelta.x != 0.0f || io.MouseDelta.y != 0.0f)) {
            const Vec2 at = state.view.view_to_world(
                {io.MousePos.x - origin.x, io.MousePos.y - origin.y});
            commands.invoke("tool.pointer",
                            json{{"phase", "drag"}, {"at", json::array({at.x, at.y})}});
        }
        if (state.stroke.active && ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
            commands.invoke("tool.pointer", json{{"phase", "release"}});
        }
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

// Every widget here is a front-end for a command: the panel, a key and a wire client cannot
// drift apart because there is nothing else to keep in step.
void draw_tool_panel(AppState& state, CommandTable& commands) {
    ImGui::Begin(kWindowTool);

    for (const ToolInfo& tool : kTools) {
        const std::string label = std::string(tool.id) + "###" + tool.id;
        if (ImGui::Selectable(label.c_str(), state.tool == tool.id)) {
            commands.invoke(std::string("tool.") + tool.id, json::object());
        }
        ImGui::SameLine();
        ImGui::TextDisabled("[%s]", tool.select_key);
    }

    // Sliders write one field at a time through tool.set, so a drag of the size slider and a
    // `tool.set {size}` from the wire are the same event as far as the app is concerned.
    float size = static_cast<float>(state.brush.size);
    if (ImGui::SliderFloat("size", &size, static_cast<float>(kMinBrushSize),
                           static_cast<float>(kMaxBrushSize), "%.1f px",
                           ImGuiSliderFlags_Logarithmic) &&
        static_cast<double>(size) != state.brush.size) {
        commands.invoke("tool.set", json{{"size", static_cast<double>(size)}});
    }
    struct Slider {
        const char* label;
        const double Brush::*member;
    };
    const Slider sliders[]{
        {"hardness", &Brush::hardness},
        {"opacity", &Brush::opacity},
        {"flow", &Brush::flow},
        {"spacing", &Brush::spacing},
    };
    for (const Slider& slider : sliders) {
        float value = static_cast<float>(state.brush.*(slider.member));
        if (ImGui::SliderFloat(slider.label, &value, 0.0f, 1.0f, "%.2f") &&
            static_cast<double>(value) != state.brush.*(slider.member)) {
            commands.invoke("tool.set", json{{slider.label, static_cast<double>(value)}});
        }
    }

    float rgba[4] = {static_cast<float>(state.color.r) / 255.0f,
                     static_cast<float>(state.color.g) / 255.0f,
                     static_cast<float>(state.color.b) / 255.0f,
                     static_cast<float>(state.color.a) / 255.0f};
    if (ImGui::ColorEdit4("color", rgba, ImGuiColorEditFlags_AlphaBar)) {
        auto channel = [](float v) {
            return static_cast<std::int64_t>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f));
        };
        commands.invoke("tool.set", json{{"color", json::array({channel(rgba[0]), channel(rgba[1]),
                                                                channel(rgba[2]), channel(rgba[3])})}});
    }

    ImGui::End();
}

void draw_layers_panel(const AppState& state) {
    ImGui::Begin(kWindowLayers);
    if (state.doc.empty()) {
        ImGui::TextUnformatted("no document");
    } else {
        ImGui::TextUnformatted(state.doc.layer_name().c_str());
        ImGui::SameLine();
        ImGui::TextDisabled("%d x %d  %.1f%%", state.doc.width(), state.doc.height(),
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

}

void draw_ui(AppState& state, CommandTable& commands, gfx::Texture& canvas_texture,
             const gfx::Checker& checker) {
    ImGuiViewport* viewport = ImGui::GetMainViewport();
    const ImGuiID dockspace = ImGui::DockSpaceOverViewport(0, viewport);
    static bool layout_built = false;
    if (!layout_built) {
        layout_built = true;
        build_default_dock(dockspace, viewport->WorkSize);
    }

    draw_canvas(state, commands, canvas_texture, checker);
    draw_tool_panel(state, commands);
    draw_layers_panel(state);
    draw_log_panel();

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
    ImGui::SameLine();
    // What a stroke would paint now, and how far back the pixels can go: enough to read a
    // screenshot back into the settings that made it.
    ImGui::TextDisabled("%s %.0f px", state.tool.c_str(), static_cast<double>(state.brush.size));
    ImGui::SameLine();
    if (state.stroke.active) {
        ImGui::TextDisabled("painting %zu dabs", state.stroke.dabs);
        ImGui::SameLine();
    }
    ImGui::TextDisabled("undo %d / redo %d", state.doc.history().undo_depth,
                        state.doc.history().redo_depth);
    ImGui::SameLine();
    ImGui::TextDisabled("%.1f ms", state.frame_ms);
    ImGui::SameLine();
    ImGui::TextDisabled("wire :%u", state.wire_port);
    ImGui::End();
}

}
