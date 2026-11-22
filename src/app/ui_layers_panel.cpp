// SPDX-License-Identifier: Apache-2.0
#include "app/ui_internal.h"
#include "app/ui_icons.h"

#include "app/command_args.h"
#include "app/layer_thumbs.h"
#include "app/ui_theme.h"

#include "core/blend.h"
#include "core/log.h"

#include "imgui.h"
#include "imgui_internal.h"
#include "sokol_app.h"
#include "sokol_imgui.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <string>
#include <string_view>
#include <utility>

namespace noise::app {

using nlohmann::json;

namespace {
constexpr float kLayerOpacitySliderPx = 88.0f;
constexpr float kLayerModeMinPx = 48.0f;
constexpr float kLayerScaleFieldPx = 64.0f;
constexpr float kLayerCardPaddingX = 6.0f;
constexpr float kLayerCardPaddingY = 3.0f;
constexpr float kLayerCardSpacingX = 4.0f;
constexpr float kLayerCardSpacingY = 2.0f;
constexpr ImU32 kCardShadow = IM_COL32(0, 0, 0, 0);
constexpr float kLayerNameMinPx = 32.0f;
constexpr float kLayerActionPx = 22.0f;
constexpr float kMaskActionPx = 26.0f;
constexpr float kLayerSplitterPx = 6.0f;
constexpr float kLayersMinimumWidthPx = 280.0f;

// The box a layer's picture is drawn into: the long edge is the thumbnail size, the other follows the
// layer's own proportions, so a wide document's card does not show its picture stretched.
ImVec2 thumb_draw_size(Extent size) {
    const int long_edge = std::max(1, std::max(size.w, size.h));
    const float scale = static_cast<float>(kLayerThumbLongEdge) / static_cast<float>(long_edge);
    return ImVec2(std::max(1.0f, static_cast<float>(size.w) * scale),
                  std::max(1.0f, static_cast<float>(size.h) * scale));
}

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

void draw_layer_title(AppState& state, LayerCard& card, const Layer& layer, int row,
                      int layer_count, float arrows_px, CommandTable& commands) {
    if (ui_icons::button(layer.visible ? ui_icons::Icon::Eye : ui_icons::Icon::EyeOff,
                         "visible", ImVec2(kLayerActionPx, kLayerActionPx),
                         layer.visible ? "Hide layer" : "Show layer")) {
        commands.invoke("layer.set", json{{"index", row}, {"visible", !layer.visible}});
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
    if (ui_icons::button(ui_icons::Icon::ArrowUp, "up", ImVec2(kLayerActionPx, kLayerActionPx), "Move layer up")) {
        commands.invoke("layer.move", json{{"index", row}, {"to", row + 1}});
    }
    ImGui::EndDisabled();
    card.move_up = item_rect();
    ImGui::SameLine();
    ImGui::BeginDisabled(!card.can_move_down);
    if (ui_icons::button(ui_icons::Icon::ArrowDown, "down", ImVec2(kLayerActionPx, kLayerActionPx), "Move layer down")) {
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
        if (ImGui::IsItemClicked()) {
            commands.invoke("layer.plane", json{{"index", row}, {"plane", "pixels"}});
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
            ImGui::SetTooltip("Edit image pixels (Brush / Eraser)");
        }
        if (card_state.editing_pixels) {
            ImGui::GetWindowDrawList()->AddRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(),
                ImGui::ColorConvertFloat4ToU32(ui_theme::palette(state.ui_theme).accent),
                2.0f, 0, 2.0f);
        }
    }
    if (preview.has_mask && preview.mask.valid()) {
        ImGui::SameLine();
        ImGui::Image(simgui_imtextureid(preview.mask.view), thumb_draw_size(layer.mask->size()));
        card_state.mask_picture = item_rect();
        if (ImGui::IsItemClicked()) {
            commands.invoke("layer.plane", json{{"index", row}, {"plane", "mask"}});
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
            ImGui::SetTooltip("Edit layer mask: Brush reveals, Eraser hides");
        }
        if (card_state.editing_mask) {
            ImGui::GetWindowDrawList()->AddRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(),
                                                IM_COL32(235, 95, 95, 255), 0.0f, 0, 2.0f);
        }
    } else if (preview.pixels.valid()) {
        ImGui::SameLine();
        ImGui::TextDisabled("no mask");
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(!card_state.can_merge);
    if (ui_icons::button(ui_icons::Icon::Merge, "merge", ImVec2(kLayerActionPx, kLayerActionPx), "Merge down")) {
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
        if (ui_icons::button(ui_icons::Icon::Scaling, "scale_button", ImVec2(kLayerActionPx, kLayerActionPx), "Scale layer")) {
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
    card.editing_pixels = active && state.edit_plane == LayerPlane::pixels;
    card.editing_mask = active && state.editing_mask();
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,
                        ImVec2(kLayerCardPaddingX, kLayerCardPaddingY));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,
                        ImVec2(kLayerCardSpacingX, kLayerCardSpacingY));
    ImGui::BeginChild("card", ImVec2(0.0f, 0.0f),
                      ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY);
    draw_layer_title(state, card, layer, static_cast<int>(row), static_cast<int>(layers.size()),
                     arrows_px, commands);
    draw_layer_preview(state, card, layer, static_cast<int>(row), thumbs, commands);
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
    if (ui_icons::button(ui_icons::Icon::LayersPlus, "add", ImVec2(kLayerActionPx, kLayerActionPx), "Add layer")) {
        commands.invoke("layer.add", json::object());
    }
    state.layer_panel.add = item_rect();
    ImGui::SameLine();
    if (ui_icons::button(ui_icons::Icon::Trash, "remove", ImVec2(kLayerActionPx, kLayerActionPx), "Delete layer")) {
        commands.invoke("layer.remove", json::object());
    }
    state.layer_panel.remove = item_rect();
    ImGui::SameLine();
    state.layer_panel.can_flatten = layer_count > 1;
    ImGui::BeginDisabled(!state.layer_panel.can_flatten);
    if (ui_icons::labeled_button(ui_icons::Icon::Layers2, "Flatten", ImVec2(0.0f, kLayerActionPx))) {
        commands.invoke("layer.flatten", json::object());
    }
    ImGui::EndDisabled();
    state.layer_panel.flatten = item_rect();
}

void draw_mask_controls(AppState& state, CommandTable& commands) {
    const Layer& active = state.doc.layer(static_cast<std::size_t>(state.doc.active()));
    const json facts = active.mask.has_value() ? layer_mask_json(active) : json(nullptr);
    state.layer_panel.has_mask = !facts.is_null();
    ImGui::SetNextItemOpen(true, ImGuiCond_Once);
    if (!ImGui::CollapsingHeader("Mask")) {
        return;
    }
    if (facts.is_null()) {
        if (ui_icons::button(ui_icons::Icon::MaskAdd, "mask_add",
                             ImVec2(kMaskActionPx, kMaskActionPx), "Add mask")) {
            commands.invoke("mask.add", json::object());
        }
        state.layer_panel.mask_add = item_rect();
        ImGui::SameLine();
        ImGui::TextDisabled("No mask");
    } else {
        if (ui_icons::button(ui_icons::Icon::Eye, "reveal_all",
                             ImVec2(kMaskActionPx, kMaskActionPx), "Reveal all")) {
            commands.invoke("mask.fill", json{{"to", "show"}});
        }
        ImGui::SameLine();
        if (ui_icons::button(ui_icons::Icon::EyeOff, "hide_all",
                             ImVec2(kMaskActionPx, kMaskActionPx), "Hide all")) {
            commands.invoke("mask.fill", json{{"to", "hide"}});
        }
        ImGui::SameLine();
        if (ui_icons::button(ui_icons::Icon::RefreshCw, "invert",
                             ImVec2(kMaskActionPx, kMaskActionPx), "Invert mask")) {
            commands.invoke("mask.invert", json::object());
        }
        ImGui::SameLine();
        const ui_theme::Palette& colors = ui_theme::palette(state.ui_theme);
        ImGui::PushStyleColor(ImGuiCol_Button, colors.danger);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, colors.danger);
        if (ui_icons::button(ui_icons::Icon::MaskRemove, "remove_mask",
                             ImVec2(kMaskActionPx, kMaskActionPx), "Remove mask")) {
            commands.invoke("mask.remove", json::object());
        }
        ImGui::PopStyleColor(2);
        state.layer_panel.mask_remove = item_rect();
        bool overlay = state.mask_overlay_visible;
        if (ImGui::Checkbox("Show hidden area in red", &overlay)) {
            commands.invoke("view.mask_overlay", json{{"show", overlay}});
        }
    }

    ImGui::SetNextItemOpen(false, ImGuiCond_Once);
    if (!ImGui::CollapsingHeader("Selection / Mask")) {
        return;
    }
    const bool has_selection = state.doc.selection().has_value();
    ImGui::BeginDisabled(!has_selection);
    if (ui_icons::button(ui_icons::Icon::Contrast, "mask_from_selection",
                         ImVec2(kMaskActionPx, kMaskActionPx), "Make mask from selection")) {
        commands.invoke("mask.from_selection", json::object());
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!active.mask.has_value());
    if (ui_icons::button(ui_icons::Icon::SquareDashed, "selection_from_mask",
                         ImVec2(kMaskActionPx, kMaskActionPx), "Make selection from mask")) {
        commands.invoke("select.from_mask", json::object());
    }
    ImGui::EndDisabled();
}

}

void draw_layers_panel(AppState& state, CommandTable& commands, LayerThumbs& thumbs) {
    ImGui::SetNextWindowSizeConstraints(ImVec2(kLayersMinimumWidthPx, 0.0f),
                                        ImVec2(std::numeric_limits<float>::max(),
                                               std::numeric_limits<float>::max()));
    ImGuiWindowClass dock_class{};
    dock_class.DockNodeFlagsOverrideSet = ImGuiDockNodeFlags_NoWindowMenuButton;
    ImGui::SetNextWindowClass(&dock_class);
    ImGui::Begin(kWindowLayers);
    const ui_theme::Palette& colors = ui_theme::palette(state.ui_theme);
    state.layer_cards.clear();
    state.layer_panel = LayerPanelControls{};
    const ImVec2 panel_pos = ImGui::GetWindowPos();
    const ImVec2 panel_size = ImGui::GetWindowSize();
    state.layers_window = ScreenRect{panel_pos.x, panel_pos.y, panel_size.x, panel_size.y};
    const std::span<const Layer> layers = state.doc.layers();
    float upper_height = 0.0f;
    float lower_height = 0.0f;
    const ImVec2 content_origin = ImGui::GetCursorScreenPos();
    const ImVec2 content_size = ImGui::GetContentRegionAvail();
    const float item_gap = ImGui::GetStyle().ItemSpacing.y;
    const float pane_height = std::max(
        0.0f, content_size.y - kLayerSplitterPx - item_gap * 2.0f);
    upper_height = layer_panel_upper_height(pane_height, state.layer_panel_upper_ratio);
    lower_height = std::max(0.0f, pane_height - upper_height);
    ImGui::SetCursorScreenPos(ImVec2(content_origin.x,
                                     content_origin.y + upper_height + item_gap));
    draw_layer_panel_splitter(state.layer_panel_upper_ratio, content_size.x,
                              kLayerSplitterPx, upper_height, lower_height);
    const ImVec2 lower_origin(content_origin.x,
                              content_origin.y + upper_height + item_gap +
                                  kLayerSplitterPx + item_gap);

    ImGui::SetCursorScreenPos(content_origin);
    if (ImGui::BeginChild("layer-stack-region", ImVec2(0.0f, upper_height))) {
        if (state.doc.empty()) {
            ImGui::TextDisabled("No document");
        } else {
        const std::vector<DocumentVersionView> versions = state.doc.versions();
        if (versions.size() > 1) {
            const auto active = std::find_if(versions.begin(), versions.end(),
                                             [](const auto& version) { return version.current; });
            const std::string label = active->name + " (" +
                std::to_string(active->layers.front().pixels.size().w) + " x " +
                std::to_string(active->layers.front().pixels.size().h) + ")";
            ImGui::SetNextItemWidth(-1.0f);
            if (ImGui::BeginCombo("##document_version", label.c_str())) {
                for (const DocumentVersionView& version : versions) {
                    const Extent size = version.layers.front().pixels.size();
                    const std::string option = version.name + " (" +
                        std::to_string(size.w) + " x " + std::to_string(size.h) +
                        ")##" + std::to_string(version.id);
                    if (ImGui::Selectable(option.c_str(), version.current) && !version.current) {
                        commands.invoke("doc.version.select", json{{"id", version.id}});
                    }
                }
                ImGui::EndCombo();
            }
        }
        const bool live = state.doc.in_provisional();
        thumbs.keep(layers);
        const auto gone_from_stack = [&layers](std::uint64_t id) {
            return std::find_if(layers.begin(), layers.end(),
                                [id](const Layer& layer) { return layer.id == id; }) == layers.end();
        };
        if (state.renaming.layer != 0 && gone_from_stack(state.renaming.layer)) {
            state.renaming = InlineField{};
        }
        if (state.scaling.layer != 0 && gone_from_stack(state.scaling.layer)) {
            state.scaling = InlineField{};
        }
        const float arrows_px = kLayerActionPx * 2.0f + ImGui::GetStyle().ItemSpacing.x;
        const float list_height = std::max(
            0.0f, ImGui::GetContentRegionAvail().y - ImGui::GetFrameHeight() -
                      ImGui::GetStyle().ItemSpacing.y);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2{0.0f, 0.0f});
        ImGui::BeginChild("layer-list", ImVec2(0.0f, list_height), ImGuiChildFlags_None);
        for (std::size_t row = layers.size(); row-- > 0;) {
            draw_layer_card(state, commands, thumbs, layers, row, live, colors, arrows_px);
        }
        ImGui::EndChild();
        ImGui::PopStyleVar();
        state.layer_panel.list = item_rect();
        draw_stack_actions(state, commands, layers.size());
        }
    }
    ImGui::EndChild();
    ImGui::SetCursorScreenPos(lower_origin);
    if (ImGui::BeginChild("layer-controls-region", ImVec2(0.0f, lower_height))) {
        if (!state.doc.empty()) {
            draw_mask_controls(state, commands);
            draw_mask_edge_controls(state, commands);
        }
        draw_tool_settings(state, commands);
    }
    ImGui::EndChild();
    state.layer_thumb_rebuilds = thumbs.rebuilds;
    ImGui::End();
}

}
