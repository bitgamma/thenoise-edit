// SPDX-License-Identifier: Apache-2.0
#include "app/verb_bar.h"

#include "imgui.h"

#include "app/verb_fields.h"
#include "app/selection_edit.h"
#include "imgui_internal.h"  // BringWindowToDisplayFront: the bar has to sit above the canvas
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cstdio>
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
// A prompt is a sentence: a box that shows eight characters of it is a box nobody types into, so the
// phrase box is wider than the bar it hangs from.
constexpr double kAskWidthPx = 340.0;

// The words a marked-area verb asks for, typed over the mark rather than in the panel across the
// window. The words belong to the intent and the press to `ai.<verb>`; this holds nothing of its own
// but the edit buffer and the last refusal, and closes when the press is accepted.
void draw_ask(AppState& state, CommandTable& commands, const json& ask, bool bar_above_mark) {
    static std::string open_for;
    static std::string refused;
    static bool field_held = false;
    if (ask.is_null()) {
        state.verb_ask = {};
        open_for.clear();
        refused.clear();
        field_held = false;
        return;
    }
    const std::string verb = ask["verb"].get<std::string>();
    const bool just_opened = verb != open_for;
    open_for = verb;

    const ImGuiStyle& style = ImGui::GetStyle();
    const double width = std::max(kAskWidthPx, static_cast<double>(state.verb_bar.w));
    // Room enough for the tallest it can be (label, field, buttons, refusal) is used only to decide
    // which side of the bar it goes on; the box sizes itself to what it actually holds, because a box
    // measured by hand is a box that clips its own buttons.
    const double reserve = ImGui::GetTextLineHeight() * 4.0 + style.FramePadding.y * 4.0 +
                           style.ItemSpacing.y * 3.0 + kBarPadPx * 2.0;

    // On the far side of the bar from the mark: a box over the boundary would hide the edge whose
    // coverage is being described, and the bar is already on the other side of it.
    double x = state.verb_bar.x;
    double y = bar_above_mark ? state.verb_bar.y - kBarGapPx - reserve
                              : state.verb_bar.y + state.verb_bar.h + kBarGapPx;
    if (bar_above_mark && y < state.canvas_origin.y) {
        y = state.verb_bar.y + state.verb_bar.h + kBarGapPx;
    } else if (!bar_above_mark && y + reserve > state.canvas_origin.y + state.viewport.h) {
        y = state.verb_bar.y - kBarGapPx - reserve;
    }
    x = std::min(x, state.canvas_origin.x + state.viewport.w - width);
    x = std::max(x, static_cast<double>(state.canvas_origin.x));

    ImGui::SetNextWindowPos(ImVec2(static_cast<float>(x), static_cast<float>(y)), ImGuiCond_Always);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, IM_COL32(22, 22, 26, 242));
    ImGui::PushStyleColor(ImGuiCol_Border, IM_COL32(150, 150, 168, 220));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(static_cast<float>(kBarPadPx),
                                                            static_cast<float>(kBarPadPx)));
    ImGui::Begin("##verb_ask", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                     ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
                     ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_AlwaysAutoResize);
    ImGui::BringWindowToDisplayFront(ImGui::GetCurrentWindow());
    ImGui::Dummy(ImVec2(static_cast<float>(width) - style.WindowPadding.x * 2.0f, 0.0f));
    state.verb_ask = {ImGui::GetWindowPos().x, ImGui::GetWindowPos().y, ImGui::GetWindowSize().x,
                      ImGui::GetWindowSize().y};

    const std::string field = ask["field"].get<std::string>();
    std::array<char, kVerbFieldChars>& box = verb_field_boxes()[verb];
    if (!field_held) {
        std::snprintf(box.data(), box.size(), "%s", ask["value"].get<std::string>().c_str());
    }
    ImGui::TextUnformatted(ask["label"].get<std::string>().c_str());
    ImGui::SameLine();
    ImGui::TextDisabled("(%s)", field.c_str());
    if (just_opened) {
        ImGui::SetKeyboardFocusHere();
    }
    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::InputText("##ask_words", box.data(), box.size() - 1)) {
        refused.clear();
        commands.invoke("ai.intent", json{{"recipe", ask["recipe"]},
                                          {"vars", json{{field, box.data()}}}});
    }
    field_held = ImGui::IsItemActive();

    const bool confirmed_enter = ImGui::IsItemFocused() && ImGui::IsKeyPressed(ImGuiKey_Enter, false);
    const bool confirmed_click = ImGui::Button("Run");
    ImGui::SameLine();
    if (ImGui::Button("Cancel")) {
        commands.invoke("verb.ask", json{{"show", "close"}});
    }
    if (!refused.empty()) {
        ImGui::TextDisabled("%s", refused.c_str());
    }
    // A press that is refused leaves the box standing with the reason in it: the words are already in
    // the intent, so the fix is one edit away rather than a retyped phrase.
    if (confirmed_enter || confirmed_click) {
        const CommandResult ran =
            commands.invoke(ask["command"].get<std::string>(), json::object());
        if (ran.ok) {
            commands.invoke("verb.ask", json{{"show", "close"}});
        } else {
            refused = ran.error;
        }
    }
    ImGui::End();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(2);
}

}  // namespace

void draw_verb_bar(AppState& state, CommandTable& commands) {
    state.verb_bar = {};
    state.verb_new_edit = {};
    const SelectionEditPhase mode = selection_edit_phase(state);
    const std::optional<Rect> mark = verb_bar_anchor_mark(state);
    if (!mark) {
        return;
    }
    const json rows = mode == SelectionEditPhase::editing
                          ? json::array() : verb_bar_rows(commands, nullptr);
    if (rows.empty() && mode != SelectionEditPhase::editing) {
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
    if (mode == SelectionEditPhase::editing) {
        width += std::max(static_cast<double>(ImGui::CalcTextSize("Editor mode active").x),
                          static_cast<double>(ImGui::CalcTextSize("New AI edit").x +
                                              style.FramePadding.x * 2.0f));
        height = ImGui::GetTextLineHeight() + ImGui::GetFrameHeight() + style.ItemSpacing.y;
    } else {
        for (const json& row : rows) {
            const ImVec2 text = ImGui::CalcTextSize(row["label"].get<std::string>().c_str());
            width += text.x + style.FramePadding.x * 2.0 + style.ItemSpacing.x;
            height = std::max(height, static_cast<double>(text.y + style.FramePadding.y * 2.0f));
        }
        width -= style.ItemSpacing.x;
    }
    height += kBarPadPx * 2.0;

    // Above the mark when there is room, below when there is not, and always inside the canvas area so
    // the bar cannot hide behind the panel edge.
    double x = anchor_x + (far_x - anchor_x - width) * 0.5;
    double y = anchor_y - kBarGapPx - height;
    bool bar_above_mark = true;
    if (y < state.canvas_origin.y) {
        y = far_y + kBarGapPx;
        bar_above_mark = false;
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
    if (mode == SelectionEditPhase::editing) {
        ImGui::TextUnformatted("Editor mode active");
        if (ImGui::Button("New AI edit")) {
            commands.invoke("verb.new_edit", json::object());
        }
        state.verb_new_edit = {ImGui::GetItemRectMin().x, ImGui::GetItemRectMin().y,
                               ImGui::GetItemRectSize().x, ImGui::GetItemRectSize().y};
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
            ImGui::SetTooltip("Keep the result layer; switch Cover back to the next marked area");
        }
    }
    for (std::size_t i = 0; i < rows.size(); ++i) {
        const json& row = rows[i];
        if (i > 0) {
            ImGui::SameLine();
        }
        ImGui::BeginDisabled(mode != SelectionEditPhase::actions ||
                             !row["ready"].get<bool>());
        const bool pressed = ImGui::Button(row["label"].get<std::string>().c_str());
        ImGui::EndDisabled();
        // A verb that wants words opens the box over the mark instead of running on whatever the panel
        // happens to hold; one that wants none runs, because a dialog in its way would be theatre.
        if (pressed) {
            if (row["field"].is_null()) {
                commands.invoke(row["command"].get<std::string>(), json::object());
            } else {
                commands.invoke("verb.ask", json{{"show", row["verb"]}});
            }
        }
        // Why a button is dim, where a person looks when a button does nothing. The panel states the
        // same reason beside its own copy; the bar has no room for it and no need to restate it.
        if (!row["ready"].get<bool>() && ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s", row["reason"].get<std::string>().c_str());
        }
    }
    ImGui::End();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(2);

    draw_ask(state, commands, verb_ask_state(state, commands), bar_above_mark);
}

}  // namespace noise::app
