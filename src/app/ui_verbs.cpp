// SPDX-License-Identifier: Apache-2.0
#include "app/ui_internal.h"

#include "app/ui_theme.h"
#include "app/verb_fields.h"

#include "imgui.h"

#include <array>
#include <cstdio>
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
    const bool pressed = ImGui::Button(label.c_str());
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::TextDisabled("[%s]", verb["key"].get<std::string>().c_str());
    if (!ready && !reason.empty() && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
        ImGui::SetTooltip("%s", reason.c_str());
    }
    return pressed;
}

void draw_verb_field(CommandTable& commands, const json& intent, const std::string& id,
                     const std::string& recipe, const std::string& field) {
    std::map<std::string, std::array<char, kVerbFieldChars>>& boxes = verb_field_boxes();
    std::array<char, kVerbFieldChars>& box = boxes[id];
    if (editing_verb != id) {
        const bool matches = intent["recipe"].is_string() &&
                             recipe == intent["recipe"].get<std::string>();
        const std::string shown = matches ? intent["vars"].value(field, std::string())
                                          : std::string();
        std::snprintf(box.data(), box.size(), "%s", shown.c_str());
    }
    ImGui::TextDisabled("%s", field.c_str());
    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::InputText("##words", box.data(), box.size() - 1)) {
        commands.invoke("ai.intent",
                        json{{"recipe", recipe}, {"vars", json{{field, box.data()}}}});
    }
    editing_verb = ImGui::IsItemActive() ? id : std::string();
}

void draw_aspect_picker(CommandTable& commands, const json& intent,
                        const std::string& recipe, const json& aspects) {
    if (!aspects.is_array() || aspects.empty()) {
        return;
    }
    const std::string chosen =
        intent["aspect"].is_null() ? std::string() : intent["aspect"].get<std::string>();
    ImGui::TextDisabled("aspect");
    ImGui::SetNextItemWidth(-1.0f);
    if (!ImGui::BeginCombo("##aspect", chosen.empty() ? "canvas" : chosen.c_str())) {
        return;
    }
    if (ImGui::Selectable("canvas", chosen.empty())) {
        commands.invoke("ai.intent", json{{"recipe", recipe}, {"aspect", nullptr}});
    }
    for (const json& offered : aspects) {
        const std::string name = offered.get<std::string>();
        const bool selected = name == chosen;
        if (ImGui::Selectable(name.c_str(), selected)) {
            commands.invoke("ai.intent", json{{"recipe", recipe}, {"aspect", name}});
        }
        if (selected) {
            ImGui::SetItemDefaultFocus();
        }
    }
    ImGui::EndCombo();
}

void draw_style_picker(CommandTable& commands, const json& verb, const std::string& label,
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
    ImGui::TextDisabled("style");
    ImGui::SetNextItemWidth(-1.0f);
    if (!ImGui::BeginCombo("##style", shown.c_str())) {
        return;
    }
    if (ImGui::Selectable(label.c_str(), active == recipe)) {
        commands.invoke("ai.intent", json{{"recipe", recipe}});
    }
    for (const json& member : variants) {
        const std::string id = member["id"].get<std::string>();
        const std::string name = member["label"].get<std::string>();
        const bool selected = id == active;
        if (ImGui::Selectable(name.c_str(), selected)) {
            commands.invoke("ai.intent", json{{"recipe", id}});
        }
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

void draw_verb_row(CommandTable& commands, const json& intent, const json& verb) {
    const std::string id = verb["id"].get<std::string>();
    const std::string label = verb["label"].is_null() ? id : verb["label"].get<std::string>();
    const std::string reason = verb["reason"].get<std::string>();
    const std::string recipe = verb["recipe"].get<std::string>();
    const json ask = verb["ask"];
    const std::string field =
        ask.is_array() && !ask.empty() ? ask[0].get<std::string>() : std::string();

    ImGui::PushID(id.c_str());
    const bool pressed = draw_verb_button(verb, label, reason);
    if (!field.empty()) {
        draw_verb_field(commands, intent, id, recipe, field);
    }
    draw_aspect_picker(commands, intent, recipe, verb["aspects"]);
    draw_style_picker(commands, verb, label, recipe, verb["variants"]);
    ImGui::PopID();
    if (pressed) {
        activate_verb(commands, verb);
    }
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

void draw_verb_section(CommandTable& commands, const json& intent, io::Theme theme) {
    ImGui::SeparatorText("verbs");
    const json view = commands.invoke("ai.verbs", json::object()).value;
    const std::string table_error = view["error"].get<std::string>();
    if (!table_error.empty()) {
        ImGui::TextColored(ui_theme::palette(theme).danger, "%s", table_error.c_str());
        return;
    }
    for (const json& verb : view["verbs"]) {
        if (verb["scope"].get<std::string>() != "selection") {
            draw_verb_row(commands, intent, verb);
        }
    }
    draw_model_switch_popup(commands);
}

}
