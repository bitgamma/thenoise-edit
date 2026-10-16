// SPDX-License-Identifier: Apache-2.0
#include "app/ui.h"

#include "app/ants.h"
#include "app/coverage_tint.h"
#include "app/layer_thumbs.h"
#include "app/brush_steps.h"
#include "app/command_args.h"
#include "app/verb_bar.h"
#include "app/verb_fields.h"

#include "core/log.h"
#include "gfx/checker.h"

#include "imgui.h"
#include "imgui_internal.h"  // DockBuilder*: the default dock layout has no public equivalent yet
#include "sokol_app.h"
#include "sokol_imgui.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace noise::app {

using nlohmann::json;

// Defined below, at this scope: the front door and the file row are drawn from the panels as well
// as from `draw_ui`, so they are not anonymous.
void draw_file_row(AppState& state, CommandTable& commands);

namespace {

constexpr ImU32 kBackdrop = IM_COL32(38, 38, 42, 255);
constexpr ImU32 kCanvasBorder = IM_COL32(90, 90, 100, 255);
constexpr float kRightPanelFraction = 0.30f;
// Two lines: the document's own facts, and beneath them the AI line rule F asks for. A bar with one
// line gives the AI line about a third of its width after the zoom, size, path and undo counts, and a
// clipped warning about a model swap is worse than no warning.
constexpr float kStatusBarHeightPx = 86.0f;
// The Tool panel holds the verbs as well as the tool rows, and the generate row carries an aspect and a
// style dropdown under its words: measured against a 700 px tall window, where 0.52 clipped the verbs.
constexpr float kToolPanelFraction = 0.64f;
// A layer row is eye + name + opacity on one line and a blend-mode dropdown on the next. The
// slider gets a fixed slice so a long name clips instead of pushing the control off the panel, and
// the name keeps a slice of its own so a narrow panel still shows which layer is which. The mode
// dropdown is the width of the name slice, so the two lines read as one record.
constexpr float kLayerOpacitySliderPx = 88.0f;
// Each layer is one bordered card, because a checkbox, a slider and a dropdown that float in a list
// belong to whichever name they happen to sit near. The active card's border is the accent so which
// layer the controls answer to is a fact about the frame, not about proximity.
constexpr ImU32 kCardBorder = IM_COL32(70, 70, 82, 255);
constexpr ImU32 kCardActiveBorder = IM_COL32(120, 170, 255, 255);
constexpr ImU32 kCardBg = IM_COL32(32, 32, 38, 255);
constexpr ImU32 kCardActiveBg = IM_COL32(42, 46, 60, 255);
constexpr ImU32 kCardShadow = IM_COL32(0, 0, 0, 0);
constexpr ImU32 kAntLight = IM_COL32(255, 255, 255, 255);
constexpr ImU32 kAntDark = IM_COL32(20, 20, 20, 255);
// The footprint ring has to read on a black pixel and on a white one, so it is drawn twice. The dark
// circle goes *inside* the light one, because the rim is what a person aims with and the overlay's
// outer edge has to be that rim rather than a pixel past it.
constexpr ImU32 kFootprint = IM_COL32(255, 255, 255, 235);
constexpr float kFootprintOutlinePx = 1.0f;
constexpr float kLayerNameMinPx = 40.0f;
// What the swap confirmation is about, remembered between the frame that opened the popup and the one
// whose button answers it: the verb row is gone by then, and a modal that re-read the table would ask
// about whatever the table said next rather than what was pressed.
std::string pending_command;
json pending_args;
std::string pending_needs;
std::string pending_resident;
const ImVec4 kWarnInk{0.92f, 0.69f, 0.27f, 1.0f};
const ImVec4 kStopInk{0.91f, 0.38f, 0.33f, 1.0f};

// Docked by name below and opened by the same constants here.
constexpr const char* kWindowCanvas = "Canvas";
constexpr const char* kWindowTool = "Brush";
constexpr const char* kWindowLayers = "Layers";
constexpr const char* kWindowLog = "Log";
constexpr const char* kWindowStatus = "Status";

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
    if (!state.doc.selection()) {
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

void draw_canvas(AppState& state, CommandTable& commands, gfx::Texture& canvas_texture,
                 const gfx::Checker& checker, CoverageTint& tint) {
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

    // The overlay reads the pointer the host reported, so a driver that moves the real pointer and a
    // hand doing the same thing land on one code path. Decided every frame, including the frames with no
    // document: a stale `over_canvas` here leaves the OS cursor hidden over a window with nothing in it.
    const std::optional<Vec2> over_canvas =
        (hovered && !state.doc.empty())
            ? std::optional<Vec2>(state.view.view_to_world(
                  {io.MousePos.x - origin.x, io.MousePos.y - origin.y}))
            : std::nullopt;
    state.pointer = resolve_pointer(state, over_canvas, state.footprint_forced);

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

        canvas_texture.sync(state.doc.composite(), state.doc.revision());
        if (canvas_texture.valid()) {
            list->AddImage(simgui_imtextureid(canvas_texture.view), canvas_min, canvas_max);
        }
        list->AddRect(canvas_min, canvas_max, kCanvasBorder);
        draw_selection(list, state, origin, tint);
        if (state.pointer.drawn) {
            draw_footprint(list, ImVec2(io.MousePos.x, io.MousePos.y),
                           state.pointer.footprint_px, state.pointer.core_px);
        }
        if (state.select.active && !state.select.rect.empty()) {
            // The marquee is the selection during the gesture, and drawing it on top is what tells
            // the user the second corner follows the pointer rather than the first.
            const Vec2 corner = state.view.world_to_view(
                {static_cast<double>(state.select.rect.x),
                 static_cast<double>(state.select.rect.y)});
            const Vec2 far = state.view.world_to_view(
                {static_cast<double>(state.select.rect.right()),
                 static_cast<double>(state.select.rect.bottom())});
            list->AddRect(ImVec2(static_cast<float>(corner.x) + origin.x,
                                 static_cast<float>(corner.y) + origin.y),
                          ImVec2(static_cast<float>(far.x) + origin.x,
                                 static_cast<float>(far.y) + origin.y),
                          kAntLight, 0.0f, 0, 1.0f);
        }

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
        // `gesturing()` rather than `stroke.active`: the release belongs to whichever tool has the
        // button, and gating it on the paint gesture left a move gesture running with the button
        // still held - invisible to a command-level test, because `tool.pointer` is driven directly
        // there and reaches `move_release` either way.
        if (state.gesturing() && ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
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

void draw_verb_section(CommandTable& commands, const json& intent);
void draw_ai_status_line(AppState& state, CommandTable& commands, const json& view);

// Every widget here is a front-end for a command: the panel, a key and a wire client cannot
// drift apart because there is nothing else to keep in step.
void draw_tool_panel(AppState& state, CommandTable& commands, const json& intent) {
    ImGui::Begin(kWindowTool);

    draw_file_row(state, commands);
    ImGui::Separator();

    for (const ToolInfo& tool : kTools) {
        const std::string label = std::string(tool.id) + "###" + tool.id;
        if (ImGui::Selectable(label.c_str(), state.tool == tool.id)) {
            commands.invoke(std::string("tool.") + tool.id, json::object());
        }
        ImGui::SameLine();
        ImGui::TextDisabled("[%s]", tool.select_key);
    }

    // The selection, because the tool above is the one that draws it. Every control here is a
    // `select.*` command, so the keyboard, the wire and a click cannot disagree about what
    // deselecting means.
    // Coverage is one thing with two lifetimes, so there is one editor and one switch: the brush, the
    // marquee and the maths write whichever coverage this names, and what differs is only whether it is
    // put down at the end or saved with the layer. The mask half is dimmed where the layer has none, so
    // the reason a stroke would do nothing is on the screen rather than in a canvas that refused to move.
    ImGui::SeparatorText("coverage");
    const bool has_mask = !state.doc.empty() && state.active_layer().mask.has_value();
    if (ImGui::RadioButton("marked area", state.coverage_target == CoverageTarget::marked)) {
        commands.invoke("coverage.target", json{{"to", "marked"}});
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(!has_mask);
    if (ImGui::RadioButton("layer mask", state.coverage_target == CoverageTarget::mask)) {
        commands.invoke("coverage.target", json{{"to", "mask"}});
    }
    ImGui::EndDisabled();

    const bool at_mask = state.coverage_target == CoverageTarget::mask;
    // The measure of the coverage lives in the status line, not here: the panel would be a second
    // place to keep the same numbers, and every line it spends is a line the verbs below do not get.
    const json selection = selection_json(state);
    if (at_mask && commands.invoke("mask.get", json::object()).value["mask"].is_null()) {
        ImGui::TextDisabled("no mask on this layer yet - Add mask in the Layers panel");
    }
    // Three operations, each run against whichever coverage is named: the marked area has its commands
    // and the mask has its own, and the panel holds no third version of either.
    if (ImGui::Button("All")) {
        commands.invoke(at_mask ? "mask.fill" : "select.all",
                        at_mask ? json{{"to", "show"}} : json::object());
    }
    ImGui::SameLine();
    if (ImGui::Button("None")) {
        commands.invoke(at_mask ? "mask.fill" : "select.clear",
                        at_mask ? json{{"to", "hide"}} : json::object());
    }
    ImGui::SameLine();
    if (ImGui::Button("Invert")) {
        commands.invoke(at_mask ? "mask.invert" : "select.invert", json::object());
    }

    // The two directions between the selection and the active layer's mask. They are here rather
    // than in the Layers panel because the thing being acted on is the selection. Guarded, because
    // the app starts with no document at all and the panel is drawn anyway - which is how a
    // `layer(active)` on an empty stack crashes the frame rather than the command.
    if (!state.doc.empty()) {
        const Layer& active = state.doc.layer(static_cast<std::size_t>(state.doc.active()));
        ImGui::BeginDisabled(!selection["active"].get<bool>());
        if (ImGui::Button("To layer mask")) {
            commands.invoke("mask.from_selection", json::object());
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(!active.mask.has_value());
        if (ImGui::Button("From layer mask")) {
            commands.invoke("select.from_mask", json::object());
        }
        ImGui::EndDisabled();
        if (active.mask) {
            ImGui::TextDisabled("mask on '%s': %d px shown", active.name.c_str(),
                                static_cast<int>(active.mask->covered()));
        }
    }

    draw_verb_section(commands, intent);

    // The brush belongs to the tool a person is holding: `move` and the marquees paint nothing, so
    // their panel does not spend its height on sliders they cannot use (core/brush.h knows which rows
    // paint - this does not decide it again).
    if (!tool_mode(state.tool).has_value()) {
        ImGui::End();
        return;
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
        // The keys that step this field, asked of the command table rather than typed here: a hint
        // that drifted from the binding would teach the wrong key. Fields with no step rows - flow
        // and spacing - get no hint.
        std::string hint;
        for (const BrushStep& step : kBrushSteps) {
            if (std::string_view(step.field) != slider.label) {
                continue;
            }
            for (const std::string& binding : commands.shortcuts_for(step.id)) {
                // One binding per line: the panel is docked against the right edge, so a tooltip
                // laid out horizontally runs off the screen whatever it says.
                if (!hint.empty()) {
                    hint += "\n";
                }
                hint += binding;
            }
        }
        if (ImGui::SliderFloat(slider.label, &value, 0.0f, 1.0f, "%.2f") &&
            static_cast<double>(value) != state.brush.*(slider.member)) {
            commands.invoke("tool.set", json{{slider.label, static_cast<double>(value)}});
        }
        // On hover rather than beside the slider, and on two lines: the binding list is wider than
        // the slider in a panel a third of the window, and a tooltip that runs off the screen edge
        // says nothing either. `command.list` is the machine-readable form of the same fact.
        if (!hint.empty() && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
            ImGui::SetTooltip("step keys\n%s", hint.c_str());
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

// The verbs, read from the table the commands are generated from and pressed as those commands: a row
// here cannot mean something other than `ai.<verb>`, because it asks the command table for the name and
// the key.
//
// The words a verb asks for are typed into that verb's own box and committed through `ai.intent`, which
// is also the door the row's press and the key go through - so the text on screen, the button, the
// shortcut and the wire cannot describe four different runs. Committing selects the row's recipe as
// well as filling its field, which is what makes the price on the status line the price of the press
// that would follow the text. The panel keeps no copy of the words of its own: the app's intent holds
// the words for one recipe at a time (Rule E prunes the rest when the intent moves), so a row whose
// recipe is not the intent's shows an empty box - which is the truth, and what asking means.
void draw_verb_section(CommandTable& commands, const json& intent) {
    ImGui::SeparatorText("verbs");
    const json view = commands.invoke("ai.verbs", json::object()).value;
    const std::string table_error = view["error"].get<std::string>();
    if (!table_error.empty()) {
        ImGui::TextColored(kStopInk, "%s", table_error.c_str());
        return;
    }

    const std::string intended =
        intent["recipe"].is_null() ? std::string() : intent["recipe"].get<std::string>();
    // The row whose box currently owns the keyboard: only that box may be left as ImGui last wrote it.
    static std::string editing;
    std::map<std::string, std::array<char, kVerbFieldChars>>& boxes = verb_field_boxes();

    for (const json& verb : view["verbs"]) {
        const std::string id = verb["id"].get<std::string>();
        if (verb["scope"].get<std::string>() == "selection") {
            // These live on the canvas, beside the mark they act on, words and all. A second copy of
            // each button on the far side of the window is a second thing to remember and a second
            // place for the same refusal to appear.
            continue;
        }
        const std::string label = verb["label"].is_null() ? id : verb["label"].get<std::string>();
        const std::string reason = verb["reason"].get<std::string>();
        const std::string recipe = verb["recipe"].get<std::string>();
        const json ask = verb["ask"];
        const std::string field =
            ask.is_array() && !ask.empty() ? ask[0].get<std::string>() : std::string();

        ImGui::PushID(id.c_str());
        ImGui::BeginDisabled(!verb["ready"].get<bool>());
        const bool pressed = ImGui::Button(label.c_str());
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::TextDisabled("[%s]", verb["key"].get<std::string>().c_str());
        if (!verb["ready"].get<bool>() && !reason.empty() &&
            ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
            ImGui::SetTooltip("%s", reason.c_str());
        }

        if (!field.empty()) {
            std::array<char, kVerbFieldChars>& box = boxes[id];
            if (editing != id) {
                const std::string shown =
                    recipe == intended ? intent["vars"].value(field, std::string()) : std::string();
                std::snprintf(box.data(), box.size(), "%s", shown.c_str());
            }
            ImGui::TextDisabled("%s", field.c_str());
            ImGui::SetNextItemWidth(-1.0f);
            // The value goes in as typed, blanks included: clearing the box clears the words, and the
            // press then refuses naming the field rather than running on the recipe's own guess.
            if (ImGui::InputText("##words", box.data(), box.size() - 1)) {
                commands.invoke("ai.intent",
                                json{{"recipe", recipe}, {"vars", json{{field, box.data()}}}});
            }
            editing = ImGui::IsItemActive() ? id : std::string();
        }

        // Both are rows of the recipe table reaching the panel through `ai.verbs` - this file holds no
        // list of either - and both commit through `ai.intent`, the door the key reads. A row that offers
        // nothing draws nothing.
        const json aspects = verb["aspects"];
        if (aspects.is_array() && !aspects.empty()) {
            const std::string chosen =
                intent["aspect"].is_null() ? std::string() : intent["aspect"].get<std::string>();
            ImGui::TextDisabled("aspect");
            ImGui::SetNextItemWidth(-1.0f);
            // "canvas" is not an aspect: it is the absence of one, and it is listed first because putting
            // the pixel budget back is a thing a person has to be able to do.
            if (ImGui::BeginCombo("##aspect", chosen.empty() ? "canvas" : chosen.c_str())) {
                if (ImGui::Selectable("canvas", chosen.empty())) {
                    commands.invoke("ai.intent", json{{"recipe", recipe}, {"aspect", nullptr}});
                }
                for (const json& offered : aspects) {
                    const std::string name = offered.get<std::string>();
                    const bool on = name == chosen;
                    if (ImGui::Selectable(name.c_str(), on)) {
                        commands.invoke(
                            "ai.intent", json{{"recipe", recipe}, {"aspect", name}});
                    }
                    if (on) {
                        ImGui::SetItemDefaultFocus();
                    }
                }
                ImGui::EndCombo();
            }
        }

        const json variants = verb["variants"];
        if (variants.is_array() && !variants.empty()) {
            const std::string active =
                verb["active"].is_null() ? recipe : verb["active"].get<std::string>();
            std::string shown = label;
            for (const json& member : variants) {
                if (member["id"].get<std::string>() == active) {
                    shown = member["label"].get<std::string>();
                }
            }
            ImGui::TextDisabled("style");
            ImGui::SetNextItemWidth(-1.0f);
            if (ImGui::BeginCombo("##style", shown.c_str())) {
                // Selecting a style selects its recipe on the intent; it does not run it. The head of the
                // family is the row's own recipe, so "back to Generate" is a row of the list rather than a
                // hidden default.
                if (ImGui::Selectable(label.c_str(), active == recipe)) {
                    commands.invoke("ai.intent", json{{"recipe", recipe}});
                }
                for (const json& member : variants) {
                    const std::string id_of_member = member["id"].get<std::string>();
                    const std::string label_of_member = member["label"].get<std::string>();
                    const bool on = id_of_member == active;
                    if (ImGui::Selectable(label_of_member.c_str(), on)) {
                        commands.invoke("ai.intent", json{{"recipe", id_of_member}});
                    }
                    if (on) {
                        ImGui::SetItemDefaultFocus();
                    }
                }
                ImGui::EndCombo();
            }
        }

        // No `vars` on the press: the commit above already put the words where the command reads them,
        // and `Ctrl+Shift+F` - which carries nothing - has to mean the same run as this button.
        if (pressed) {
            if (verb["swap"].get<bool>()) {
                pending_command = verb["command"].get<std::string>();
                pending_args = json::object();
                pending_needs = verb["model"].get<std::string>();
                pending_resident = verb["resident"].get<std::string>();
                ImGui::OpenPopup("Switch model?");
            } else {
                commands.invoke(verb["command"].get<std::string>(), json::object());
            }
        }
        ImGui::PopID();
    }

    if (ImGui::BeginPopupModal("Switch model?", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("That verb needs %s; the server is running %s.", pending_needs.c_str(),
                    pending_resident.c_str());
        ImGui::TextDisabled("Loading it costs tens of seconds on a shared box.");
        if (ImGui::Button("Run anyway")) {
            pending_args["confirm"] = true;
            commands.invoke(pending_command, pending_args);
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

// Rule F: no AI tab, but never an invisible AI. What the next press will do is stated in the bar that
// is on screen whatever panel is docked in front - which model answers, what it costs on this box,
// whether it has to load one first, and the job that is running or waiting to land. Every fact comes
// from `ai.intent`, so the bar cannot quote a price the command would not charge.
void draw_ai_status_line(AppState& state, CommandTable& commands, const json& view) {
    if (!view["configured"].get<bool>()) {
        ImGui::TextDisabled("ai: no server, set $THENOISE_URL");
        return;
    }
    if (!view["connected"].get<bool>()) {
        ImGui::TextColored(kStopInk, "ai: offline");
        ImGui::SameLine();
        if (ImGui::SmallButton("Connect")) {
            commands.invoke("ai.intent", json{{"connect", true}});
        }
        return;
    }
    const std::string model = view["model"].get<std::string>();
    if (view["label"].is_null() || model.empty()) {
        ImGui::TextDisabled("ai: %s", view["reason"].get<std::string>().c_str());
        return;
    }
    // The verb the intent holds names the line: the price on screen belongs to the key a person would
    // press next, not to an operation they never chose.
    ImGui::Text("ai: %s", view["label"].get<std::string>().c_str());
    ImGui::SameLine();
    ImGui::TextDisabled("%s", model.c_str());
    ImGui::SameLine();
    if (view["estimate_s"].is_number()) {
        ImGui::SameLine();
        ImGui::TextDisabled("~%.1f s", view["estimate_s"].get<double>());
    }
    if (view["quote"].is_object()) {
        // The size and the endpoint the press would ask for; the endpoint is in `ai.intent`'s quote for
        // anyone who wants it, and the bar has to leave room for where an answer went.
        const json& quote = view["quote"];
        ImGui::SameLine();
        ImGui::TextDisabled("%dx%d", quote["size"][0].get<int>(), quote["size"][1].get<int>());
    }
    // The seed is on screen because it is in the request; nothing draws a number behind the person's
    // back, and `ai.intent {seed: n}` is how it changes.
    ImGui::SameLine();
    ImGui::TextDisabled("seed %llu", static_cast<unsigned long long>(view["seed"].get<std::uint64_t>()));
    if (view["swap"].get<bool>()) {
        ImGui::SameLine();
        ImGui::TextColored(kWarnInk, "will load the model (resident: %s)",
                           view["resident"].get<std::string>().c_str());
    }
    if (view["job"].is_object()) {
        const json& job = view["job"];
        const std::string job_state = job.contains("state") ? job["state"].get<std::string>() : "?";
        ImGui::SameLine();
        // While it works, the row says what was asked and for how long, because that is the sentence a
        // person is waiting to hear; the id comes back for a finished job, where it is what a log line
        // and a bug report have in common.
        if (job_state == "queued" || job_state == "running") {
            const double started = job.value("started_at", 0.0);
            if (started > 0.0) {
                ImGui::TextDisabled("working: %s  %d s",
                                    job.value("recipe", std::string()).c_str(),
                                    static_cast<int>(state.now - started));
            } else {
                ImGui::TextDisabled("working: %s", job.value("recipe", std::string()).c_str());
            }
        } else {
            ImGui::TextDisabled("%s %s",
                                job.contains("id") ? job["id"].get<std::string>().c_str() : "",
                                job_state.c_str());
        }
        if (job_state == "succeeded") {
            // No Apply button: the answer is already on the document by the time the job says so, on a
            // layer named for the verb and the job, and `Ctrl+Z` takes the layer and its pixels together.
            ImGui::SameLine();
            const json& landing = job.contains("landing") ? job["landing"] : json::object();
            const std::string landed_layer = landing.contains("landed_layer")
                                                 ? landing["landed_layer"].get<std::string>()
                                                 : std::string();
            if (landed_layer.empty()) {
                // One frame at most: the beat lands a finished answer at the first read after it.
                ImGui::TextDisabled("landing...");
            } else {
                ImGui::TextDisabled("on '%s', Ctrl+Z", landed_layer.c_str());
            }
        } else if (job_state == "queued" || job_state == "running") {
            // The one stop that means something while the box is working. An answer that arrived anyway
            // is kept, which is `job.cancel`'s promise rather than a second opinion here.
            ImGui::SameLine();
            if (ImGui::SmallButton("Cancel")) {
                commands.invoke("job.cancel", json{{"id", job["id"]}});
            }
        } else if (job_state == "failed" || job_state == "cancelled") {
            ImGui::SameLine();
            ImGui::TextColored(kStopInk, "%s",
                               job.contains("failure") ? job["failure"].get<std::string>().c_str()
                                                       : "");
        }
    }
    const std::string error = view["error"].get<std::string>();
    if (!error.empty()) {
        ImGui::SameLine();
        ImGui::TextColored(kStopInk, "%s", error.c_str());
    }
}

// The box a layer's picture is drawn into: the long edge is the thumbnail size, the other follows the
// layer's own proportions, so a wide document's card does not show its picture stretched.
ImVec2 thumb_draw_size(Extent size) {
    const int long_edge = std::max(1, std::max(size.w, size.h));
    const float scale = static_cast<float>(kLayerThumbLongEdge) / static_cast<float>(long_edge);
    return ImVec2(std::max(1.0f, static_cast<float>(size.w) * scale),
                  std::max(1.0f, static_cast<float>(size.h) * scale));
}

void draw_layers_panel(AppState& state, CommandTable& commands, LayerThumbs& thumbs) {
    ImGui::Begin(kWindowLayers);
    state.layer_cards.clear();
    const ImVec2 panel_pos = ImGui::GetWindowPos();
    const ImVec2 panel_size = ImGui::GetWindowSize();
    state.layers_window = ScreenRect{panel_pos.x, panel_pos.y, panel_size.x, panel_size.y};
    if (state.doc.empty()) {
        ImGui::TextUnformatted("no document");
    } else {
        // Top layer first, which is how every layer list shows them; the document is bottom-first, so
        // the rows run against the indices.
        const std::span<const Layer> layers = state.doc.layers();
        const bool live = state.doc.in_provisional();
        thumbs.resize(layers.size());
        const float card_px = ImGui::GetContentRegionAvail().x;
        const float name_px = std::max(
            kLayerNameMinPx,
            card_px - (ImGui::GetFrameHeight() + ImGui::GetStyle().ItemSpacing.x * 2.0f));
        for (std::size_t row = layers.size(); row-- > 0;) {
            const Layer& layer = layers[row];
            thumbs.sync(row, layer, live);
            const bool active = static_cast<int>(row) == state.doc.active();
            ImGui::PushID(static_cast<int>(row));
            ImGui::PushStyleColor(ImGuiCol_ChildBg, active ? kCardActiveBg : kCardBg);
            ImGui::PushStyleColor(ImGuiCol_Border, active ? kCardActiveBorder : kCardBorder);
            ImGui::PushStyleColor(ImGuiCol_BorderShadow, kCardShadow);
            LayerCard card_state;
            card_state.index = static_cast<int>(row);
            card_state.name = layer.name;
            card_state.active = active;
            card_state.visible = layer.visible;
            if (ImGui::BeginChild("card", ImVec2(0.0f, 0.0f),
                                  ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY)) {
                bool visible = layer.visible;
                if (ImGui::Checkbox("##visible", &visible)) {
                    commands.invoke("layer.set", json{{"index", static_cast<int>(row)},
                                                      {"visible", visible}});
                }
                const ImVec2 box_toggle_min = ImGui::GetItemRectMin();
                const ImVec2 box_toggle_max = ImGui::GetItemRectMax();
                card_state.visibility = ScreenRect{box_toggle_min.x, box_toggle_min.y,
                                                   box_toggle_max.x - box_toggle_min.x,
                                                   box_toggle_max.y - box_toggle_min.y};
                ImGui::SameLine();
                if (ImGui::Selectable(layer.name.c_str(), active, 0, ImVec2(name_px, 0.0f))) {
                    commands.invoke("layer.select", json{{"index", static_cast<int>(row)}});
                }
                // The layer's own pictures: what it holds, and the coverage that hides part of it. Both
                // are inside this border because both belong to the name above them.
                const LayerThumbs::Card& card = thumbs.card(row);
                if (card.pixels.valid()) {
                    ImGui::Image(simgui_imtextureid(card.pixels.view),
                                 thumb_draw_size(layer.pixels.size()));
                    const ImVec2 picture_min = ImGui::GetItemRectMin();
                    const ImVec2 picture_max = ImGui::GetItemRectMax();
                    card_state.picture = ScreenRect{picture_min.x, picture_min.y,
                                                    picture_max.x - picture_min.x,
                                                    picture_max.y - picture_min.y};
                }
                if (card.has_mask && card.mask.valid()) {
                    ImGui::SameLine();
                    ImGui::Image(simgui_imtextureid(card.mask.view),
                                 thumb_draw_size(layer.mask->size()));
                    const ImVec2 mask_min = ImGui::GetItemRectMin();
                    const ImVec2 mask_max = ImGui::GetItemRectMax();
                    card_state.mask_picture = ScreenRect{mask_min.x, mask_min.y,
                                                         mask_max.x - mask_min.x,
                                                         mask_max.y - mask_min.y};
                } else if (card.pixels.valid()) {
                    ImGui::SameLine();
                    ImGui::TextDisabled("no mask");
                }
                // The two facts the composite reads and the layer does not hold: how much of it shows,
                // and how it mixes with what is under it.
                float opacity = static_cast<float>(layer.opacity);
                ImGui::PushItemWidth(kLayerOpacitySliderPx);
                if (ImGui::SliderFloat("##opacity", &opacity, 0.0f, 1.0f, "%.2f") &&
                    static_cast<double>(opacity) != layer.opacity) {
                    commands.invoke("layer.set",
                                    json{{"index", static_cast<int>(row)},
                                         {"opacity", static_cast<double>(opacity)}});
                }
                ImGui::PopItemWidth();
                ImGui::SameLine();
                // The items are `kBlendModes` in table order, so a mode added in core/blend appears
                // here without this file being told about it.
                const std::string_view mode_name = blend_mode_name(layer.mode);
                ImGui::PushItemWidth(std::max(
                    64.0f, card_px - kLayerOpacitySliderPx - ImGui::GetStyle().ItemSpacing.x * 3.0f));
                if (ImGui::BeginCombo("##mode",
                                      mode_name.empty() ? "no such mode" : mode_name.data(), 0)) {
                    for (const BlendModeInfo& info : kBlendModes) {
                        const bool chosen = info.mode == layer.mode;
                        if (ImGui::Selectable(info.name, chosen)) {
                            commands.invoke(
                                "layer.set",
                                json{{"index", static_cast<int>(row)}, {"mode", info.name}});
                        }
                        if (chosen) {
                            ImGui::SetItemDefaultFocus();
                        }
                    }
                    ImGui::EndCombo();
                }
                ImGui::PopItemWidth();
            }
            ImGui::EndChild();
            const ImVec2 card_min = ImGui::GetItemRectMin();
            const ImVec2 card_max = ImGui::GetItemRectMax();
            card_state.box = ScreenRect{card_min.x, card_min.y, card_max.x - card_min.x,
                                        card_max.y - card_min.y};
            state.layer_cards.push_back(std::move(card_state));
            ImGui::PopStyleColor(3);
            ImGui::PopID();
        }
        if (ImGui::Button("Add layer")) {
            commands.invoke("layer.add", json::object());
        }
        ImGui::SameLine();
        if (ImGui::Button("Delete layer")) {
            commands.invoke("layer.remove", json::object());
        }
        // The active layer's mask, and which plane of it strokes write. Every control is a `mask.*`
        // command, so a click, the wire and a scripted test cannot disagree about what "Hide all"
        // means, and none of them has to know whether a mask is a plane, a channel or a claim.
        const Layer& active = state.doc.layer(static_cast<std::size_t>(state.doc.active()));
        ImGui::SeparatorText("mask");
        if (active.mask) {
            if (ImGui::Button("Remove")) {
                commands.invoke("mask.remove", json::object());
            }
            ImGui::SameLine();
            if (ImGui::Button("Show all")) {
                commands.invoke("mask.fill", json{{"to", "show"}});
            }
            ImGui::SameLine();
            if (ImGui::Button("Hide all")) {
                commands.invoke("mask.fill", json{{"to", "hide"}});
            }
            ImGui::SameLine();
            if (ImGui::Button("Invert")) {
                commands.invoke("mask.invert", json::object());
            }
            const json facts = layer_mask_json(active);
            const json& bounds = facts["bounds"];
            // Two lines, because one of them clipped mid-word in a panel a third of the window.
            // "Touched" and "shown" are the two counts a coverage plane has - how many pixels it
            // reached, and how much of them it shows - and an anti-aliased edge makes them differ, so
            // a painter who read only one would misjudge what the brush did.
            ImGui::TextDisabled("%d px touched, %.1f px shown",
                                static_cast<int>(facts["covered"].get<std::int64_t>()),
                                facts["area"].get<double>());
            ImGui::TextDisabled("%d x %d at %d,%d", bounds[2].get<int>(), bounds[3].get<int>(),
                                bounds[0].get<int>(), bounds[1].get<int>());
        } else {
            if (ImGui::Button("Add mask")) {
                commands.invoke("mask.add", json::object());
            }
            ImGui::SameLine();
            ImGui::TextDisabled("nothing hidden");
        }
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

// The menu and the front door both ask for a path and act on the answer later: a native dialog is
// asynchronous by design, so the click that opened it and the document it produced cannot share a
// frame. This remembers which door the open picker is behind.
char front_door_prompt[kVerbFieldChars] = "";
// Which box the keyboard has, by name, taken after the widget drew: the same rule the verb fields use, so
// the form can fill a box a driver wrote to without rewriting one a hand is typing into.
std::string front_door_editing;
bool front_door_drawn = false;

// The file set is what is remembered, not a bespoke action word: the answer goes back through
// `file.answer` under the same name it came in with, so a picker and a probe cannot mean different things.
void start_pick(CommandTable& commands, const char* set, const char* title) {
    commands.invoke("file.pick", json{{"for", set}, {"title", title}});
}

// Files first, in the panel rather than in a menu bar: `BeginMainMenuBar()` asserts in this frame
// (`Missing End()` from `ErrorRecoveryTryToRecoverState`) with sokol_imgui and a fullscreen dockspace
// in ImGui 1.93 WIP, so a menu bar is a sokol/version question rather than a layout one. Until it is
// answered the actions are here, where they work, and the keys in the command table are the same keys
// a menu would have shown.
void draw_file_row(AppState& state, CommandTable& commands) {
    if (ImGui::Button("New")) {
        commands.invoke("file.new", json::object());
    }
    ImGui::SameLine();
    if (ImGui::Button("Open")) {
        start_pick(commands, "open", "Open an image or a document");
    }
    const bool has_document = !state.doc.empty();
    ImGui::SameLine();
    ImGui::BeginDisabled(!has_document || state.doc.path().empty());
    if (ImGui::Button("Save")) {
        commands.invoke("doc.save", json::object());
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!has_document);
    if (ImGui::Button("Save As")) {
        start_pick(commands, "project", "Save document");
    }
    ImGui::SameLine();
    if (ImGui::Button("Export")) {
        start_pick(commands, "flat", "Export an image");
    }
    // The way in that does not wait for the compositor to hand the window a file: under Wayland the
    // editor is an X11 client, so a drop has to cross XWayland's XDND bridge, which is not ours to fix.
    // This runs the command a drop runs, centred on the canvas instead of under a pointer.
    ImGui::SameLine();
    if (ImGui::Button("Insert")) {
        start_pick(commands, "image", "Insert an image");
    }
    ImGui::EndDisabled();
    if (has_document && state.doc.dirty()) {
        ImGui::SameLine();
        ImGui::TextDisabled("unsaved");
    }
}

// The body both doors draw: a size and, if the person already knows what they want, the words. Reading
// the fields through the command rather than writing the struct keeps a click, a key and a driver
// asking for the same Create.
void draw_new_document_body(AppState& state, CommandTable& commands) {
    ImGui::TextDisabled("size");
    for (const NewDocumentPreset& preset : kNewDocumentPresets) {
        char label[96];
        std::snprintf(label, sizeof(label), "%d x %d   %s", preset.width, preset.height, preset.label);
        if (ImGui::Selectable(label, state.new_document.width == preset.width &&
                                      state.new_document.height == preset.height)) {
            commands.invoke("new_document",
                            json{{"width", preset.width}, {"height", preset.height}});
        }
    }
    int width = state.new_document.width;
    int height = state.new_document.height;
    bool resized = false;
    ImGui::PushItemWidth(84.0f);
    resized = ImGui::InputInt("w", &width, 0, 0) || resized;
    ImGui::SameLine();
    resized = ImGui::InputInt("h", &height, 0, 0) || resized;
    ImGui::PopItemWidth();
    if (resized) {
        commands.invoke("new_document", json{{"width", width}, {"height", height}});
    }
    // The kinds are the recipe family's rows, read through the command that stores the choice: the label
    // that says "photo" and the recipe a press runs are one table read, so they cannot drift.
    const json form = commands.invoke("new_document", json::object()).value;
    const json& kinds = form["kinds"];
    if (!kinds.empty()) {
        ImGui::TextDisabled("kind");
        const std::string chosen = form["kind"].get<std::string>();
        for (const json& kind : kinds) {
            const std::string id = kind["id"].get<std::string>();
            const std::string label = kind["label"].get<std::string>() + "###" + id;
            if (ImGui::RadioButton(label.c_str(), chosen == id)) {
                commands.invoke("new_document", json{{"kind", id}});
            }
            ImGui::SameLine();
        }
        ImGui::NewLine();
    }
    ImGui::TextDisabled("what should it be?  (empty makes a blank canvas)");
    if (front_door_editing != "prompt") {
        std::snprintf(front_door_prompt, sizeof(front_door_prompt), "%s",
                      state.new_document.prompt.c_str());
    }
    ImGui::SetNextItemWidth(-1.0f);
    // The value goes out as typed, blanks included: clearing the box means a blank canvas, which is what
    // the label above it says.
    if (ImGui::InputTextMultiline("##new_prompt", front_door_prompt, sizeof(front_door_prompt) - 1,
                                  ImVec2(-1.0f, ImGui::GetTextLineHeight() * 3.0f))) {
        commands.invoke("new_document", json{{"prompt", front_door_prompt}});
    }
    front_door_editing = ImGui::IsItemActive() ? "prompt" : std::string();
}

void create_from_front_door(CommandTable& commands) {
    const CommandResult made = commands.invoke("front_door.create", json::object());
    if (!made.ok) {
        // The door stays open on a refusal: the words are usually what is wrong, and a dialog that shut
        // on the way out would throw away what was typed with it.
        log::error("new document: " + made.error);
    }
}

void draw_front_door(AppState& state, CommandTable& commands) {
    const bool wanted = state.front_door != FrontDoor::closed;
    if (wanted && !front_door_drawn) {
        std::snprintf(front_door_prompt, sizeof(front_door_prompt), "%s",
                      state.new_document.prompt.c_str());
        ImGui::OpenPopup("Welcome");
        front_door_drawn = true;
    }
    if (!wanted && front_door_drawn) {
        // Not drawn is not open: the popup only exists while its Begin is called, so closing the door
        // is stopping the call rather than fighting the popup stack.
        front_door_drawn = false;
    }
    if (!front_door_drawn) {
        return;
    }
    const bool welcome = state.front_door == FrontDoor::welcome;
    const ImGuiViewport* door_viewport = ImGui::GetMainViewport();
    const ImVec2 door_size(welcome ? 760.0f : 360.0f, 380.0f);
    ImGui::SetNextWindowSize(door_size, ImGuiCond_Appearing);
    ImGui::SetNextWindowPos(ImVec2(door_viewport->Pos.x + (door_viewport->Size.x - door_size.x) * 0.5f,
                                   door_viewport->Pos.y + (door_viewport->Size.y - door_size.y) * 0.4f),
                            ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal("Welcome", nullptr, 0)) {
        return;
    }
    if (ImGui::BeginChild("new", ImVec2(welcome ? 340.0f : 0.0f, 0.0f), false)) {
        draw_new_document_body(state, commands);
        if (ImGui::Button("Create")) {
            create_from_front_door(commands);
        }
        ImGui::SameLine();
        // Inside the column rather than under both of them: the children take the dialog's height, and
        // a button below them was a pixel or two of blue at the bottom edge.
        if (ImGui::Button("Cancel")) {
            commands.invoke("front_door", json{{"show", "close"}});
        }
    }
    ImGui::EndChild();
    if (welcome) {
        ImGui::SameLine();
        if (ImGui::BeginChild("open", ImVec2(0.0f, 0.0f), false)) {
            ImGui::TextDisabled("or open what is already there");
            const json files = commands.invoke("file.recent", json::object()).value.value(
                "files", json::array());
            if (files.empty()) {
                ImGui::TextDisabled("nothing opened yet");
            }
            for (const json& entry : files) {
                const std::string path = entry.get<std::string>();
                if (ImGui::Selectable(path.c_str())) {
                    commands.invoke("doc.open", json{{"path", path}});
                    commands.invoke("front_door", json{{"show", "close"}});
                }
            }
            if (ImGui::Button("Browse...")) {
                start_pick(commands, "open", "Open an image or a document");
            }
        }
        ImGui::EndChild();
    }
    ImGui::EndPopup();
}

// A key asked to leave and the document is not saved. The question is `app.quit`'s, held in
// `quit_pending`, so the window draws an answer rather than owning a decision: `Keep editing` withdraws
// the ask (which is also what a second Escape does), and `Discard` is the same press with `force`.
// Saving is not offered here - that is a different question, with a file dialog of its own.
void draw_quit_guard(AppState& state, CommandTable& commands) {
    if (!state.doc.dirty()) {
        // The question exists only while there is something to discard, so saving answers it.
        state.quit_pending = false;
    }
    if (!state.quit_pending) {
        return;
    }
    static bool shown = false;
    if (!ImGui::IsPopupOpen("Discard unsaved changes?")) {
        ImGui::OpenPopup("Discard unsaved changes?");
    }
    bool open_now = false;
    if (ImGui::BeginPopupModal("Discard unsaved changes?", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
        open_now = true;
        if (state.doc.path().empty()) {
            ImGui::TextUnformatted("This picture has unsaved changes.");
        } else {
            ImGui::Text("'%s' has unsaved changes.", state.doc.path().c_str());
        }
        if (ImGui::Button("Keep editing")) {
            commands.invoke("app.quit", json::object());
        }
        ImGui::SameLine();
        if (ImGui::Button("Discard")) {
            commands.invoke("app.quit", json{{"force", true}});
        }
        ImGui::EndPopup();
    }
    // `Escape` closes a modal before the key reaches anyone, so the popup closing on its own is the
    // answer: the question is withdrawn rather than re-opened the next frame, which is what a person
    // pressing Escape means.
    if (shown && !open_now) {
        state.quit_pending = false;
    }
    shown = open_now;
}

void draw_ui(AppState& state, CommandTable& commands, gfx::Texture& canvas_texture,
             const gfx::Checker& checker, CoverageTint& tint, LayerThumbs& thumbs) {
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
    // A dialog that was answered badly is the one failure a person can be left holding: the picker is
    // gone by the time the command refuses, and the reason has to be where they are looking.
    if (!state.pick_note.empty()) {
        ImGui::SameLine();
        ImGui::TextDisabled("%s", state.pick_note.c_str());
    }
    ImGui::SameLine();
    // What a stroke would paint now, and how far back the pixels can go: enough to read a
    // screenshot back into the settings that made it.
    ImGui::TextDisabled("%s %.0f px", state.tool.c_str(), static_cast<double>(state.brush.size));
    ImGui::SameLine();
    if (state.doc.selection()) {
        const Rect bounds = state.doc.selection()->bounds();
        ImGui::TextDisabled("sel %d x %d", bounds.w, bounds.h);
        ImGui::SameLine();
    }
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
    ImGui::NewLine();
    draw_ai_status_line(state, commands, ai_view);
    ImGui::End();
}

}
