// SPDX-License-Identifier: Apache-2.0
#include "app/ui_internal.h"

#include "app/ui_theme.h"
#include "app/ui_icons.h"

#include "imgui.h"
#include "sokol_app.h"
#include "sokol_imgui.h"

#include <algorithm>
#include <string>

namespace noise::app {
namespace {

using nlohmann::json;

ScreenRect item_rect() {
    const ImVec2 from = ImGui::GetItemRectMin();
    const ImVec2 to = ImGui::GetItemRectMax();
    const ImVec2 clip_min = ImGui::GetWindowDrawList()->GetClipRectMin();
    const ImVec2 clip_max = ImGui::GetWindowDrawList()->GetClipRectMax();
    const float left = std::max(from.x, clip_min.x);
    const float top = std::max(from.y, clip_min.y);
    const float right = std::min(to.x, clip_max.x);
    const float bottom = std::min(to.y, clip_max.y);
    return ScreenRect{left, top, std::max(0.0f, right - left),
                      std::max(0.0f, bottom - top)};
}

const AiReference* reference_by_id(const AppState& state, const std::string& id) {
    for (const AiReference& reference : state.ai.references) {
        if (reference.id == id) {
            return &reference;
        }
    }
    return nullptr;
}

void draw_primary(const json& primary, const ui_theme::Palette& colors) {
    if (primary.is_null()) {
        ImGui::TextDisabled("Open an image to set the primary target");
        return;
    }
    ImGui::TextColored(colors.accent, "[1]");
    ImGui::SameLine();
    ImGui::TextUnformatted(primary["name"].get<std::string>().c_str());
    ImGui::SameLine();
    ImGui::TextDisabled("Base");
}

void draw_reference_row(AppState& state, CommandTable& commands, ReferenceThumbs& thumbs,
                        const json& row, std::size_t index, int count,
                        const ui_theme::Palette& colors) {
    const std::string id = row["id"].get<std::string>();
    const AiReference* reference = reference_by_id(state, id);
    ImGui::PushID(id.c_str());
    if (reference != nullptr) {
        const gfx::Texture& texture = thumbs.sync(*reference);
        if (texture.valid()) {
            ImGui::Image(simgui_imtextureid(texture.view),
                         ImVec2(static_cast<float>(reference->thumbnail.size().w),
                                static_cast<float>(reference->thumbnail.size().h)));
        }
        ImGui::SameLine();
    }
    ImGui::BeginGroup();
    ImGui::TextColored(colors.accent, "[%d]", row["number"].get<int>());
    ImGui::SameLine();
    ImGui::TextUnformatted(row["name"].get<std::string>().c_str());
    if (row["source"].get<std::string>() == "layer" && !row["linked"].get<bool>()) {
        ImGui::SameLine();
        ImGui::TextDisabled("snapshot");
    }
    ImGui::TextDisabled("%d × %d", row["dimensions"][0].get<int>(),
                        row["dimensions"][1].get<int>());
    if (!row["error"].get<std::string>().empty()) {
        ImGui::TextColored(colors.danger, "%s", row["error"].get<std::string>().c_str());
    }
    ImGui::EndGroup();
    ImGui::BeginDisabled(index == 0);
    if (ui_icons::button(ui_icons::Icon::ArrowUp, "ref_up", ImVec2(25.0f, 24.0f), "Move reference up")) {
        commands.invoke("ai.references.move", json{{"id", id}, {"to", static_cast<int>(index) - 1}});
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(index + 1 >= static_cast<std::size_t>(count));
    if (ui_icons::button(ui_icons::Icon::ArrowDown, "ref_down", ImVec2(25.0f, 24.0f), "Move reference down")) {
        commands.invoke("ai.references.move", json{{"id", id}, {"to", static_cast<int>(index) + 1}});
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ui_icons::button(ui_icons::Icon::Trash, "ref_remove", ImVec2(25.0f, 24.0f), "Remove reference")) {
        commands.invoke("ai.references.remove", json{{"id", id}});
    }
    ImGui::PopID();
}

void add_from_layer(AppState& state, CommandTable& commands) {
    if (state.doc.empty()) {
        return;
    }
    const auto layers = state.doc.layers();
    if (!ImGui::BeginCombo("+ From layer", "Choose a layer")) {
        return;
    }
    for (std::size_t i = layers.size(); i-- > 0;) {
        const Layer& layer = layers[i];
        std::string label = layer.name;
        if (!layer.visible) {
            label += " (hidden)";
        }
        if (ImGui::Selectable(label.c_str())) {
            commands.invoke("ai.references.add", json{{"layer", static_cast<int>(i)}});
        }
    }
    ImGui::EndCombo();
}

}  // namespace

void draw_ai_references(AppState& state, CommandTable& commands, ReferenceThumbs& thumbs,
                        const json& intent) {
    const ui_theme::Palette& colors = ui_theme::palette(state.ui_theme);
    const json view = commands.invoke("ai.references", json::object()).value;
    thumbs.keep(state.ai.references);
    ImGui::BeginGroup();
    ImGui::SeparatorText("AI References");
    ImGui::SameLine();
    ImGui::TextDisabled("session only");
    draw_primary(view["primary"], colors);
    for (std::size_t i = 0; i < view["references"].size(); ++i) {
        draw_reference_row(state, commands, thumbs, view["references"][i], i,
                           static_cast<int>(view["references"].size()), colors);
    }
    if (!view["references"].empty()) {
        bool include = intent["include_references"].get<bool>();
        if (ImGui::Checkbox("Send additional references", &include)) {
            commands.invoke("ai.intent", json{{"include_references", include}});
        }
        const std::string status =
            intent["additional_references"]["status"].get<std::string>();
        if (status == "unsupported_endpoint") {
            ImGui::TextDisabled("This endpoint cannot accept additional references");
        } else if (include) {
            ImGui::TextDisabled("%d additional image(s) follow Image 1 in this request",
                                static_cast<int>(view["references"].size()));
        } else {
            ImGui::TextDisabled("%d configured reference(s) will not be sent",
                                static_cast<int>(view["references"].size()));
        }
    }
    if (view["references"].empty()) {
        ImGui::TextDisabled("No additional images · drop files here");
    } else {
        ImGui::TextDisabled("Drop more files here");
    }
    ImGui::BeginDisabled(state.doc.empty());
    if (ui_icons::labeled_button(ui_icons::Icon::ImagePlus, "Add images", ImVec2(0.0f, 0.0f))) {
        commands.invoke("file.pick", json{{"for", "reference"},
                                           {"title", "Add AI reference images"},
                                           {"multiple", true}});
    }
    add_from_layer(state, commands);
    ImGui::EndDisabled();
    if (!view["error"].get<std::string>().empty()) {
        ImGui::TextColored(colors.danger, "%s", view["error"].get<std::string>().c_str());
    }
    ImGui::EndGroup();
    const ScreenRect bounds = item_rect();
    state.ai_references_rect = bounds;
}

}  // namespace noise::app
