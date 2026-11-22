// SPDX-License-Identifier: Apache-2.0
#include "app/verb_bar.h"

#include "imgui.h"

#include "app/verb_fields.h"
#include "app/ui_icons.h"
#include "app/ui_theme.h"
#include "app/ui_prompt.h"
#include "app/selection_edit.h"
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
constexpr double kAskWidthPx = 340.0;
constexpr float kAiNoticeWidthPx = 380.0f;
constexpr float kAiNoticeGapPx = 9.0f;
bool action_notice_visible = false;

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

    const std::string field = ask["field"].get<std::string>();
    std::string& box = verb_field_boxes()[verb];
    if (!field_held) {
        box = ask["value"].get<std::string>();
    }

    const ImGuiStyle& style = ImGui::GetStyle();
    const double width = std::min(std::max(kAskWidthPx, static_cast<double>(state.verb_bar.w)),
                                  std::max(1.0, static_cast<double>(state.viewport.w)));
    const double above = state.verb_bar.y - kBarGapPx - state.canvas_origin.y;
    const double below = state.canvas_origin.y + state.viewport.h -
                         (state.verb_bar.y + state.verb_bar.h + kBarGapPx);
    const float line = ImGui::GetTextLineHeightWithSpacing();
    const float error_height = refused.empty() ? 0.0f
        : ImGui::CalcTextSize(refused.c_str(), nullptr, false,
                              static_cast<float>(width - kBarPadPx * 2.0)).y +
              style.ItemSpacing.y;
    const double chrome = kBarPadPx * 2.0 + line * 2.0 + ImGui::GetFrameHeight() +
                          style.ItemSpacing.y * 3.0 + error_height;
    const float prompt_max = std::max(kPromptMinVisibleLines * line,
                                     std::min(kPromptMaxVisibleLines * line,
                                              static_cast<float>(std::max(above, below) - chrome)));
    const float input_height = prompt_field_height(box, static_cast<float>(width) -
                                                    static_cast<float>(kBarPadPx * 2.0), prompt_max);
    const double reserve = chrome + input_height;
    bool popup_above = bar_above_mark;
    if ((popup_above ? above : below) < reserve &&
        (popup_above ? below : above) > (popup_above ? above : below)) {
        popup_above = !popup_above;
    }
    double x = state.verb_bar.x;
    double y = popup_above ? state.verb_bar.y - kBarGapPx - reserve
                           : state.verb_bar.y + state.verb_bar.h + kBarGapPx;
    x = std::clamp(x, static_cast<double>(state.canvas_origin.x),
                   std::max(static_cast<double>(state.canvas_origin.x),
                            state.canvas_origin.x + state.viewport.w - width));
    y = std::clamp(y, static_cast<double>(state.canvas_origin.y),
                   std::max(static_cast<double>(state.canvas_origin.y),
                            state.canvas_origin.y + state.viewport.h - reserve));

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

    ImGui::TextUnformatted(ask["label"].get<std::string>().c_str());
    ImGui::SameLine();
    ImGui::TextDisabled("(%s)", field.c_str());
    if (just_opened) {
        ImGui::SetKeyboardFocusHere();
    }
    const bool confirmed_enter = prompt_input("##ask_words", box, input_height,
        ImGuiInputTextFlags_CtrlEnterForNewLine | ImGuiInputTextFlags_EnterReturnsTrue);
    if (ImGui::IsItemEdited()) {
        refused.clear();
        commands.invoke("ai.intent", json{{"recipe", ask["recipe"]},
                                          {"vars", json{{field, box}}}});
    }
    field_held = ImGui::IsItemActive();
    ImGui::TextDisabled("Enter: Run   Shift+Enter: New line");

    const bool confirmed_click = ImGui::Button("Run");
    ImGui::SameLine();
    if (ImGui::Button("Cancel")) {
        commands.invoke("verb.ask", json{{"show", "close"}});
    }
    if (!refused.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        ImGui::TextWrapped("%s", refused.c_str());
        ImGui::PopStyleColor();
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

void draw_ai_action_notice(AppState& state, CommandTable& commands) {
    const std::string& error = state.ai_intent.error;
    if (error.empty() || state.verb_bar.empty()) {
        action_notice_visible = false;
        return;
    }
    if (!action_notice_visible) return;
    const json intent = commands.invoke("ai.intent", json::object()).value;
    const float width = std::max(1.0f, std::min(kAiNoticeWidthPx,
                              static_cast<float>(state.viewport.w) - 12.0f));
    const float line = ImGui::GetTextLineHeightWithSpacing();
    const float text_height = ImGui::CalcTextSize(error.c_str(), nullptr, false,
                                                  width - 24.0f).y;
    const float estimated_height = text_height + line * 4.0f + 28.0f;
    const float canvas_top = static_cast<float>(state.canvas_origin.y);
    const float canvas_bottom = canvas_top + static_cast<float>(state.viewport.h);
    const float space_below = canvas_bottom - state.verb_bar.y - state.verb_bar.h;
    const float x = std::clamp(state.verb_bar.x, static_cast<float>(state.canvas_origin.x),
                               std::max(static_cast<float>(state.canvas_origin.x),
                                        static_cast<float>(state.canvas_origin.x + state.viewport.w) - width));
    const float preferred_y = space_below >= estimated_height + kAiNoticeGapPx
        ? state.verb_bar.y + state.verb_bar.h + kAiNoticeGapPx
        : state.verb_bar.y - estimated_height - kAiNoticeGapPx;
    const float y = std::clamp(preferred_y, canvas_top + kAiNoticeGapPx,
                               std::max(canvas_top + kAiNoticeGapPx,
                                        canvas_bottom - estimated_height - kAiNoticeGapPx));
    ImGui::SetNextWindowPos(ImVec2(x, y), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(width, 0.0f));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImGui::GetStyleColorVec4(ImGuiCol_PopupBg));
    ImGui::PushStyleColor(ImGuiCol_Border, ui_theme::palette(state.ui_theme).danger);
    ImGui::Begin("##ai_action_notice", nullptr,
                  ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                  ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                  ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize);
    ImGui::TextColored(ui_theme::palette(state.ui_theme).danger, "AI action unavailable");
    ImGui::TextWrapped("%s", error.c_str());
    if (!intent.value("configured", false)) {
        ImGui::TextDisabled("Set $THENOISE_URL and restart the editor.");
    } else if (!intent.value("connected", false)) {
        if (ImGui::Button("Retry connection")) {
            commands.invoke("ai.intent", json{{"connect", true}});
        }
        ImGui::SameLine();
    }
    if (ImGui::Button("Dismiss")) {
        commands.invoke("ai.intent", json{{"connect", false}});
        action_notice_visible = false;
    }
    ImGui::End();
    ImGui::PopStyleColor(2);
}

}  // namespace

void draw_verb_bar(AppState& state, CommandTable& commands) {
    state.verb_bar = {};
    const SelectionEditPhase mode = selection_edit_phase(state);
    const std::optional<Rect> mark = verb_bar_mark(state);
    if (!mark || selection_edit_hides_bar(state)) {
        action_notice_visible = false;
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
    double row_width = kBarPadPx * 2.0;
    double row_height = 0.0;
    for (const json& row : rows) {
        const std::string label = row["label"].get<std::string>();
        const ImVec2 text = ImGui::CalcTextSize(label.c_str());
        row_width += ui_icons::labeled_button_width(label.c_str()) + style.ItemSpacing.x;
        row_height = std::max(row_height,
                              static_cast<double>(text.y + style.FramePadding.y * 2.0f));
    }
    row_width -= style.ItemSpacing.x;
    width = std::max(width, row_width);
    height = row_height;
    height += kBarPadPx * 2.0;

    double x = anchor_x + (far_x - anchor_x - width) * 0.5;
    const double canvas_top = state.canvas_origin.y;
    const double canvas_bottom = state.canvas_origin.y + state.viewport.h;
    const double above_y = anchor_y - kBarGapPx - height;
    const double below_y = far_y + kBarGapPx;
    const bool above_fits = above_y >= canvas_top && above_y + height <= canvas_bottom;
    const bool below_fits = below_y >= canvas_top && below_y + height <= canvas_bottom;
    double y = above_y;
    bool bar_above_mark = true;
    if (!above_fits && below_fits) {
        y = below_y;
        bar_above_mark = false;
    } else if (!above_fits) {
        y = canvas_top + kBarGapPx;
    }
    const double limit_x = state.canvas_origin.x + state.viewport.w - width;
    const double limit_y = canvas_bottom - height;
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
        ImGui::BeginDisabled(mode == SelectionEditPhase::generating);
        const bool pressed = ui_icons::labeled_button(
            ui_icons::for_verb(row["verb"].get<std::string>()),
            row["label"].get<std::string>().c_str(), ImVec2(0.0f, 0.0f));
        ImGui::EndDisabled();
        // A verb that wants words opens the box over the mark instead of running on whatever the panel
        // happens to hold; one that wants none runs, because a dialog in its way would be theatre.
        if (pressed) {
            if (!row["ready"].get<bool>() || row["field"].is_null()) {
                const CommandResult result = commands.invoke(
                    row["command"].get<std::string>(), json::object());
                action_notice_visible = !result.ok;
            } else {
                const CommandResult result = commands.invoke(
                    "verb.ask", json{{"show", row["verb"]}});
                action_notice_visible = !result.ok;
            }
        }
        // Why a button is dim, where a person looks when a button does nothing. The panel states the
        // same reason beside its own copy; the bar has no room for it and no need to restate it.
        if (!row["ready"].get<bool>() &&
            ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            ImGui::SetTooltip("%s", row["reason"].get<std::string>().c_str());
        }
    }
    ImGui::End();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(2);

    draw_ask(state, commands, verb_ask_state(state, commands), bar_above_mark);
    draw_ai_action_notice(state, commands);
}

}  // namespace noise::app
