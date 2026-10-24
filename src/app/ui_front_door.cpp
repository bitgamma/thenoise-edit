// SPDX-License-Identifier: Apache-2.0
#include "app/ui_internal.h"

#include "app/ui_theme.h"
#include "app/verb_fields.h"

#include "core/log.h"

#include "imgui.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <string>

namespace noise::app {

using nlohmann::json;

namespace {
constexpr float kThemeSelectorWidthPx = 82.0f;
constexpr float kFileActionPaddingPx = 1.0f;
constexpr float kFileActionVerticalPaddingPx = 3.0f;
constexpr float kFileActionSpacingPx = 2.0f;
// The menu and the front door both ask for a path and act on the answer later: a native dialog is
// asynchronous by design, so the click that opened it and the document it produced cannot share a
// frame. This remembers which door the open picker is behind.
char front_door_prompt[kVerbFieldChars] = "";
// Which box the keyboard has, by name, taken after the widget drew: the same rule the verb fields use, so
// the form can fill a box a driver wrote to without rewriting one a hand is typing into.
std::string front_door_editing;
bool front_door_drawn = false;


void start_pick(CommandTable& commands, const char* set, const char* title) {
    commands.invoke("file.pick", json{{"for", set}, {"title", title}});
}

void draw_file_buttons(AppState& state, CommandTable& commands, bool has_document) {
    if (ImGui::Button("New")) {
        commands.invoke("file.new", json::object());
    }
    ImGui::SameLine();
    if (ImGui::Button("Open")) {
        start_pick(commands, "open", "Open an image or a document");
    }
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
    ImGui::EndDisabled();
}

void draw_theme_picker(AppState& state, CommandTable& commands) {
    ImGui::TextDisabled("Theme");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(kThemeSelectorWidthPx);
    const std::string selected_theme = io::theme_name(state.ui_theme);
    if (!ImGui::BeginCombo("##theme", selected_theme.c_str())) {
        return;
    }
    constexpr std::array<io::Theme, 2> choices{io::Theme::dark, io::Theme::light};
    for (const io::Theme choice : choices) {
        const std::string name = io::theme_name(choice);
        const bool selected = choice == state.ui_theme;
        if (ImGui::Selectable(name.c_str(), selected) && !selected) {
            const CommandResult changed = commands.invoke("ui.theme", json{{"theme", name}});
            if (!changed.ok) {
                log::error(changed.error);
            }
        }
        if (selected) {
            ImGui::SetItemDefaultFocus();
        }
    }
    ImGui::EndCombo();
}

void draw_export_insert_buttons(CommandTable& commands, bool has_document) {
    ImGui::SameLine();
    ImGui::BeginDisabled(!has_document);
    if (ImGui::Button("Export")) {
        start_pick(commands, "flat", "Export an image");
    }
    ImGui::SameLine();
    if (ImGui::Button("Insert")) {
        start_pick(commands, "image", "Insert an image");
    }
    ImGui::EndDisabled();
}
}

// Files first, in the panel rather than in a menu bar: `BeginMainMenuBar()` asserts in this frame
// (`Missing End()` from `ErrorRecoveryTryToRecoverState`) with sokol_imgui and a fullscreen dockspace
// in ImGui 1.93 WIP, so a menu bar is a sokol/version question rather than a layout one. Until it is
// answered the actions are here, where they work, and the keys in the command table are the same keys
// a menu would have shown.
void draw_file_row(AppState& state, CommandTable& commands) {
    const bool has_document = !state.doc.empty();
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,
                        ImVec2(kFileActionPaddingPx, kFileActionVerticalPaddingPx));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,
                        ImVec2(kFileActionSpacingPx, ImGui::GetStyle().ItemSpacing.y));
    draw_file_buttons(state, commands, has_document);
    draw_export_insert_buttons(commands, has_document);
    ImGui::NewLine();
    draw_theme_picker(state, commands);
    if (has_document && state.doc.dirty()) {
        ImGui::SameLine();
        ImGui::TextDisabled("unsaved");
    }
    ImGui::PopStyleVar(2);
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
    ImGui::PushItemWidth(96.0f);
    resized = ImGui::InputInt("Width", &width, 0, 0) || resized;
    ImGui::SameLine();
    resized = ImGui::InputInt("Height", &height, 0, 0) || resized;
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

}
