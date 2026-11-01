// SPDX-License-Identifier: Apache-2.0
#include "app/ui_internal.h"

#include "app/brush_steps.h"
#include "app/command_args.h"
#include "app/ui_theme.h"
#include "core/maskops.h"
#include "imgui.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <string>
#include <string_view>

namespace noise::app {

using nlohmann::json;

namespace {
struct BrushSlider {
    const char* label;
    const double Brush::*member;
};

void draw_tool_picker(AppState& state, CommandTable& commands) {
    for (const ToolInfo& tool : kTools) {
        const std::string label = std::string(tool.id) + "###" + tool.id;
        if (ImGui::Selectable(label.c_str(), state.tool == tool.id)) {
            commands.invoke(std::string("tool.") + tool.id, json::object());
        }
        ImGui::SameLine();
        ImGui::TextDisabled("[%s]", tool.select_key);
    }
}

void draw_coverage_operations(CommandTable& commands) {
    if (ImGui::Button("All")) {
        commands.invoke("select.all", json::object());
    }
    ImGui::SameLine();
    if (ImGui::Button("None")) {
        commands.invoke("select.clear", json::object());
    }
    ImGui::SameLine();
    if (ImGui::Button("Invert")) {
        commands.invoke("select.invert", json::object());
    }
}

void draw_coverage_transfer(AppState& state, CommandTable& commands, const json& selection) {
    if (state.doc.empty()) {
        return;
    }
    const Layer& active = state.active_layer();
    ImGui::BeginDisabled(!selection["active"].get<bool>());
    if (ImGui::Button("To layer mask")) {
        commands.invoke("mask.from_selection", json::object());
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!active.mask.has_value());
    if (ImGui::Button("From layer mask")) {
        commands.invoke("select.from_mask", json::object());
    }
    ImGui::EndDisabled();
    if (active.mask) {
        ImGui::TextDisabled("mask on '%s': %d px shown", active.name.c_str(),
                            static_cast<int>(active.mask->covered()));
    }
}

void draw_canvas_controls(AppState& state, CommandTable& commands) {
    if (!ImGui::CollapsingHeader("Canvas size / Crop")) {
        return;
    }
    if (state.doc.empty()) {
        ImGui::TextDisabled("Open an image first");
        return;
    }
    CanvasForm& form = state.canvas_form;
    const Extent old = state.doc.size();
    if (form.source != old || form.source_revision != state.doc.geometry_revision()) {
        form.source = old;
        form.source_revision = state.doc.geometry_revision();
        form.width = old.w;
        form.height = old.h;
        form.crop = Rect{0, 0, old.w, old.h};
        form.error.clear();
    }
    ImGui::TextDisabled("Resize canvas without scaling the picture");
    ImGui::SetNextItemWidth(110.0f);
    ImGui::InputInt("Width##canvas", &form.width);
    ImGui::SetNextItemWidth(110.0f);
    ImGui::InputInt("Height##canvas", &form.height);
    constexpr const char* kAnchors[]{"top left", "top", "top right", "left", "centre",
                                     "right", "bottom left", "bottom", "bottom right"};
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::Combo("Anchor##canvas", &form.anchor, kAnchors, 9);
    ImGui::Checkbox("Select added area for AI Fill", &form.mark_added);
    const auto resize = [&](bool fill) {
        if (form.width <= 0 || form.height <= 0 ||
            static_cast<std::int64_t>(form.width) * form.height > kMaxCanvasPixels) {
            form.error = "Canvas must be positive and at most " +
                         std::to_string(kMaxCanvasPixels) + " pixels";
            return;
        }
        const int column = form.anchor % 3;
        const int row = form.anchor / 3;
        const int dx = column == 0 ? 0 : column == 2 ? form.width - old.w
                                                      : (form.width - old.w) / 2;
        const int dy = row == 0 ? 0 : row == 2 ? form.height - old.h
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
    };
    if (ImGui::Button("Resize canvas")) {
        resize(false);
    }
    ImGui::SameLine();
    if (ImGui::Button("Resize & Fill...")) {
        resize(true);
    }
    ImGui::TextDisabled("Fill opens a prompt; Run starts the AI request.");
    ImGui::SeparatorText("Crop");
    std::array<int, 4> crop{form.crop.x, form.crop.y, form.crop.w, form.crop.h};
    if (ImGui::InputInt4("x, y, w, h##crop", crop.data())) {
        form.crop = Rect{crop[0], crop[1], crop[2], crop[3]};
    }
    if (ImGui::Button("Crop to rectangle")) {
        const auto result = commands.invoke("doc.crop", json{{"rect", json::array(
            {form.crop.x, form.crop.y, form.crop.w, form.crop.h})}});
        form.error = result.ok ? "" : result.error;
    }
    ImGui::SameLine();
    if (ImGui::Button("Crop to selection")) {
        const auto result = commands.invoke("doc.crop", json::object());
        form.error = result.ok ? "" : result.error;
    }
    if (!form.error.empty()) {
        ImGui::TextWrapped("%s", form.error.c_str());
    }
}

// One absolute value, shown both on a slider and on pixel-precise +/- buttons.
// A click commits immediately; a drag shows live and records at most one history step on release.
void draw_edge_slider(CommandTable& commands, const char* title, const char* id,
                      const char* key, int value, int min_value, int max_value,
                      int scale_min, int scale_max, std::string& error) {
    ImGui::TextUnformatted(title);
    ImGui::PushID(id);
    int next = value;
    ImGui::BeginDisabled(value <= min_value);
    if (ImGui::SmallButton("-")) {
        next = value - 1;
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::SetNextItemWidth(std::max(100.0f, std::min(230.0f,
                              ImGui::GetContentRegionAvail().x - 88.0f)));
    ImGui::BeginDisabled(min_value == max_value);
    const bool moved = ImGui::SliderInt("##edge", &next, scale_min, scale_max);
    const bool dragging = ImGui::IsItemActive();
    const bool released = ImGui::IsItemDeactivatedAfterEdit();
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(value >= max_value);
    if (ImGui::SmallButton("+")) {
        next = value + 1;
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::Text("%d px", next);
    ImGui::PopID();

    if (next != value && (moved || !dragging)) {
        const CommandResult preview = commands.invoke("mask.edge.preview", json{{key, next}});
        error = preview.ok ? "" : preview.error;
        if (!preview.ok) {
            return;
        }
    }
    // Keep the displayed value when the pointer is released. Only the provisional Undo
    // gesture ends here; the reference mask and slider position remain unchanged.
    if ((!dragging && next != value) || released) {
        const CommandResult finish = commands.invoke("mask.edge.finish", json::object());
        error = finish.ok ? "" : finish.error;
    }
}

void draw_edge_controls(AppState& state, CommandTable& commands) {
    CanvasForm& form = state.canvas_form;
    if (form.edge_preview_layer != 0 &&
        (state.doc.empty() || state.active_layer().id != form.edge_preview_layer)) {
        commands.invoke("mask.edge.finish", json::object());
    }
    if (!state.editing_mask()) {
        return;
    }
    if (!ImGui::CollapsingHeader("Adjust layer edge")) {
        if (form.edge_preview_layer != 0) {
            commands.invoke("mask.edge.finish", json::object());
        }
        return;
    }
    const CommandResult current = commands.invoke("mask.edge.state", json::object());
    if (!current.ok) {
        ImGui::TextWrapped("%s", current.error.c_str());
        return;
    }
    const json& edge = current.value;
    ImGui::TextWrapped("Adjust the layer mask directly. Move either slider or click +/-; reversing the value restores the original edge.");
    draw_edge_slider(commands, "Grow / shrink", "size", "shift",
                     edge.at("shift").get<int>(), -edge.at("shift_limit").get<int>(),
                     edge.at("shift_limit").get<int>(), edge.at("shift_min").get<int>(),
                     edge.at("shift_max").get<int>(), form.edge_error);
    draw_edge_slider(commands, "Feather (0 = hard edge)", "feather", "feather",
                     edge.at("feather").get<int>(), 0, edge.at("feather_limit").get<int>(),
                     edge.at("feather_min").get<int>(), edge.at("feather_max").get<int>(),
                     form.edge_error);
    ImGui::TextWrapped("The ranges follow the values at their limits. No Apply button or reset required.");
    if (!form.edge_error.empty()) {
        ImGui::TextWrapped("%s", form.edge_error.c_str());
    }
}

void draw_coverage_controls(AppState& state, CommandTable& commands) {
    ImGui::SeparatorText("Selection (blue)");
    ImGui::TextDisabled("Cover / Un-cover and selection tools always edit this area");
    draw_coverage_operations(commands);
    draw_coverage_transfer(state, commands, selection_json(state));
    if (!state.doc.empty()) {
        ImGui::SeparatorText("Layer editing");
        ImGui::TextUnformatted(state.editing_mask() ? "Editing layer mask: Brush reveals, Eraser hides"
                                                : "Editing image pixels");
        ImGui::TextDisabled("Click the image or mask thumbnail in Layers to switch");
    }
}

std::string brush_step_hint(const CommandTable& commands, const char* field) {
    std::string hint;
    for (const BrushStep& step : kBrushSteps) {
        if (std::string_view(step.field) != field) {
            continue;
        }
        for (const std::string& binding : commands.shortcuts_for(step.id)) {
            if (!hint.empty()) {
                hint += "\n";
            }
            hint += binding;
        }
    }
    return hint;
}

void draw_brush_size(AppState& state, CommandTable& commands) {
    float size = static_cast<float>(state.brush.size);
    if (ImGui::SliderFloat("size", &size, static_cast<float>(kMinBrushSize),
                           static_cast<float>(kMaxBrushSize), "%.1f px",
                           ImGuiSliderFlags_Logarithmic) &&
        static_cast<double>(size) != state.brush.size) {
        commands.invoke("tool.set", json{{"size", static_cast<double>(size)}});
    }
}

void draw_brush_slider(AppState& state, CommandTable& commands, const BrushSlider& slider) {
    float value = static_cast<float>(state.brush.*(slider.member));
    if (ImGui::SliderFloat(slider.label, &value, 0.0f, 1.0f, "%.2f") &&
        static_cast<double>(value) != state.brush.*(slider.member)) {
        commands.invoke("tool.set", json{{slider.label, static_cast<double>(value)}});
    }
    const std::string hint = brush_step_hint(commands, slider.label);
    if (!hint.empty() && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
        ImGui::SetTooltip("step keys\n%s", hint.c_str());
    }
}

void draw_brush_sliders(AppState& state, CommandTable& commands) {
    constexpr BrushSlider sliders[]{
        {"hardness", &Brush::hardness}, {"opacity", &Brush::opacity},
        {"flow", &Brush::flow},         {"spacing", &Brush::spacing},
    };
    draw_brush_size(state, commands);
    for (const BrushSlider& slider : sliders) {
        draw_brush_slider(state, commands, slider);
    }
}

void draw_brush_color(AppState& state, CommandTable& commands) {
    float rgba[4] = {static_cast<float>(state.color.r) / 255.0f,
                     static_cast<float>(state.color.g) / 255.0f,
                     static_cast<float>(state.color.b) / 255.0f,
                     static_cast<float>(state.color.a) / 255.0f};
    if (!ImGui::ColorEdit4("color", rgba, ImGuiColorEditFlags_AlphaBar)) {
        return;
    }
    const auto channel = [](float value) {
        return static_cast<std::int64_t>(std::lround(std::clamp(value, 0.0f, 1.0f) * 255.0f));
    };
    commands.invoke("tool.set", json{{"color", json::array({channel(rgba[0]), channel(rgba[1]),
                                                              channel(rgba[2]), channel(rgba[3])})}});
}
}

void draw_verb_section(CommandTable& commands, const json& intent, io::Theme theme);
void draw_ai_status_line(AppState& state, CommandTable& commands, const json& view);

// Every widget here is a front-end for a command: the panel, a key and a wire client cannot
// drift apart because there is nothing else to keep in step.
void draw_tool_panel(AppState& state, CommandTable& commands, const json& intent,
                     ReferenceThumbs& reference_thumbs) {
    ImGui::Begin(kWindowTool);
    draw_file_row(state, commands);
    draw_canvas_controls(state, commands);
    draw_edge_controls(state, commands);
    ImGui::Separator();
    draw_tool_picker(state, commands);
    draw_coverage_controls(state, commands);
    draw_ai_references(state, commands, reference_thumbs, intent);
    draw_verb_section(commands, intent, state.ui_theme);
    if (tool_mode(state.tool).has_value()) {
        draw_brush_sliders(state, commands);
        if (!state.editing_mask()) {
            draw_brush_color(state, commands);
        }
    }
    ImGui::End();
}

namespace {

void draw_ai_quote(const json& view, const ui_theme::Palette& colors) {
    if (view["estimate_s"].is_number()) {
        ImGui::SameLine();
        ImGui::TextDisabled("~%.1f s", view["estimate_s"].get<double>());
    }
    if (view["quote"].is_object()) {
        const json& quote = view["quote"];
        ImGui::SameLine();
        ImGui::TextDisabled("%dx%d%s", quote["size"][0].get<int>(), quote["size"][1].get<int>(),
                            quote["resampled"].get<bool>() ? " rescaled" : "");
    }
    if (view["swap"].get<bool>()) {
        ImGui::SameLine();
        ImGui::TextColored(colors.warning, "load %s (currently %s)",
                           view["model"].get<std::string>().c_str(),
                           view["resident"].get<std::string>().c_str());
    }
}

void draw_job_status(AppState& state, CommandTable& commands, const json& job,
                     const ui_theme::Palette& colors) {
    const std::string job_state = job.contains("state") ? job["state"].get<std::string>() : "?";
    ImGui::SameLine();
    if (job_state == "queued" || job_state == "running") {
        const double started = job.value("started_at", 0.0);
        if (started > 0.0) {
            ImGui::TextDisabled("working: %s  %d s", job.value("recipe", std::string()).c_str(),
                                static_cast<int>(state.now - started));
        } else {
            ImGui::TextDisabled("working: %s", job.value("recipe", std::string()).c_str());
        }
    } else {
        ImGui::TextDisabled("%s %s", job.contains("id") ? job["id"].get<std::string>().c_str() : "",
                            job_state.c_str());
    }
    if (job_state == "succeeded") {
        ImGui::SameLine();
        const json& landing = job.contains("landing") ? job["landing"] : json::object();
        const std::string layer = landing.contains("landed_layer")
                                      ? landing["landed_layer"].get<std::string>()
                                      : std::string();
        if (layer.empty()) {
            ImGui::TextDisabled("landing...");
        } else {
            ImGui::TextDisabled("on '%s', Ctrl+Z", layer.c_str());
        }
    } else if (job_state == "queued" || job_state == "running") {
        ImGui::SameLine();
        if (ImGui::SmallButton("Cancel")) {
            commands.invoke("job.cancel", json{{"id", job["id"]}});
        }
    } else if (job_state == "failed" || job_state == "cancelled") {
        ImGui::SameLine();
        ImGui::TextColored(colors.danger, "%s",
                           job.contains("failure") ? job["failure"].get<std::string>().c_str() : "");
    }
}

void draw_ai_error(const json& view, const ui_theme::Palette& colors) {
    const std::string error = view["error"].get<std::string>();
    if (error.empty()) {
        return;
    }
    ImGui::SameLine();
    ImGui::TextColored(colors.danger, "%s", error.c_str());
}

}

void draw_ai_status_line(AppState& state, CommandTable& commands, const json& view) {
    const ui_theme::Palette& colors = ui_theme::palette(state.ui_theme);
    if (!view["configured"].get<bool>()) {
        ImGui::TextDisabled("ai: no server, set $THENOISE_URL");
        return;
    }
    if (!view["connected"].get<bool>()) {
        ImGui::TextColored(colors.danger, "ai: offline");
        ImGui::SameLine();
        if (ImGui::SmallButton("Connect")) {
            commands.invoke("ai.intent", json{{"connect", true}});
        }
        return;
    }
    if (view["label"].is_null() || view["model"].get<std::string>().empty()) {
        ImGui::TextDisabled("ai: %s", view["reason"].get<std::string>().c_str());
        return;
    }
    ImGui::Text("ai: %s", view["label"].get<std::string>().c_str());
    ImGui::SameLine();
    ImGui::TextDisabled("%s", view["model"].get<std::string>().c_str());
    ImGui::SameLine();
    if (view["seed_pinned"].get<bool>()) {
        ImGui::TextDisabled("seed %llu (pinned)",
                            static_cast<unsigned long long>(view["seed"].get<std::uint64_t>()));
    } else {
        ImGui::TextDisabled("seed random (last %llu)",
                            static_cast<unsigned long long>(view["seed"].get<std::uint64_t>()));
    }
    draw_ai_quote(view, colors);
    if (view["job"].is_object()) {
        draw_job_status(state, commands, view["job"], colors);
    }
    draw_ai_error(view, colors);
}

}
