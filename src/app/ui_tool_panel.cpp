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

void draw_coverage_target(AppState& state, CommandTable& commands, bool has_mask) {
    if (ImGui::RadioButton("marked area", state.coverage_target == CoverageTarget::marked)) {
        commands.invoke("coverage.target", json{{"to", "marked"}});
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(!has_mask);
    if (ImGui::RadioButton("layer mask", state.coverage_target == CoverageTarget::mask)) {
        commands.invoke("coverage.target", json{{"to", "mask"}});
    }
    ImGui::EndDisabled();
}

void draw_coverage_operations(CommandTable& commands, bool at_mask) {
    if (ImGui::Button("All")) {
        commands.invoke(at_mask ? "mask.fill" : "select.all",
                        at_mask ? json{{"to", "show"}} : json::object());
    }
    ImGui::SameLine();
    if (ImGui::Button("None")) {
        commands.invoke(at_mask ? "mask.fill" : "select.clear",
                        at_mask ? json{{"to", "hide"}} : json::object());
    }
    ImGui::SameLine();
    if (ImGui::Button("Invert")) {
        commands.invoke(at_mask ? "mask.invert" : "select.invert", json::object());
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
    if (ImGui::Button("Apply canvas size")) {
        if (form.width <= 0 || form.height <= 0 ||
            static_cast<std::int64_t>(form.width) * form.height > kMaxCanvasPixels) {
            form.error = "Canvas must be positive and at most " +
                         std::to_string(kMaxCanvasPixels) + " pixels";
        } else {
            const int column = form.anchor % 3;
            const int row = form.anchor / 3;
            const int dx = column == 0 ? 0 : column == 2 ? form.width - old.w
                                                          : (form.width - old.w) / 2;
            const int dy = row == 0 ? 0 : row == 2 ? form.height - old.h
                                                       : (form.height - old.h) / 2;
            const auto result = commands.invoke("doc.resize", json{{"width", form.width},
                                                                    {"height", form.height},
                                                                    {"offset_x", dx},
                                                                    {"offset_y", dy},
                                                                    {"mark_added", form.mark_added}});
            form.error = result.ok ? "" : result.error;
        }
    }
    ImGui::TextDisabled("Select All to repaint everything; Fill for an outpainted edge.");
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

void draw_edge_controls(AppState& state, CommandTable& commands) {
    if (state.doc.empty() || !state.active_layer().mask ||
        !ImGui::CollapsingHeader("Adjust layer edge")) {
        return;
    }
    CanvasForm& form = state.canvas_form;
    ImGui::TextWrapped("AI results keep their full generated crop under an editable layer mask.");
    ImGui::Checkbox("Hard edge", &form.edge_hard);
    ImGui::SliderInt("Grow / shrink (px)", &form.edge_shift,
                     -kMaxEdgeShiftPx, kMaxEdgeShiftPx);
    ImGui::BeginDisabled(form.edge_hard);
    ImGui::SliderInt("Additional feather (px)", &form.edge_feather,
                     0, kMaxEdgeFeatherPx);
    ImGui::EndDisabled();
    if (ImGui::Button("Apply edge")) {
        const auto result = commands.invoke("mask.edge", json{{"hard", form.edge_hard},
                                                               {"shift", form.edge_shift},
                                                               {"feather", form.edge_hard ? 0 : form.edge_feather}});
        form.edge_error = result.ok ? "" : result.error;
        if (result.ok) {
            // An edge command is incremental; do not accidentally reapply a nonzero shift.
            form.edge_shift = 0;
            form.edge_feather = 0;
        }
    }
    ImGui::TextDisabled("Use Cover/Un-cover on the layer mask for local corrections.");
    if (!form.edge_error.empty()) {
        ImGui::TextWrapped("%s", form.edge_error.c_str());
    }
}

void draw_coverage_controls(AppState& state, CommandTable& commands) {
    ImGui::SeparatorText("coverage");
    const bool has_mask = !state.doc.empty() && state.active_layer().mask.has_value();
    draw_coverage_target(state, commands, has_mask);
    const bool at_mask = state.coverage_target == CoverageTarget::mask;
    const json selection = selection_json(state);
    if (at_mask && commands.invoke("mask.get", json::object()).value["mask"].is_null()) {
        ImGui::TextDisabled("no mask on this layer yet - Add mask in the Layers panel");
    }
    draw_coverage_operations(commands, at_mask);
    draw_coverage_transfer(state, commands, selection);
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
        draw_brush_color(state, commands);
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
