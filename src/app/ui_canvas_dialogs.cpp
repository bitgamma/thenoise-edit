// SPDX-License-Identifier: Apache-2.0
#include "app/ui_internal.h"
#include "app/ui_dialog_fields.h"

#include "app/ui_theme.h"

#include "imgui.h"

#include <array>
#include <cstdint>
#include <string>

namespace noise::app {

using nlohmann::json;

namespace {

constexpr float kAnchorButtonPx = 22.0f;
constexpr int kAnchorColumns = 3;
constexpr int kAnchorCount = kAnchorColumns * kAnchorColumns;
constexpr int kLastAnchorColumn = kAnchorColumns - 1;

void initialize_canvas_form(AppState& state) {
    CanvasForm& form = state.canvas_form;
    const Extent source = state.doc.size();
    if (form.source == source && form.source_revision == state.doc.geometry_revision()) return;
    reset_canvas_dialog(state);
}

}

void reset_canvas_dialog(AppState& state) {
    CanvasForm& form = state.canvas_form;
    const Extent source = state.doc.size();
    form.source = source;
    form.source_revision = state.doc.geometry_revision();
    form.width = source.w;
    form.height = source.h;
    form.anchor = 4;
    form.mark_added = true;
    form.crop = Rect{0, 0, source.w, source.h};
    form.error.clear();
}

namespace {

void draw_anchor_picker(CanvasForm& form, io::Theme theme) {
    constexpr const char* kAnchorNames[]{"top left", "top", "top right", "left", "centre",
                                         "right", "bottom left", "bottom", "bottom right"};
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Anchor");
    ImGui::TableSetColumnIndex(1);
    if (!ImGui::BeginTable("##canvas_anchor", kAnchorColumns, ImGuiTableFlags_SizingFixedFit |
                                                    ImGuiTableFlags_NoSavedSettings)) {
        return;
    }
    for (int anchor = 0; anchor < kAnchorCount; ++anchor) {
        ImGui::TableNextColumn();
        ImGui::PushID(anchor);
        const bool selected = form.anchor == anchor;
        if (selected) {
            ImGui::PushStyleColor(ImGuiCol_Button, ui_theme::palette(theme).accent);
        }
        const bool clicked = ImGui::Button("##anchor", ImVec2(kAnchorButtonPx, kAnchorButtonPx));
        if (selected) ImGui::PopStyleColor();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
            ImGui::SetTooltip("%s", kAnchorNames[anchor]);
        }
        if (clicked) form.anchor = anchor;
        ImGui::PopID();
    }
    ImGui::EndTable();
}

bool canvas_size_valid(const CanvasForm& form) {
    return form.width > 0 && form.height > 0 &&
        static_cast<std::int64_t>(form.width) * form.height <= kMaxCanvasPixels;
}

bool resize_canvas(AppState& state, CommandTable& commands, bool fill) {
    CanvasForm& form = state.canvas_form;
    const Extent old = state.doc.size();
    if (!canvas_size_valid(form)) {
        form.error = "Canvas must be positive and at most " +
                     std::to_string(kMaxCanvasPixels) + " pixels";
        return false;
    }
    const int column = form.anchor % kAnchorColumns;
    const int row = form.anchor / kAnchorColumns;
    const int dx = column == 0 ? 0 : column == kLastAnchorColumn ? form.width - old.w
                                                 : (form.width - old.w) / 2;
    const int dy = row == 0 ? 0 : row == kLastAnchorColumn ? form.height - old.h
                                           : (form.height - old.h) / 2;
    const CommandResult result = fill
        ? commands.invoke("doc.outpaint.prepare",
                          json{{"width", form.width}, {"height", form.height},
                               {"offset_x", dx}, {"offset_y", dy}})
        : commands.invoke("doc.resize",
                          json{{"width", form.width}, {"height", form.height},
                               {"offset_x", dx}, {"offset_y", dy},
                               {"mark_added", form.mark_added}});
    form.error = result.ok ? "" : result.error;
    return result.ok;
}

void draw_crop_rectangle(CanvasForm& form) {
    if (!dialog_fields::begin_dimensions("##crop_dimensions")) return;
    const std::array<const char*, 4> labels{"X", "Y", "Width", "Height"};
    int* values[]{&form.crop.x, &form.crop.y, &form.crop.w, &form.crop.h};
    for (std::size_t i = 0; i < labels.size(); ++i) {
        ImGui::PushID(static_cast<int>(i));
        dialog_fields::dimension_input(labels[i], "##crop_field", *values[i]);
        ImGui::PopID();
    }
    ImGui::EndTable();
}

}

void draw_canvas_size_dialog(AppState& state, CommandTable& commands) {
    initialize_canvas_form(state);
    CanvasForm& form = state.canvas_form;
    ImGui::TextDisabled("Resize the canvas without scaling the picture");
    if (dialog_fields::begin_dimensions("##canvas_dimensions")) {
        dialog_fields::dimension_input("Width", "##canvas_width", form.width);
        dialog_fields::dimension_input("Height", "##canvas_height", form.height);
        draw_anchor_picker(form, state.ui_theme);
        ImGui::EndTable();
    }
    ImGui::Checkbox("Select added area for AI Fill", &form.mark_added);
    if (ImGui::Button("Resize Canvas")) {
        if (resize_canvas(state, commands, false)) close_image_dialog();
    }
    ImGui::SameLine();
    if (ImGui::Button("Resize & Fill...")) {
        if (resize_canvas(state, commands, true)) close_image_dialog();
    }
    ImGui::TextDisabled("Fill opens a prompt; Run starts the AI request.");
    if (!form.error.empty()) ImGui::TextWrapped("%s", form.error.c_str());
    ImGui::Separator();
    if (ImGui::Button("Cancel")) close_image_dialog();
}

void draw_crop_dialog(AppState& state, CommandTable& commands) {
    initialize_canvas_form(state);
    CanvasForm& form = state.canvas_form;
    draw_crop_rectangle(form);
    const bool valid_rectangle = !form.crop.empty() && form.crop.inside(state.doc.size());
    ImGui::BeginDisabled(!valid_rectangle);
    if (ImGui::Button("Crop to Rectangle")) {
        const CommandResult result = commands.invoke("doc.crop", json{{"rect", json::array(
            {form.crop.x, form.crop.y, form.crop.w, form.crop.h})}});
        form.error = result.ok ? "" : result.error;
        if (result.ok) close_image_dialog();
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!state.doc.selection().has_value());
    if (ImGui::Button("Crop to Selection")) {
        const CommandResult result = commands.invoke("doc.crop", json::object());
        form.error = result.ok ? "" : result.error;
        if (result.ok) close_image_dialog();
    }
    ImGui::EndDisabled();
    if (!form.error.empty()) ImGui::TextWrapped("%s", form.error.c_str());
    ImGui::Separator();
    if (ImGui::Button("Cancel")) close_image_dialog();
}

}
