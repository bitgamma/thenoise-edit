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
#include <cmath>
#include <cstdlib>
#include <string>
#include <string_view>
#include <utility>

namespace noise::app {

using nlohmann::json;

namespace {
constexpr float kLayerOpacitySliderPx = 88.0f;
constexpr float kLayerModeMinPx = 64.0f;
constexpr const char* kLabelScale = "Scale";
constexpr float kLayerScaleFieldPx = 64.0f;
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

// A factor off the field's text: a whole number, a decimal, either sign rejected downstream. Anything
// else - "big", "1.5x", empty - is not a factor, and the field keeps what was typed rather than sending a
// zero the command would have to guess about.
bool reads_factor(const char* text, double& factor) {
    char* end = nullptr;
    const double parsed = std::strtod(text, &end);
    if (end == text) {
        return false;
    }
    while (end != nullptr && *end != '\0' && (*end == ' ' || *end == '\t')) {
        ++end;
    }
    if (*end != '\0' || !std::isfinite(parsed) || parsed <= 0.0) {
        return false;
    }
    factor = parsed;
    return true;
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

void draw_layer_title(AppState& state, LayerCard& card, const Layer& layer, int row,
                      int layer_count, float arrows_px, CommandTable& commands) {
    bool visible = layer.visible;
    if (ImGui::Checkbox("##visible", &visible)) {
        commands.invoke("layer.set", json{{"index", row}, {"visible", visible}});
    }
    card.visibility = item_rect();
    ImGui::SameLine();
    // The name stops where the arrows begin, so no pixel belongs to two controls. Its floor is the one
    // thing that can lay them over each other again, in a card too narrow for the arrows and a name
    // nobody could read anyway.
    const float name_px = std::max(
        kLayerNameMinPx,
        ImGui::GetContentRegionAvail().x - arrows_px - ImGui::GetStyle().ItemSpacing.x);
    if (state.renaming.layer == layer.id) {
        // The field takes the name's place rather than sitting over it: two items claiming one box and
        // the point goes to whichever was submitted first. `Enter` commits and anything else drops it, so
        // a name that was never confirmed cannot end up in the file - and while the field holds the
        // keyboard, `Escape` is the field's to cancel with rather than `app.quit`'s to leave with.
        const bool opening = state.renaming.just_opened;
        if (opening) {
            ImGui::SetKeyboardFocusHere(0);
            state.renaming.just_opened = false;
        }
        ImGui::SetNextItemWidth(name_px);
        const bool confirmed =
            ImGui::InputText("##name", state.renaming.text.data(), state.renaming.text.size() - 1,
                             ImGuiInputTextFlags_EnterReturnsTrue);
        card.title = item_rect();
        card.renaming = true;
        const bool escaping = ImGui::IsItemFocused() && ImGui::IsKeyPressed(ImGuiKey_Escape, false);
        if (confirmed) {
            commands.invoke("layer.rename",
                            json{{"index", row}, {"name", state.renaming.text.data()}});
            state.renaming.layer = 0;
        } else if (escaping) {
            state.renaming.layer = 0;
        } else if (!opening && !ImGui::IsItemActive()) {
            // On the frame a field opens, focus has not arrived yet: dropping the edit for its own first
            // frame would close a field nobody ever saw.
            state.renaming.layer = 0;
        }
    } else {
        if (ImGui::Selectable(layer.name.c_str(), card.active, 0, ImVec2(name_px, 0.0f))) {
            commands.invoke("layer.select", json{{"index", row}});
        }
        card.title = item_rect();
        if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
            begin_layer_rename(state, row);
        }
    }
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

void draw_layer_preview(AppState& state, LayerCard& card_state, const Layer& layer, int row,
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

    // The picture can be made bigger or smaller without touching the canvas it sits in, and the way to
    // say by how much is a number: the button opens a field in its own place, `Enter` sends it to
    // `layer.scale`, and a factor that will not fit is refused by the command with the largest one that
    // does.
    ImGui::SameLine();
    if (state.scaling.layer == layer.id) {
        const bool opening = state.scaling.just_opened;
        if (opening) {
            state.scaling.just_opened = false;
            ImGui::SetKeyboardFocusHere();
        }
        ImGui::PushItemWidth(kLayerScaleFieldPx);
        const bool confirmed =
            ImGui::InputText("##scale", state.scaling.text.data(), state.scaling.text.size() - 1,
                             ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::PopItemWidth();
        card_state.scale = item_rect();
        card_state.scaling = true;
        const bool escaping = ImGui::IsItemFocused() && ImGui::IsKeyPressed(ImGuiKey_Escape, false);
        double factor = 0.0;
        if (confirmed && reads_factor(state.scaling.text.data(), factor)) {
            commands.invoke("layer.scale", json{{"index", row}, {"factor", factor}});
            state.scaling = InlineField{};
        } else if (confirmed) {
            // Re-armed rather than let go: a single-line InputText gives up focus on Enter whether or not
            // the flag asked it to, and the key a person reaches for after a refusal is Escape - which
            // with no field holding the keyboard is the app's quit key. The word they typed stays where
            // they typed it.
            log::warn("scale: '" + std::string(state.scaling.text.data()) +
                      "' is not a factor: 2 is twice as big, 0.5 is half");
            state.scaling.just_opened = true;
        } else if (escaping) {
            state.scaling = InlineField{};
        } else if (!opening && !ImGui::IsItemActive()) {
            state.scaling = InlineField{};
        }
    } else {
        if (ImGui::Button(kLabelScale)) {
            begin_layer_scale(state, row);
        }
        card_state.scale = item_rect();
    }
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
    draw_layer_title(state, card, layer, static_cast<int>(row), static_cast<int>(layers.size()),
                     arrows_px, commands);
    ImGui::NewLine();
    draw_layer_preview(state, card, layer, static_cast<int>(row), thumbs, commands);
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
        const auto gone_from_stack = [&layers](std::uint64_t id) {
            return std::find_if(layers.begin(), layers.end(),
                                [id](const Layer& layer) { return layer.id == id; }) == layers.end();
        };
        if (state.renaming.layer != 0 && gone_from_stack(state.renaming.layer)) {
            state.renaming = InlineField{};  // a card that left the stack is not being typed into
        }
        if (state.scaling.layer != 0 && gone_from_stack(state.scaling.layer)) {
            state.scaling = InlineField{};
        }
        // Both arrows take the right end of every title row, so their width is known before the first
        // name of the stack is measured.
        const float arrows_px = button_px(kLabelMoveUp) + ImGui::GetStyle().ItemSpacing.x +
                                button_px(kLabelMoveDown);
        // The stack's own verbs are pinned under the list rather than being its last entry: with three
        // layers they fell past the panel's bottom edge - reported at y=926 in a window ending at 800 -
        // controls for the whole stack, drawn nowhere and reachable only by scrolling to them. What the
        // panel has left after one button row is the list's, and a row is a frame tall plus the gap above
        // it, both from the style; the list carries no padding of its own, so the cards are the width
        // they were.
        const float pinned_row_px = ImGui::GetFrameHeight() + ImGui::GetStyle().ItemSpacing.y;
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2{0.0f, 0.0f});
        ImGui::BeginChild("layer-list", ImVec2(0.0f, -pinned_row_px));
        for (std::size_t row = layers.size(); row-- > 0;) {
            draw_layer_card(state, commands, thumbs, layers, row, live, colors, arrows_px);
        }
        draw_mask_controls(state, commands);
        ImGui::EndChild();
        ImGui::PopStyleVar();
        // The scrolling region: what a card has to be inside to be on screen, and where the pinned row
        // begins.
        state.layer_panel.list = item_rect();
        draw_stack_actions(state, commands, layers.size());
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
