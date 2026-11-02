// SPDX-License-Identifier: Apache-2.0
#include "app/ui_internal.h"

#include "app/ants.h"
#include "app/ui_theme.h"
#include "gfx/checker.h"

#include "imgui.h"
#include "sokol_app.h"
#include "sokol_imgui.h"

#include <algorithm>
#include <cmath>
#include <optional>
#include <vector>

namespace noise::app {

using nlohmann::json;

namespace {
constexpr ImU32 kAntLight = IM_COL32(255, 255, 255, 255);
constexpr ImU32 kAntDark = IM_COL32(20, 20, 20, 255);
constexpr ImU32 kLassoUnderlay = IM_COL32(0, 0, 0, 220);
constexpr ImU32 kLassoLine = IM_COL32(255, 255, 255, 255);
constexpr ImU32 kFootprint = IM_COL32(255, 255, 255, 235);
constexpr float kFootprintOutlinePx = 1.0f;
constexpr float kLassoUnderlayWidthPx = 3.0f;
constexpr float kLassoLineWidthPx = 1.0f;

// Hand the dashes to ImGui: one-device-pixel filled rectangles with their ends snapped to whole
// pixels, never antialiased lines. A line centred on a pixel edge spreads over two rows at partial
// alpha, and that half-lit row is the difference between a crisp ants line and a fuzzy edge nobody
// can follow.
// `axis_limit` is the viewport extent along the run's axis: only the visible part of a run is walked,
// with the pattern position interpolated to the visible start so clipping never moves the dashes.
void draw_ants_run(ImDrawList* list, const ImVec2& origin, bool along_x, float line, float axis_from,
                   float axis_to, float s_from, float s_to, float phase, float axis_limit) {
    const float first = std::max(axis_from, 0.0f);
    const float last = std::min(axis_to, axis_limit);
    if (axis_limit <= 0.0f || axis_to <= axis_from || last <= first) {
        return;
    }
    const auto visible_s = [axis_from, axis_to, s_from, s_to](float axis) {
        return s_from + (s_to - s_from) * ((axis - axis_from) / (axis_to - axis_from));
    };
    const float band = std::floor(line);
    for (const AntDash& dash : ant_dashes(first, last, visible_s(first), visible_s(last), phase)) {
        const float a = std::round(dash.from);
        const float b = std::round(dash.to);
        if (b <= a) {
            continue;
        }
        const ImVec2 lo = along_x ? ImVec2(a + origin.x, band + origin.y)
                                  : ImVec2(band + origin.x, a + origin.y);
        const ImVec2 hi = along_x ? ImVec2(b + origin.x, band + 1.0f + origin.y)
                                  : ImVec2(band + 1.0f + origin.x, b + origin.y);
        list->AddRectFilled(lo, hi, dash.light ? kAntLight : kAntDark);
    }
}

// The selection's boundary, in dashes that march. Runs come from the mask, which reports each edge
// once and along its own length, so a rectangle, a painted blob, a selection that fills the canvas
// and a selection with a hole in it are drawn by this one path - and no edge is stroked twice, which
// matters as much as where the dashes fall: an edge drawn twice, once by a run marching one way and
// once by a perpendicular run of single pixels, reads as a line that blinks.
void draw_ants(ImDrawList* list, const AppState& state, const ImVec2& origin, float phase,
               const Mask& selection) {
    const AntRing ring{selection.bounds()};
    const float scale = static_cast<float>(state.view.scale);
    for (const MaskRun& run : selection.boundary()) {
        const bool along_x = run.side == MaskSide::up || run.side == MaskSide::down;
        // The edge itself: coverage ends at `fixed` on the up and left sides and one pixel further on
        // the down and right ones, so the ants sit between the selected pixel and the unselected one
        // rather than through the middle of the selected one.
        const int across = along_x ? state.doc.height() : state.doc.width();
        const double cross = ant_line(run.side, run.fixed, across);
        const Vec2 start = along_x ? Vec2{static_cast<double>(run.from), cross}
                                   : Vec2{cross, static_cast<double>(run.from)};
        const Vec2 end = along_x ? Vec2{static_cast<double>(run.to), cross}
                                 : Vec2{cross, static_cast<double>(run.to)};
        const Vec2 screen_start = state.view.world_to_view(start);
        const Vec2 screen_end = state.view.world_to_view(end);
        draw_ants_run(list, origin, along_x,
                      static_cast<float>(along_x ? screen_start.y : screen_start.x),
                      static_cast<float>(along_x ? screen_start.x : screen_start.y),
                      static_cast<float>(along_x ? screen_end.x : screen_end.y),
                      static_cast<float>(ring.along(run.side, run.from)) * scale,
                      static_cast<float>(ring.along(run.side, run.to)) * scale, phase,
                      static_cast<float>(along_x ? state.viewport.w : state.viewport.h));
    }
}

// The ant overlay for a debug shot: the same pattern at the requested phases, one band beside the
// next, so a single capture shows a whole cycle and whether the pattern agrees across the sides.
void draw_ants_phases(ImDrawList* list, AppState& state, const ImVec2& origin,
                      const std::vector<float>& phases) {
    if (!state.doc.selection() || phases.empty()) {
        return;
    }
    const float stride = static_cast<float>(state.doc.width()) + 4.0f;
    for (std::size_t i = 0; i < phases.size(); ++i) {
        const ImVec2 band{origin.x + static_cast<float>(i) * stride, origin.y};
        draw_ants(list, state, band, phases[i], *state.doc.selection());
    }
}

// The selection's outline, or the frozen bands a probe asked for. The clock is read here and
// published: it is the only wall time an overlay may use, and `app.state` has to say what a frame
// actually drew for a screenshot to be reproducible.
void draw_selection(ImDrawList* list, AppState& state, const ImVec2& origin, CoverageTint& tint) {
    state.ants.time = ImGui::GetTime();
    // Editing the layer mask has its own red preview. Keep the independently retained AI
    // selection available for Cover, but do not draw its blue wash or ants over the mask.
    if (state.painting_mask() || !state.doc.selection()) {
        tint.sync(Mask{});
        return;
    }
    // The wash goes under the ants and over the picture: it is the amount of coverage, and the
    // boundary is drawn on top of it so the two read as one shape rather than as a haze with a line
    // somewhere near it.
    if (state.coverage_view != CoverageView::ants) {
        tint.sync(*state.doc.selection());
        if (tint.drawn && tint.texture.valid()) {
            const Vec2 corner = state.view.world_to_view(
                {static_cast<double>(tint.bounds.x), static_cast<double>(tint.bounds.y)});
            const Vec2 far = state.view.world_to_view(
                {static_cast<double>(tint.bounds.right()), static_cast<double>(tint.bounds.bottom())});
            list->AddImage(simgui_imtextureid(tint.texture.view),
                           ImVec2(origin.x + static_cast<float>(corner.x),
                                  origin.y + static_cast<float>(corner.y)),
                           ImVec2(origin.x + static_cast<float>(far.x),
                                  origin.y + static_cast<float>(far.y)));
        }
    } else {
        tint.sync(Mask{});
    }
    if (state.coverage_view == CoverageView::overlay) {
        return;
    }
    if (state.ants.phases.empty()) {
        draw_ants(list, state, origin, ant_drawn_phase(state.ants), *state.doc.selection());
        return;
    }
    draw_ants_phases(list, state, origin, state.ants.phases);
}

// The dab's footprint where the paint will land: a light ring at the rim, a dark one inside the
// hardness falloff. The rim is what a person is aiming with, so it is drawn to the screen pixel rather
// than filled; the core ring says how much of it is full strength.
void draw_footprint(ImDrawList* list, const ImVec2& at, double footprint_px, double core_px) {
    const float radius = static_cast<float>(footprint_px) * 0.5f;
    list->AddCircle(at, radius, kFootprint, 0, 1.0f);
    if (radius > kFootprintOutlinePx) {
        list->AddCircle(at, radius - kFootprintOutlinePx, kAntDark, 0, 1.0f);
    }
    if (core_px + 1.0 < footprint_px) {
        list->AddCircle(at, static_cast<float>(core_px) * 0.5f, kAntDark, 0, 1.0f);
    }
}

}

namespace {

void draw_checker(ImDrawList* list, const AppState& state, const gfx::Checker& checker,
                  const ImVec2& canvas_min, const ImVec2& canvas_max, const ImVec2& origin,
                  const ImVec2& avail) {
    const ImVec2 check_min(std::max(canvas_min.x, origin.x), std::max(canvas_min.y, origin.y));
    const ImVec2 check_max(std::min(canvas_max.x, origin.x + avail.x),
                           std::min(canvas_max.y, origin.y + avail.y));
    if (!checker.valid() || check_max.x <= check_min.x || check_max.y <= check_min.y) {
        return;
    }
    const gfx::CheckerUv uv = gfx::checker_uv(
        {check_min.x, check_min.y}, {check_max.x - check_min.x, check_max.y - check_min.y},
        {canvas_min.x, canvas_min.y}, state.view.scale);
    list->AddImage(simgui_imtextureid_with_sampler(checker.tile, checker.sampler), check_min,
                   check_max, ImVec2(uv.min_x, uv.min_y), ImVec2(uv.max_x, uv.max_y));
}

ImVec2 image_screen_min(const AppState& state, const ImVec2& origin) {
    const Vec2 image = state.view.world_to_view({0.0, 0.0});
    return ImVec2(origin.x + static_cast<float>(image.x), origin.y + static_cast<float>(image.y));
}

void draw_marquee(ImDrawList* list, const AppState& state, const ImVec2& origin) {
    if (!state.select.active || state.select.gesture == SelectionGesture::lasso ||
        state.select.rect.empty()) {
        return;
    }
    const Vec2 corner = state.view.world_to_view(
        {static_cast<double>(state.select.rect.x), static_cast<double>(state.select.rect.y)});
    const Vec2 far = state.view.world_to_view(
        {static_cast<double>(state.select.rect.right()),
         static_cast<double>(state.select.rect.bottom())});
    list->AddRect(ImVec2(static_cast<float>(corner.x) + origin.x,
                         static_cast<float>(corner.y) + origin.y),
                  ImVec2(static_cast<float>(far.x) + origin.x,
                         static_cast<float>(far.y) + origin.y),
                  kAntLight, 0.0f, 0, 1.0f);
}

void draw_lasso_path(ImDrawList* list, const AppState& state, const ImVec2& origin) {
    if (!state.select.active || state.select.gesture != SelectionGesture::lasso ||
        state.select.path.size() < 2u) {
        return;
    }
    std::vector<ImVec2> points;
    points.reserve(state.select.path.size());
    for (const Vec2 point : state.select.path) {
        const Vec2 screen = state.view.world_to_view(point);
        points.emplace_back(origin.x + static_cast<float>(screen.x),
                            origin.y + static_cast<float>(screen.y));
    }
    list->AddPolyline(points.data(), static_cast<int>(points.size()), kLassoUnderlay,
                      ImDrawFlags_None, kLassoUnderlayWidthPx);
    list->AddPolyline(points.data(), static_cast<int>(points.size()), kLassoLine,
                      ImDrawFlags_None, kLassoLineWidthPx);
}

void draw_document(ImDrawList* list, AppState& state, gfx::Texture& canvas_texture,
                   const gfx::Checker& checker, CoverageTint& tint, CoverageTint& mask_tint,
                   const ImVec2& origin, const ImVec2& avail, const ImGuiIO& io) {
    const ImVec2 canvas_min = image_screen_min(state, origin);
    const ImVec2 canvas_max(canvas_min.x + static_cast<float>(state.view.image_width()),
                            canvas_min.y + static_cast<float>(state.view.image_height()));
    draw_checker(list, state, checker, canvas_min, canvas_max, origin, avail);
    canvas_texture.sync(state.doc.composite(), state.doc.revision());
    if (canvas_texture.valid()) {
        list->AddImage(simgui_imtextureid(canvas_texture.view), canvas_min, canvas_max);
    }
    const ui_theme::Palette& colors = ui_theme::palette(state.ui_theme);
    list->AddRect(canvas_min, canvas_max,
                  ImGui::ColorConvertFloat4ToU32(colors.canvas_border));
    if (state.painting_mask() && state.mask_overlay_visible) {
        const Layer& layer = state.active_layer();
        mask_tint.sync_hidden(*layer.mask, layer.id, layer.content_revision);
        const Rect box = mask_tint.bounds;
        const auto screen = [&](double x, double y) {
            const Vec2 point = state.view.world_to_view({x, y});
            return ImVec2(origin.x + static_cast<float>(point.x),
                          origin.y + static_cast<float>(point.y));
        };
        const auto wash = [&](int x0, int y0, int x1, int y1) {
            if (x0 < x1 && y0 < y1) {
                list->AddRectFilled(screen(x0, y0), screen(x1, y1),
                                    IM_COL32(235, 65, 65, 110));
            }
        };
        wash(0, 0, state.doc.width(), box.y);
        wash(0, box.bottom(), state.doc.width(), state.doc.height());
        wash(0, box.y, box.x, box.bottom());
        wash(box.right(), box.y, state.doc.width(), box.bottom());
        if (mask_tint.drawn && mask_tint.texture.valid()) {
            list->AddImage(simgui_imtextureid(mask_tint.texture.view),
                           screen(box.x, box.y), screen(box.right(), box.bottom()));
        }
    }
    draw_selection(list, state, origin, tint);
    if (state.pointer.drawn) {
        draw_footprint(list, ImVec2(io.MousePos.x, io.MousePos.y),
                       state.pointer.footprint_px, state.pointer.core_px);
    }
    draw_marquee(list, state, origin);
    draw_lasso_path(list, state, origin);
}

bool update_canvas_pointer(AppState& state, const ImVec2& origin, const ImGuiIO& io) {
    const bool hovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
    const std::optional<Vec2> over_canvas =
        (hovered && !state.doc.empty())
            ? std::optional<Vec2>(state.view.view_to_world(
                  {io.MousePos.x - origin.x, io.MousePos.y - origin.y}))
            : std::nullopt;
    state.pointer = resolve_pointer(state, over_canvas, state.footprint_forced);
    return hovered;
}

void handle_canvas_input(AppState& state, CommandTable& commands, const ImVec2& origin,
                         const ImGuiIO& io, bool hovered) {
    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        const Vec2 at = state.view.view_to_world(
            {io.MousePos.x - origin.x, io.MousePos.y - origin.y});
        json args{{"phase", "press"}, {"at", json::array({at.x, at.y})}};
        if (tool_selection_gesture(state.tool) == SelectionGesture::lasso) {
            if (io.KeyShift && !io.KeyAlt) {
                args["mode"] = "add";
            } else if (io.KeyAlt && !io.KeyShift) {
                args["mode"] = "subtract";
            }
        }
        commands.invoke("tool.pointer", args);
    } else if ((hovered || state.gesturing()) && ImGui::IsMouseDown(ImGuiMouseButton_Left) &&
               (io.MouseDelta.x != 0.0f || io.MouseDelta.y != 0.0f)) {
        const Vec2 at = state.view.view_to_world(
            {io.MousePos.x - origin.x, io.MousePos.y - origin.y});
        commands.invoke("tool.pointer",
                        json{{"phase", "drag"}, {"at", json::array({at.x, at.y})}});
    }
    if (state.gesturing() && ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
        json args{{"phase", "release"}};
        if (state.select.active) {
            const Vec2 at = state.view.view_to_world(
                {io.MousePos.x - origin.x, io.MousePos.y - origin.y});
            args["at"] = json::array({at.x, at.y});
        }
        commands.invoke("tool.pointer", args);
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

}

void draw_canvas(AppState& state, CommandTable& commands, gfx::Texture& canvas_texture,
                 const gfx::Checker& checker, CoverageTint& tint, CoverageTint& mask_tint) {
    if (!ImGui::Begin(kWindowCanvas)) {
        ImGui::End();
        return;
    }
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    state.viewport.w = std::max(1, static_cast<int>(avail.x));
    state.viewport.h = std::max(1, static_cast<int>(avail.y));
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    state.canvas_origin = {origin.x, origin.y};
    ImDrawList* list = ImGui::GetWindowDrawList();
    const ui_theme::Palette& colors = ui_theme::palette(state.ui_theme);
    list->AddRectFilled(origin, ImVec2(origin.x + avail.x, origin.y + avail.y),
                        ImGui::ColorConvertFloat4ToU32(colors.canvas_backdrop));
    ImGuiIO& io = ImGui::GetIO();
    const bool hovered = update_canvas_pointer(state, origin, io);
    if (!state.doc.empty()) {
        draw_document(list, state, canvas_texture, checker, tint, mask_tint, origin, avail, io);
        handle_canvas_input(state, commands, origin, io, hovered);
    }
    ImGui::End();
}

}
