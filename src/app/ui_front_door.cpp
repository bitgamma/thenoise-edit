// SPDX-License-Identifier: Apache-2.0
#include "app/ui_internal.h"

#include "app/verb_fields.h"
#include "app/ui_prompt.h"
#include "app/ui_icons.h"
#include "app/ui_theme.h"

#include "core/log.h"

#include "imgui.h"

#include <algorithm>
#include <cstdio>
#include <iterator>
#include <string>

namespace noise::app {

using nlohmann::json;

namespace {

constexpr float kWelcomeWidthPx = 900.0f;
constexpr float kWelcomeHeightPx = 600.0f;
constexpr float kNewOnlyWidthPx = 430.0f;
constexpr float kWindowMarginPx = 24.0f;
constexpr float kPanelGapPx = 18.0f;
constexpr float kNewPanelFraction = 0.51f;
constexpr float kSinglePanelThresholdPx = 670.0f;
constexpr float kPresetRowHeightPx = 29.0f;
constexpr float kStyleButtonHeightPx = 28.0f;
constexpr float kFooterButtonHeightPx = 33.0f;
constexpr float kNewFooterSeparatorHeightPx = 1.0f;
constexpr float kRecentFooterHeightPx = 54.0f;
constexpr float kPromptVisibleLines = 3.0f;
constexpr float kWelcomeHeadingScale = 1.22f;

std::string front_door_prompt;
std::string front_door_editing;
std::string front_door_error;
std::string recent_open_error;
bool front_door_drawn = false;

void start_pick(CommandTable& commands, const char* set, const char* title) {
    const CommandResult picked = commands.invoke(
        "file.pick", json{{"for", set}, {"title", title}});
    recent_open_error = picked.ok ? std::string() : picked.error;
}

void draw_size_presets(AppState& state, CommandTable& commands) {
    ImGui::TextUnformatted("Canvas size");
    const ui_theme::Palette& theme = ui_theme::palette(state.ui_theme);
    constexpr ImGuiTableFlags flags = ImGuiTableFlags_SizingStretchSame |
                                      ImGuiTableFlags_NoSavedSettings |
                                      ImGuiTableFlags_NoPadOuterX;
    if (!ImGui::BeginTable("##new_size_presets", 2, flags)) return;
    for (std::size_t index = 0; index < std::size(kNewDocumentPresets); ++index) {
        const NewDocumentPreset& preset = kNewDocumentPresets[index];
        if (index % 2 == 0) ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(static_cast<int>(index % 2));
        const bool selected = state.new_document.width == preset.width &&
                              state.new_document.height == preset.height;
        char label[96];
        std::snprintf(label, sizeof(label), "%d x %d  %s", preset.width, preset.height,
                      preset.label);
        ImGui::PushID(static_cast<int>(index));
        if (selected) ImGui::PushStyleColor(ImGuiCol_Header, theme.accent_soft);
        if (ImGui::Selectable(label, selected, ImGuiSelectableFlags_None,
                              ImVec2(0.0f, kPresetRowHeightPx))) {
            commands.invoke("new_document", json{{"width", preset.width},
                                                  {"height", preset.height}});
            front_door_error.clear();
        }
        if (selected) ImGui::PopStyleColor();
        ImGui::PopID();
    }
    ImGui::EndTable();
}

void draw_custom_size(AppState& state, CommandTable& commands) {
    ImGui::TextDisabled("Custom dimensions");
    int width = state.new_document.width;
    int height = state.new_document.height;
    constexpr ImGuiTableFlags flags = ImGuiTableFlags_SizingStretchSame |
                                      ImGuiTableFlags_NoSavedSettings |
                                      ImGuiTableFlags_NoPadOuterX;
    if (!ImGui::BeginTable("##new_custom_size", 2, flags)) return;
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    ImGui::TextUnformatted("Width (px)");
    ImGui::SetNextItemWidth(-1.0f);
    const bool width_changed = ImGui::InputInt("##new_width", &width, 0, 0);
    ImGui::TableSetColumnIndex(1);
    ImGui::TextUnformatted("Height (px)");
    ImGui::SetNextItemWidth(-1.0f);
    const bool height_changed = ImGui::InputInt("##new_height", &height, 0, 0);
    ImGui::EndTable();
    if (width_changed || height_changed) {
        const CommandResult result = commands.invoke(
            "new_document", json{{"width", width}, {"height", height}});
        front_door_error = result.ok ? std::string() : result.error;
    }
}

void draw_document_style(AppState& state, CommandTable& commands) {
    const json form = commands.invoke("new_document", json::object()).value;
    const json& kinds = form["kinds"];
    if (kinds.empty()) return;
    ImGui::TextUnformatted("Style");
    const std::string chosen = form["kind"].get<std::string>();
    const ui_theme::Palette& theme = ui_theme::palette(state.ui_theme);
    const float count = static_cast<float>(kinds.size());
    const float gap = ImGui::GetStyle().ItemSpacing.x;
    const float button_width = std::max(1.0f,
        (ImGui::GetContentRegionAvail().x - gap * (count - 1.0f)) / count);
    bool first = true;
    for (const json& kind : kinds) {
        if (!first) ImGui::SameLine();
        first = false;
        const std::string id = kind["id"].get<std::string>();
        const bool selected = chosen == id;
        ImGui::PushID(id.c_str());
        ImGui::PushStyleColor(ImGuiCol_Button, selected ? theme.accent_soft : theme.surface);
        if (ImGui::Button(kind["label"].get<std::string>().c_str(),
                          ImVec2(button_width, kStyleButtonHeightPx))) {
            commands.invoke("new_document", json{{"kind", id}});
            front_door_error.clear();
        }
        ImGui::PopStyleColor();
        ImGui::PopID();
    }
}

void draw_document_prompt(AppState& state, CommandTable& commands) {
    ImGui::TextUnformatted("Prompt (optional)");
    ImGui::TextDisabled("Leave empty to create a blank canvas.");
    if (front_door_editing != "prompt") front_door_prompt = state.new_document.prompt;
    if (prompt_input("##new_prompt", front_door_prompt,
                     ImGui::GetTextLineHeightWithSpacing() * kPromptVisibleLines,
                     ImGuiInputTextFlags_None)) {
        commands.invoke("new_document", json{{"prompt", front_door_prompt}});
        front_door_error.clear();
    }
    front_door_editing = ImGui::IsItemActive() ? "prompt" : std::string();
}

void create_from_front_door(CommandTable& commands) {
    const CommandResult made = commands.invoke("front_door.create", json::object());
    front_door_error = made.ok ? std::string() : made.error;
    if (!made.ok) log::error("new document: " + made.error);
}

void draw_new_document_body(AppState& state, CommandTable& commands) {
    ImGui::TextUnformatted("New image");
    ImGui::TextDisabled("Choose a size or enter custom dimensions.");
    ImGui::Separator();
    draw_size_presets(state, commands);
    ImGui::Spacing();
    draw_custom_size(state, commands);
    ImGui::Spacing();
    draw_document_style(state, commands);
    ImGui::Spacing();
    draw_document_prompt(state, commands);
    if (!front_door_error.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, ui_theme::palette(state.ui_theme).danger);
        ImGui::TextWrapped("%s", front_door_error.c_str());
        ImGui::PopStyleColor();
    }
}

void draw_new_footer(CommandTable& commands) {
    ImGui::Separator();
    if (ui_icons::labeled_button(ui_icons::Icon::ImagePlus, "Create image",
                                 ImVec2(148.0f, kFooterButtonHeightPx))) {
        create_from_front_door(commands);
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(84.0f, kFooterButtonHeightPx))) {
        commands.invoke("front_door", json{{"show", "close"}});
    }
}

void draw_recent_panel(AppState& state, CommandTable& commands) {
    ImGui::TextUnformatted("Open a file");
    ImGui::TextDisabled("Pick up where you left off.");
    ImGui::Separator();
    if (!recent_open_error.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, ui_theme::palette(state.ui_theme).danger);
        ImGui::TextWrapped("%s", recent_open_error.c_str());
        ImGui::PopStyleColor();
    }
    if (ImGui::BeginChild("##recent_items", ImVec2(0.0f, -kRecentFooterHeightPx))) {
        draw_recent_files_list(commands, recent_open_error);
    }
    ImGui::EndChild();
    ImGui::Separator();
    if (ui_icons::labeled_button(ui_icons::Icon::FolderOpen, "Browse files...",
                                  ImVec2(ImGui::GetContentRegionAvail().x,
                                         kFooterButtonHeightPx))) {
        start_pick(commands, "open", "Open an image or a document");
    }
}

void draw_new_pane(AppState& state, CommandTable& commands, ImVec2 size) {
    if (ImGui::BeginChild("##new_pane", size, ImGuiChildFlags_AlwaysUseWindowPadding,
                          ImGuiWindowFlags_None)) {
        const float footer_reserve = kFooterButtonHeightPx +
            2.0f * ImGui::GetStyle().ItemSpacing.y + kNewFooterSeparatorHeightPx;
        if (ImGui::BeginChild("##new_form", ImVec2(0.0f, -footer_reserve),
                              ImGuiChildFlags_None, ImGuiWindowFlags_None)) {
            draw_new_document_body(state, commands);
        }
        ImGui::EndChild();
        draw_new_footer(commands);
    }
    ImGui::EndChild();
}

void draw_open_pane(AppState& state, CommandTable& commands, ImVec2 size) {
    if (ImGui::BeginChild("##recent_pane", size, ImGuiChildFlags_AlwaysUseWindowPadding,
                          ImGuiWindowFlags_None)) {
        draw_recent_panel(state, commands);
    }
    ImGui::EndChild();
}

void draw_welcome_content(AppState& state, CommandTable& commands, bool welcome) {
    const float available = ImGui::GetContentRegionAvail().x;
    if (welcome && available < kSinglePanelThresholdPx) {
        if (ImGui::BeginTabBar("##welcome_tabs")) {
            if (ImGui::BeginTabItem("New image")) {
                draw_new_pane(state, commands, ImVec2(0.0f, 0.0f));
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Recent files")) {
                draw_open_pane(state, commands, ImVec2(0.0f, 0.0f));
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
        return;
    }
    const float new_width = welcome
        ? (available - kPanelGapPx) * kNewPanelFraction : available;
    draw_new_pane(state, commands, ImVec2(new_width, 0.0f));
    if (welcome) {
        ImGui::SameLine(0.0f, kPanelGapPx);
        draw_open_pane(state, commands, ImVec2(0.0f, 0.0f));
    }
}

} // namespace

void draw_front_door(AppState& state, CommandTable& commands) {
    const bool wanted = state.front_door != FrontDoor::closed;
    if (wanted && !front_door_drawn) {
        front_door_prompt = state.new_document.prompt;
        front_door_editing.clear();
        front_door_error.clear();
        recent_open_error.clear();
        ImGui::OpenPopup("Welcome");
        front_door_drawn = true;
    }
    if (!wanted && front_door_drawn) front_door_drawn = false;
    if (!front_door_drawn) return;

    const bool welcome = state.front_door == FrontDoor::welcome;
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const float width = std::min(welcome ? kWelcomeWidthPx : kNewOnlyWidthPx,
                                 viewport->WorkSize.x - kWindowMarginPx * 2.0f);
    const float height = std::min(kWelcomeHeightPx,
                                  viewport->WorkSize.y - kWindowMarginPx * 2.0f);
    const ImVec2 size(std::max(width, 1.0f), std::max(height, 1.0f));
    ImGui::SetNextWindowSize(size, ImGuiCond_Appearing);
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + (viewport->WorkSize.x - size.x) * 0.5f,
                                   viewport->WorkPos.y + (viewport->WorkSize.y - size.y) * 0.5f),
                            ImGuiCond_Appearing);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16.0f, 12.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8.0f, 6.0f));
    if (!ImGui::BeginPopupModal("Welcome", nullptr, ImGuiWindowFlags_NoSavedSettings)) {
        ImGui::PopStyleVar(2);
        return;
    }
    ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * kWelcomeHeadingScale);
    ImGui::TextUnformatted(welcome ? "Welcome to TheNoise-Edit" : "Create a new image");
    ImGui::PopFont();
    ImGui::TextDisabled(welcome ? "Start a new image or open an existing project."
                                : "Set up your canvas and start creating.");
    ImGui::Spacing();

    draw_welcome_content(state, commands, welcome);
    ImGui::EndPopup();
    ImGui::PopStyleVar(2);
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
    // `Escape` closes a modal before the key reaches anyone, so the popup closing on its own is
    // the answer: the question is withdrawn rather than re-opened the next frame, which is what a person
    // pressing Escape means.
    if (shown && !open_now) {
        state.quit_pending = false;
    }
    shown = open_now;
}

}
