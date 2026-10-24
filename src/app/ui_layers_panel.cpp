// SPDX-License-Identifier: Apache-2.0
#include "app/ui_internal.h"

#include "app/command_args.h"
#include "app/layer_thumbs.h"
#include "app/ui_theme.h"

#include "core/blend.h"
#include "core/log.h"

#include "imgui.h"
#include "sokol_app.h"
#include "sokol_imgui.h"

#include <algorithm>
#include <string>
#include <string_view>
#include <utility>

namespace noise::app {

using nlohmann::json;

namespace {
constexpr float kLayerOpacitySliderPx = 88.0f;
constexpr float kLayerModeMinPx = 64.0f;
constexpr float kLayerCardPaddingX = 8.0f;
constexpr float kLayerCardPaddingY = 4.0f;
constexpr float kLayerCardSpacingX = 6.0f;
constexpr float kLayerCardSpacingY = 5.0f;
constexpr ImU32 kCardShadow = IM_COL32(0, 0, 0, 0);
constexpr float kLayerNameMinPx = 40.0f;
constexpr const char* kLabelMoveUp = "Up";
constexpr const char* kLabelMoveDown = "Down";
constexpr const char* kLabelMergeDown = "Merge down";

// The box a layer's picture is drawn into: the long edge is the thumbnail size, the other follows the
// layer's own proportions, so a wide document's card does not show its picture stretched.
ImVec2 thumb_draw_size(Extent size) {
    const int long_edge = std::max(1, std::max(size.w, size.h));
    const float scale = static_cast<float>(kLayerThumbLongEdge) / static_cast<float>(long_edge);
    return ImVec2(std::max(1.0f, static_cast<float>(size.w) * scale),
                  std::max(1.0f, static_cast<float>(size.h) * scale));
}

// The box of the item just submitted, in screen coordinates - the same box the hit test used.
ScreenRect item_rect() {
    const ImVec2 from = ImGui::GetItemRectMin();
    const ImVec2 to = ImGui::GetItemRectMax();
    return ScreenRect{from.x, from.y, to.x - from.x, to.y - from.y};
}

// A button is its label plus the frame padding on each side, which is `ButtonEx`'s own formula. Measured
// rather than estimated: `ItemHoverable` gives a point to the *first* item submitted under it, so a
// button left overlapping a wider one draws, lights up, and never answers a press.
float button_px(const char* label) {
    return ImGui::CalcTextSize(label).x + ImGui::GetStyle().FramePadding.x * 2.0f;
}

void draw_layer_title(LayerCard& card, const Layer& layer, int row, int layer_count,
                      float arrows_px, CommandTable& commands) {
    bool visible = layer.visible;
    if (ImGui::Checkbox("##visible", &visible)) {
        commands.invoke("layer.set", json{{"index", row}, {"visible", visible}});
    }
    card.visibility = item_rect();
    ImGui::SameLine();
    const float name_px = std::max(
        kLayerNameMinPx,
        ImGui::GetContentRegionAvail().x - arrows_px - ImGui::GetStyle().ItemSpacing.x);
    if (ImGui::Selectable(layer.name.c_str(), card.active, 0, ImVec2(name_px, 0.0f))) {
        commands.invoke("layer.select", json{{"index", row}});
    }
    card.title = item_rect();
    card.can_move_up = card.index + 1 < layer_count;
    card.can_move_down = card.index > 0;
    card.can_merge = card.index > 0;
    ImGui::SameLine();
    ImGui::BeginDisabled(!card.can_move_up);
    if (ImGui::Button(kLabelMoveUp)) {
        commands.invoke("layer.move", json{{"index", row}, {"to", row + 1}});
    }
    ImGui::EndDisabled();
    card.move_up = item_rect();
    ImGui::SameLine();
    ImGui::BeginDisabled(!card.can_move_down);
    if (ImGui::Button(kLabelMoveDown)) {
        commands.invoke("layer.move", json{{"index", row}, {"to", row - 1}});
    }
    ImGui::EndDisabled();
    card.move_down = item_rect();
}

void draw_layer_preview(LayerCard& card_state, const Layer& layer, int row,
                        LayerThumbs& thumbs, CommandTable& commands) {
    const LayerThumbs::Card& preview = thumbs.card(layer.id);
    if (preview.pixels.valid()) {
        ImGui::Image(simgui_imtextureid(preview.pixels.view), thumb_draw_size(layer.pixels.size()));
        card_state.picture = item_rect();
    }
    if (preview.has_mask && preview.mask.valid()) {
        ImGui::SameLine();
        ImGui::Image(simgui_imtextureid(preview.mask.view), thumb_draw_size(layer.mask->size()));
        card_state.mask_picture = item_rect();
    } else if (preview.pixels.valid()) {
        ImGui::SameLine();
        ImGui::TextDisabled("no mask");
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(!card_state.can_merge);
    if (ImGui::Button(kLabelMergeDown)) {
        commands.invoke("layer.merge", json{{"index", row}});
    }
    ImGui::EndDisabled();
    card_state.merge = item_rect();
}

void draw_layer_properties(LayerCard& card, const Layer& layer, int row,
                            CommandTable& commands) {
    float opacity = static_cast<float>(layer.opacity);
    ImGui::PushItemWidth(kLayerOpacitySliderPx);
    if (ImGui::SliderFloat("##opacity", &opacity, 0.0f, 1.0f, "%.2f") &&
        static_cast<double>(opacity) != layer.opacity) {
        commands.invoke("layer.set", json{{"index", row}, {"opacity", static_cast<double>(opacity)}});
    }
    ImGui::PopItemWidth();
    card.opacity = item_rect();
    ImGui::SameLine();
    const std::string_view mode_name = blend_mode_name(layer.mode);
    ImGui::PushItemWidth(std::max(kLayerModeMinPx, ImGui::GetContentRegionAvail().x));
    const bool opening_mode = ImGui::BeginCombo(
        "##mode", mode_name.empty() ? "no such mode" : mode_name.data(), 0);
    card.mode = item_rect();
    if (opening_mode) {
        for (const BlendModeInfo& info : kBlendModes) {
            const bool chosen = info.mode == layer.mode;
            if (ImGui::Selectable(info.name, chosen)) {
                commands.invoke("layer.set", json{{"index", row}, {"mode", info.name}});
            }
            if (chosen) {
                ImGui::SetItemDefaultFocus();
            }
        }
        ImGui::EndCombo();
    }
    ImGui::PopItemWidth();
}

void draw_layer_card(AppState& state, CommandTable& commands, LayerThumbs& thumbs,
                     const std::span<const Layer>& layers, std::size_t row, bool live,
                     const ui_theme::Palette& colors, float arrows_px) {
    const Layer& layer = layers[row];
    thumbs.sync(layer, live);
    const bool active = static_cast<int>(row) == state.doc.active();
    ImGui::PushID(static_cast<int>(row));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, active ? colors.card_selected : colors.card);
    ImGui::PushStyleColor(ImGuiCol_Border,
                          active ? colors.card_selected_border : colors.card_border);
    ImGui::PushStyleColor(ImGuiCol_BorderShadow, kCardShadow);
    LayerCard card;
    card.index = static_cast<int>(row);
    card.name = layer.name;
    card.active = active;
    card.visible = layer.visible;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,
                        ImVec2(kLayerCardPaddingX, kLayerCardPaddingY));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,
                        ImVec2(kLayerCardSpacingX, kLayerCardSpacingY));
    ImGui::BeginChild("card", ImVec2(0.0f, 0.0f),
                      ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY);
    draw_layer_title(card, layer, static_cast<int>(row), static_cast<int>(layers.size()),
                     arrows_px, commands);
    ImGui::NewLine();
    draw_layer_preview(card, layer, static_cast<int>(row), thumbs, commands);
    ImGui::NewLine();
    draw_layer_properties(card, layer, static_cast<int>(row), commands);
    ImGui::EndChild();
    ImGui::PopStyleVar(2);
    const ImVec2 card_min = ImGui::GetItemRectMin();
    const ImVec2 card_max = ImGui::GetItemRectMax();
    card.box = ScreenRect{card_min.x, card_min.y, card_max.x - card_min.x,
                          card_max.y - card_min.y};
    state.layer_cards.push_back(std::move(card));
    ImGui::PopStyleColor(3);
    ImGui::PopID();
}

void draw_stack_actions(AppState& state, CommandTable& commands, std::size_t layer_count) {
    if (ImGui::Button("Add layer")) {
        commands.invoke("layer.add", json::object());
    }
    state.layer_panel.add = item_rect();
    ImGui::SameLine();
    if (ImGui::Button("Delete layer")) {
        commands.invoke("layer.remove", json::object());
    }
    state.layer_panel.remove = item_rect();
    ImGui::SameLine();
    state.layer_panel.can_flatten = layer_count > 1;
    ImGui::BeginDisabled(!state.layer_panel.can_flatten);
    if (ImGui::Button("Flatten")) {
        commands.invoke("layer.flatten", json::object());
    }
    ImGui::EndDisabled();
    state.layer_panel.flatten = item_rect();
}

void draw_mask_controls(AppState& state, CommandTable& commands) {
    const Layer& active = state.doc.layer(static_cast<std::size_t>(state.doc.active()));
    ImGui::SeparatorText("mask");
    const json facts = active.mask.has_value() ? layer_mask_json(active) : json(nullptr);
    state.layer_panel.has_mask = !facts.is_null();
    if (facts.is_null()) {
        if (ImGui::Button("Add mask")) {
            commands.invoke("mask.add", json::object());
        }
        state.layer_panel.mask_add = item_rect();
        ImGui::SameLine();
        ImGui::TextDisabled("nothing hidden");
        return;
    }
    if (ImGui::Button("Remove")) {
        commands.invoke("mask.remove", json::object());
    }
    state.layer_panel.mask_remove = item_rect();
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
    const json& bounds = facts["bounds"];
    ImGui::TextDisabled("%d px touched, %.1f px shown",
                        static_cast<int>(facts["covered"].get<std::int64_t>()),
                        facts["area"].get<double>());
    ImGui::TextDisabled("%d x %d at %d,%d", bounds[2].get<int>(), bounds[3].get<int>(),
                        bounds[0].get<int>(), bounds[1].get<int>());
}

}

void draw_layers_panel(AppState& state, CommandTable& commands, LayerThumbs& thumbs) {
    ImGui::Begin(kWindowLayers);
    const ui_theme::Palette& colors = ui_theme::palette(state.ui_theme);
    state.layer_cards.clear();
    state.layer_panel = LayerPanelControls{};
    const ImVec2 panel_pos = ImGui::GetWindowPos();
    const ImVec2 panel_size = ImGui::GetWindowSize();
    state.layers_window = ScreenRect{panel_pos.x, panel_pos.y, panel_size.x, panel_size.y};
    if (state.doc.empty()) {
        ImGui::TextUnformatted("no document");
    } else {
        const std::span<const Layer> layers = state.doc.layers();
        const bool live = state.doc.in_provisional();
        thumbs.keep(layers);
        const float arrows_px = button_px(kLabelMoveUp) + ImGui::GetStyle().ItemSpacing.x +
                                button_px(kLabelMoveDown);
        for (std::size_t row = layers.size(); row-- > 0;) {
            draw_layer_card(state, commands, thumbs, layers, row, live, colors, arrows_px);
        }
        draw_stack_actions(state, commands, layers.size());
        draw_mask_controls(state, commands);
    }
    state.layer_thumb_rebuilds = thumbs.rebuilds;
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

}
