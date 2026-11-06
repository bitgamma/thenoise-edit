// SPDX-License-Identifier: Apache-2.0
#include "app/ui_internal.h"

#include "app/ui_theme.h"
#include "app/ui_icons.h"
#include "app/verb_fields.h"
#include "app/ui_prompt.h"

#include "imgui.h"

#include <map>
#include <string>

namespace noise::app {

using nlohmann::json;

namespace {
std::string pending_command;
json pending_args;
std::string pending_needs;
std::string pending_resident;
std::string editing_verb;

bool draw_verb_button(const json& verb, const std::string& label,
                      const std::string& reason) {
    const bool ready = verb["ready"].get<bool>();
    ImGui::BeginDisabled(!ready);
    const ui_icons::Icon icon = ui_icons::for_verb(verb["id"].get<std::string>());
    const bool pressed = ui_icons::labeled_button(icon, label.c_str(), ImVec2(0.0f, 0.0f));
    const bool hovered = ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort |
                                              ImGuiHoveredFlags_AllowWhenDisabled);
    ImGui::EndDisabled();
    if (!ready && !reason.empty() && hovered) {
        ImGui::SetTooltip("%s", reason.c_str());
    }
    ImGui::SameLine();
    ImGui::TextDisabled("[%s]", verb["key"].get<std::string>().c_str());
    return pressed;
}

ScreenRect item_box() {
    const ImVec2 a = ImGui::GetItemRectMin();
    const ImVec2 b = ImGui::GetItemRectMax();
    return ScreenRect{a.x, a.y, b.x - a.x, b.y - a.y};
}

void draw_verb_field(AppState& state, CommandTable& commands, const json& intent,
                     const std::string& id, const std::string& recipe, const std::string& field) {
    std::map<std::string, std::string>& boxes = verb_field_boxes();
    std::string& box = boxes[id];
    if (editing_verb != id) {
        const bool matches = intent["recipe"].is_string() &&
                             recipe == intent["recipe"].get<std::string>();
        const std::string shown = matches ? intent["vars"].value(field, std::string())
                                          : std::string();
        box = shown;
    }
    ImGui::TextDisabled("%s", field == "prompt" ? "Prompt" : field.c_str());
    const bool changed = prompt_input("##words", box,
                                      ImGui::GetTextLineHeightWithSpacing() * kPromptMaxVisibleLines,
                                      ImGuiInputTextFlags_None);
    state.ai_prompt_input = item_box();
    if (changed) {
        commands.invoke("ai.intent",
                        json{{"recipe", recipe}, {"vars", json{{field, box}}}});
    }
    editing_verb = ImGui::IsItemActive() ? id : std::string();
}

void draw_aspect_picker(AppState& state, CommandTable& commands, const json& intent,
                        const std::string& recipe, const json& aspects) {
    if (!aspects.is_array() || aspects.empty()) {
        return;
    }
    const std::string chosen =
        intent["aspect"].is_null() ? std::string() : intent["aspect"].get<std::string>();
    ImGui::TextDisabled("Aspect ratio");
    ImGui::SetNextItemWidth(-1.0f);
    const bool opened = ImGui::BeginCombo("##aspect", chosen.empty() ? "canvas" : chosen.c_str());
    state.ai_aspect_picker = item_box();
    if (!opened) return;
    if (ImGui::Selectable("canvas", chosen.empty())) {
        commands.invoke("ai.intent", json{{"recipe", recipe}, {"aspect", nullptr}});
    }
    state.ai_aspect_options.push_back(ToolPaletteButton{"canvas", item_box()});
    for (const json& offered : aspects) {
        const std::string name = offered.get<std::string>();
        const bool selected = name == chosen;
        if (ImGui::Selectable(name.c_str(), selected)) {
            commands.invoke("ai.intent", json{{"recipe", recipe}, {"aspect", name}});
        }
        state.ai_aspect_options.push_back(ToolPaletteButton{name, item_box()});
        if (selected) {
            ImGui::SetItemDefaultFocus();
        }
    }
    ImGui::EndCombo();
}

void draw_style_picker(AppState& state, CommandTable& commands, const json& verb, const std::string& label,
                       const std::string& recipe, const json& variants) {
    if (!variants.is_array() || variants.empty()) {
        return;
    }
    const std::string active = verb["active"].is_null()
                                   ? recipe
                                   : verb["active"].get<std::string>();
    std::string shown = label;
    for (const json& member : variants) {
        if (member["id"].get<std::string>() == active) {
            shown = member["label"].get<std::string>();
        }
    }
    ImGui::TextDisabled("Style");
    ImGui::SetNextItemWidth(-1.0f);
    const bool opened = ImGui::BeginCombo("##style", shown.c_str());
    state.ai_style_picker = item_box();
    if (!opened) return;
    if (ImGui::Selectable(label.c_str(), active == recipe)) {
        commands.invoke("ai.intent", json{{"recipe", recipe}});
    }
    state.ai_style_options.push_back(ToolPaletteButton{recipe, item_box()});
    for (const json& member : variants) {
        const std::string id = member["id"].get<std::string>();
        const std::string name = member["label"].get<std::string>();
        const bool selected = id == active;
        if (ImGui::Selectable(name.c_str(), selected)) {
            commands.invoke("ai.intent", json{{"recipe", id}});
        }
        state.ai_style_options.push_back(ToolPaletteButton{id, item_box()});
        if (selected) {
            ImGui::SetItemDefaultFocus();
        }
    }
    ImGui::EndCombo();
}

void activate_verb(CommandTable& commands, const json& verb) {
    const std::string command = verb["command"].get<std::string>();
    if (!verb["swap"].get<bool>()) {
        commands.invoke(command, json::object());
        return;
    }
    pending_command = command;
    pending_args = json::object();
    pending_needs = verb["model"].get<std::string>();
    pending_resident = verb["resident"].get<std::string>();
    ImGui::OpenPopup("Switch model?");
}

void draw_verb_row(AppState& state, CommandTable& commands, const json& intent, const json& verb) {
    const std::string id = verb["id"].get<std::string>();
    const std::string label = verb["label"].is_null() ? id : verb["label"].get<std::string>();
    const std::string reason = verb["reason"].get<std::string>();
    const std::string recipe = verb["recipe"].get<std::string>();
    const json ask = verb["ask"];
    const std::string field =
        ask.is_array() && !ask.empty() ? ask[0].get<std::string>() : std::string();

    const std::string active_recipe = verb["active"].is_string()
                                          ? verb["active"].get<std::string>()
                                          : recipe;
    ImGui::PushID(id.c_str());
    const bool generate = id == "generate";
    bool pressed = false;
    if (!generate) {
        pressed = draw_verb_button(verb, label, reason);
    }
    if (!field.empty()) {
        draw_verb_field(state, commands, intent, id, active_recipe, field);
    }
    if (generate) {
        draw_style_picker(state, commands, verb, label, recipe, verb["variants"]);
    }
    draw_aspect_picker(state, commands, intent, active_recipe, verb["aspects"]);
    if (!generate) {
        draw_style_picker(state, commands, verb, label, recipe, verb["variants"]);
    } else {
        pressed = draw_verb_button(verb, "Generate", reason);
    }
    ImGui::PopID();
    if (pressed) activate_verb(commands, verb);
}

void draw_model_switch_popup(CommandTable& commands) {
    if (!ImGui::BeginPopupModal("Switch model?", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        return;
    }
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

void draw_verb_section(AppState& state, CommandTable& commands, const json& intent, io::Theme theme) {
    state.ai_prompt_input = ScreenRect{};
    state.ai_aspect_picker = ScreenRect{};
    state.ai_style_picker = ScreenRect{};
    state.ai_aspect_options.clear();
    state.ai_style_options.clear();
    const json view = commands.invoke("ai.verbs", json::object()).value;
    const std::string table_error = view["error"].get<std::string>();
    if (!table_error.empty()) {
        ImGui::TextColored(ui_theme::palette(theme).danger, "%s", table_error.c_str());
        return;
    }
    for (const json& verb : view["verbs"]) {
        if (verb["scope"].get<std::string>() != "selection") {
            if (verb["id"] == "generate") {
                ImGui::SeparatorText("Generate image");
            } else if (verb["id"] == "blend") {
                ImGui::SeparatorText("Layer AI");
            }
            draw_verb_row(state, commands, intent, verb);
        }
    }
    draw_model_switch_popup(commands);
}

}
