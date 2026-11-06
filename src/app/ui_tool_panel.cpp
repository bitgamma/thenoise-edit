// SPDX-License-Identifier: Apache-2.0
#include "app/ui_internal.h"
#include "app/ui_icons.h"

#include "app/brush_steps.h"
#include "app/command_args.h"
#include "app/ui_theme.h"
#include "core/maskops.h"
#include "imgui.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <string>
#include <string_view>

namespace noise::app {

using nlohmann::json;

namespace {
struct BrushSlider {
    const char* label;
    const double Brush::*member;
};

bool fits_button_row(std::initializer_list<const char*> labels) {
    const ImGuiStyle& style = ImGui::GetStyle();
    float width = 0.0f;
    for (const char* label : labels) {
        width += ImGui::CalcTextSize((std::string("     ") + label).c_str()).x +
                 style.FramePadding.x * 2.0f;
    }
    width += static_cast<float>(labels.size() - 1) * style.ItemSpacing.x;
    return ImGui::GetContentRegionAvail().x >= width;
}

bool fits_button_pair(const char* first, const char* second) {
    return fits_button_row({first, second});
}

void draw_coverage_operations(CommandTable& commands) {
    const bool all_in_one_row = fits_button_row({"All", "None", "Invert"});
    const bool first_two_fit = fits_button_pair("All", "None");
    if (ui_icons::labeled_button(ui_icons::Icon::SquareDashed, "All", ImVec2(0.0f, 0.0f))) {
        commands.invoke("select.all", json::object());
    }
    if (all_in_one_row || first_two_fit) ImGui::SameLine();
    if (ui_icons::labeled_button(ui_icons::Icon::SquareDashedX, "None", ImVec2(0.0f, 0.0f))) {
        commands.invoke("select.clear", json::object());
    }
    if (all_in_one_row) ImGui::SameLine();
    if (ui_icons::labeled_button(ui_icons::Icon::RefreshCw, "Invert", ImVec2(0.0f, 0.0f))) {
        commands.invoke("select.invert", json::object());
    }
}

void draw_coverage_transfer(AppState& state, CommandTable& commands, const json& selection) {
    if (state.doc.empty()) {
        return;
    }
    const Layer& active = state.active_layer();
    const bool same_row = fits_button_pair("To layer mask", "From layer mask");
    ImGui::BeginDisabled(!selection["active"].get<bool>());
    if (ui_icons::labeled_button(ui_icons::Icon::Contrast, "To layer mask", ImVec2(0.0f, 0.0f))) {
        commands.invoke("mask.from_selection", json::object());
    }
    ImGui::EndDisabled();
    if (same_row) ImGui::SameLine();
    ImGui::BeginDisabled(!active.mask.has_value());
    if (ui_icons::labeled_button(ui_icons::Icon::Contrast, "From layer mask", ImVec2(0.0f, 0.0f))) {
        commands.invoke("select.from_mask", json::object());
    }
    ImGui::EndDisabled();
    if (active.mask) {
        ImGui::TextDisabled("mask on '%s': %d px shown", active.name.c_str(),
                            static_cast<int>(active.mask->covered()));
    }
}

void muted_wrapped(std::string_view message) {
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextWrapped("%.*s", static_cast<int>(message.size()), message.data());
    ImGui::PopStyleColor();
}

void draw_latest_ai_landing_error(const AppState& state) {
    if (state.ai.jobs == nullptr || state.doc.empty()) {
        return;
    }
    const std::vector<ai::JobRecord> jobs = state.ai.jobs->all();
    for (auto it = jobs.rbegin(); it != jobs.rend(); ++it) {
        if (it->landing.document_epoch != state.document_epoch) {
            continue;
        }
        if (it->landing.land_refused) {
            ImGui::PushStyleColor(ImGuiCol_Text, ui_theme::palette(state.ui_theme).danger);
            ImGui::TextWrapped("AI result %s could not be applied: %s", it->id.c_str(),
                               it->landing.land_error.c_str());
            ImGui::PopStyleColor();
        }
        break;
    }
}

void draw_image_size_controls(AppState& state, CommandTable& commands) {
    if (!ImGui::CollapsingHeader("Image size / Upscale")) {
        return;
    }
    if (state.doc.empty()) {
        ImGui::TextDisabled("Open an image first");
        return;
    }
    ImageSizeForm& form = state.image_size_form;
    const Extent original = state.doc.size();
    if (form.source != original || form.source_revision != state.doc.geometry_revision()) {
        form.source = original;
        form.source_revision = state.doc.geometry_revision();
        form.width = original.w;
        form.height = original.h;
        form.resize_error.clear();
        form.upscale_error.clear();
        form.deferred_error.clear();
    }
    ImGui::SetNextItemWidth(110.0f);
    const bool changed_width = ImGui::InputInt("Width##image", &form.width);
    ImGui::SetNextItemWidth(110.0f);
    const bool changed_height = ImGui::InputInt("Height##image", &form.height);
    if (form.lock_aspect && (changed_width || changed_height)) {
        if (changed_width && form.width > 0) {
            form.height = std::max(1, static_cast<int>(std::lround(
                static_cast<double>(form.width) * original.h / original.w)));
        } else if (changed_height && form.height > 0) {
            form.width = std::max(1, static_cast<int>(std::lround(
                static_cast<double>(form.height) * original.w / original.h)));
        }
    }
    if (changed_width || changed_height) {
        form.resize_error.clear();
        form.upscale_error.clear();
        form.deferred_error.clear();
    }
    ImGui::Checkbox("Keep aspect ratio##image", &form.lock_aspect);
    bool preset_changed = false;
    for (int factor : {2, 4}) {
        ImGui::PushID(factor);
        if (factor == 4) {
            ImGui::SameLine();
        }
        if (ImGui::SmallButton(factor == 2 ? "2x" : "4x")) {
            const std::int64_t width = static_cast<std::int64_t>(original.w) * factor;
            const std::int64_t height = static_cast<std::int64_t>(original.h) * factor;
            if (width * height <= kMaxCanvasPixels) {
                form.width = static_cast<int>(width);
                form.height = static_cast<int>(height);
                preset_changed = true;
                form.resize_error.clear();
                form.upscale_error.clear();
                form.deferred_error.clear();
            } else {
                form.resize_error = "Target exceeds the document pixel limit";
            }
        }
        ImGui::PopID();
    }
    if (ImGui::Checkbox("Entire layer stack##resize", &form.whole_stack) && form.whole_stack) {
        commands.invoke("image.upscale.defer", json{{"enabled", false}});
        form.deferred_error.clear();
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
        ImGui::SetTooltip("Preserve individually editable layers at the new size (Lanczos3 only)");
    }
    if (form.whole_stack) {
        muted_wrapped("Entire layer stack is available with Lanczos3 only.");
    }
    const std::int64_t source_area = static_cast<std::int64_t>(original.w) * original.h;
    const std::int64_t target_area = static_cast<std::int64_t>(form.width) * form.height;
    const bool valid = form.width > 0 && form.height > 0 &&
                       target_area <= kMaxCanvasPixels;
    const bool enlarged = valid && target_area > source_area;
    const ai::Recipe* recipe = state.ai.recipes.find("upscale");
    Readiness ready = assess(state, recipe);
    const std::vector<std::string> candidates = state.ai.registry != nullptr
        ? state.ai.registry->candidates("upscale", /*include_not_downloaded=*/true)
        : std::vector<std::string>{};
    const std::string selected_model = !form.model.empty() ? form.model
        : std::find(candidates.begin(), candidates.end(), ready.model) != candidates.end()
            ? ready.model : candidates.empty() ? std::string{} : candidates.front();
    const int native = ai::native_upscale_factor(selected_model);
    const bool valid_model =
        std::find(candidates.begin(), candidates.end(), selected_model) != candidates.end();
    ImGui::BeginDisabled(form.whole_stack);
    ImGui::TextUnformatted("AI Upscaler");
    if (!candidates.empty()) {
        ImGui::SetNextItemWidth(-1.0f);
        if (ImGui::BeginCombo("##ai_upscaler", selected_model.c_str())) {
            for (const std::string& candidate : candidates) {
                const int factor = ai::native_upscale_factor(candidate);
                const ai::ModelEntry* entry = state.ai.registry->model(candidate);
                const std::string label = candidate + (factor > 0
                    ? " (" + std::to_string(factor) + "x native)" : " (scale unknown)") +
                    (entry != nullptr && !entry->downloaded ? " - not downloaded" : "");
                if (ImGui::Selectable(label.c_str(), candidate == selected_model)) {
                    form.model = candidate;
                    form.upscale_error.clear();
                }
            }
            ImGui::EndCombo();
        }
    } else {
        muted_wrapped("No AI upscalers available.");
    }
    ImGui::EndDisabled();
    const std::int64_t cross = std::abs(static_cast<std::int64_t>(form.width) * original.h -
                                        static_cast<std::int64_t>(form.height) * original.w);
    const ai::ModelEntry* selected_entry = state.ai.registry
        ? state.ai.registry->model(selected_model) : nullptr;
    std::string ai_problem;
    if (!state.ai.connected) {
        ai_problem = "Connect to Lemonade to use AI upscaling.";
    } else if (recipe == nullptr || !state.ai.serving()) {
        ai_problem = "The AI upscale service is unavailable.";
    } else if (!valid_model) {
        ai_problem = "Choose an available AI Upscaler.";
    } else if (selected_entry != nullptr && !selected_entry->downloaded) {
        ai_problem = "Download this AI Upscaler before running it.";
    } else if (!valid) {
        ai_problem = target_area > kMaxCanvasPixels
            ? "Target exceeds the document's 64-Mi-pixel limit."
            : "Enter a positive width and height.";
    } else if (!enlarged || form.width <= original.w || form.height <= original.h) {
        ai_problem = "AI upscale needs a larger width and height than the source.";
    } else if (cross > std::max(original.w, original.h)) {
        ai_problem = "AI upscale must preserve the source aspect ratio.";
    } else if (native == 0) {
        ai_problem = "This AI Upscaler has no known native scaling factor.";
    } else if (form.width > static_cast<std::int64_t>(original.w) * native ||
               form.height > static_cast<std::int64_t>(original.h) * native) {
        ai_problem = "This AI Upscaler supports up to " + std::to_string(native) +
                     "x the source dimensions.";
    }
    bool running = false;
    if (state.ai.jobs) {
        for (const ai::JobRecord& job : state.ai.jobs->all()) {
            running = running || (job.recipe_id == "upscale" &&
                (job.state == ai::JobState::queued || job.state == ai::JobState::running));
        }
    }
    const bool same_image_row = fits_button_pair("Resize (Lanczos3)", "Upscale (AI)");
    ImGui::BeginDisabled(!valid || (form.width == original.w && form.height == original.h));
    if (ui_icons::labeled_button(ui_icons::Icon::Scaling, "Resize (Lanczos3)", ImVec2(0.0f, 0.0f))) {
        const CommandResult result = commands.invoke("doc.resample",
            json{{"width", form.width}, {"height", form.height},
                 {"whole_stack", form.whole_stack}});
        form.resize_error = result.ok ? "" : result.error;
    }
    ImGui::EndDisabled();
    if (same_image_row) ImGui::SameLine();
    ImGui::BeginDisabled(form.whole_stack || !ai_problem.empty() || running);
    if (ui_icons::labeled_button(ui_icons::Icon::ImageUpscale, "Upscale (AI)", ImVec2(0.0f, 0.0f))) {
        const CommandResult result = commands.invoke("image.upscale",
            json{{"width", form.width}, {"height", form.height},
                 {"model", selected_model}});
        form.upscale_error = result.ok ? "" : result.error;
    }
    ImGui::EndDisabled();
    if (!form.upscale_error.empty()) {
        ImGui::TextWrapped("AI upscale: %s", form.upscale_error.c_str());
    } else if (running) {
        muted_wrapped("AI upscale in progress...");
    } else if (!ai_problem.empty()) {
        muted_wrapped(ai_problem);
    }
    if (!form.resize_error.empty()) {
        ImGui::TextWrapped("Resize: %s", form.resize_error.c_str());
    }
    ImGui::BeginDisabled(form.whole_stack);
    ImGui::SeparatorText("Next AI edit");
    const bool armed = state.deferred_upscale &&
        state.deferred_upscale->document_epoch == state.document_epoch &&
        state.deferred_upscale->source == original;
    bool defer = armed;
    if (ImGui::Checkbox("Use target size for next AI edit", &defer)) {
        const CommandResult result = commands.invoke("image.upscale.defer", defer
            ? json{{"width", form.width}, {"height", form.height}}
            : json{{"enabled", false}});
        form.deferred_error = result.ok ? "" : result.error;
    } else if (armed && (changed_width || changed_height || preset_changed)) {
        const CommandResult result = commands.invoke("image.upscale.defer",
            json{{"width", form.width}, {"height", form.height}});
        form.deferred_error = result.ok ? "" : result.error;
        if (!result.ok) {
            commands.invoke("image.upscale.defer", json{{"enabled", false}});
        }
    }
    if (state.deferred_upscale && state.deferred_upscale->source == original &&
        state.deferred_upscale->document_epoch == state.document_epoch) {
        const Extent target = state.deferred_upscale->target;
        muted_wrapped("Next Change, Fill, Remove or Heal generates " +
                       std::to_string(target.w) + " x " + std::to_string(target.h) +
                       " in a single model request.");
    }
    muted_wrapped("The next AI edit generates the full image at the chosen size. "
                   "Areas outside the selection may also change.");
    if (!form.deferred_error.empty()) {
        ImGui::TextWrapped("Next AI edit: %s", form.deferred_error.c_str());
    }
    ImGui::EndDisabled();
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
    const bool same_resize_row = fits_button_pair("Resize canvas", "Resize & Fill...");
    if (ui_icons::labeled_button(ui_icons::Icon::Expand, "Resize canvas", ImVec2(0.0f, 0.0f))) {
        resize(false);
    }
    if (same_resize_row) ImGui::SameLine();
    if (ui_icons::labeled_button(ui_icons::Icon::PaintBucket, "Resize & Fill...", ImVec2(0.0f, 0.0f))) {
        resize(true);
    }
    ImGui::TextDisabled("Fill opens a prompt; Run starts the AI request.");
    ImGui::SeparatorText("Rotate");
    const bool same_rotate_row = fits_button_pair("90 deg CCW", "90 deg CW");
    if (ui_icons::labeled_button(ui_icons::Icon::RotateCcw, "90 deg CCW", ImVec2(0.0f, 0.0f))) {
        const CommandResult result = commands.invoke("doc.rotate", json{{"direction", "ccw"}});
        form.error = result.ok ? "" : result.error;
    }
    if (same_rotate_row) ImGui::SameLine();
    if (ui_icons::labeled_button(ui_icons::Icon::RotateCw, "90 deg CW", ImVec2(0.0f, 0.0f))) {
        const CommandResult result = commands.invoke("doc.rotate", json{{"direction", "cw"}});
        form.error = result.ok ? "" : result.error;
    }
    ImGui::SeparatorText("Crop");
    std::array<int, 4> crop{form.crop.x, form.crop.y, form.crop.w, form.crop.h};
    if (ImGui::InputInt4("x, y, w, h##crop", crop.data())) {
        form.crop = Rect{crop[0], crop[1], crop[2], crop[3]};
    }
    const bool same_crop_row = fits_button_pair("Crop to rectangle", "Crop to selection");
    if (ui_icons::labeled_button(ui_icons::Icon::Crop, "Crop to rectangle", ImVec2(0.0f, 0.0f))) {
        const auto result = commands.invoke("doc.crop", json{{"rect", json::array(
            {form.crop.x, form.crop.y, form.crop.w, form.crop.h})}});
        form.error = result.ok ? "" : result.error;
    }
    if (same_crop_row) ImGui::SameLine();
    if (ui_icons::labeled_button(ui_icons::Icon::Crop, "Crop to selection", ImVec2(0.0f, 0.0f))) {
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

void record_tool_option(AppState& state, const char* name) {
    const ImVec2 start = ImGui::GetItemRectMin();
    const ImVec2 end = ImGui::GetItemRectMax();
    state.tool_option_controls.push_back(ToolPaletteButton{
        name, ScreenRect{start.x, start.y, end.x - start.x, end.y - start.y}});
}

void draw_brush_size(AppState& state, CommandTable& commands) {
    ImGui::TextUnformatted("Size");
    float size = static_cast<float>(state.brush.size);
    ImGui::SetNextItemWidth(-1.0f);
    const bool changed = ImGui::SliderFloat("##value", &size, static_cast<float>(kMinBrushSize),
                                             static_cast<float>(kMaxBrushSize), "%.1f px",
                                             ImGuiSliderFlags_Logarithmic);
    record_tool_option(state, "size");
    if (changed && static_cast<double>(size) != state.brush.size) {
        commands.invoke("tool.set", json{{"size", static_cast<double>(size)}});
    }
}

void draw_brush_slider(AppState& state, CommandTable& commands, const BrushSlider& slider) {
    ImGui::TextUnformatted(slider.label);
    float value = static_cast<float>(state.brush.*(slider.member));
    ImGui::SetNextItemWidth(-1.0f);
    const bool changed = ImGui::SliderFloat("##value", &value, 0.0f, 1.0f, "%.2f");
    record_tool_option(state, slider.label);
    if (changed && static_cast<double>(value) != state.brush.*(slider.member)) {
        commands.invoke("tool.set", json{{slider.label, static_cast<double>(value)}});
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
        const std::string hint = brush_step_hint(commands, slider.label);
        if (!hint.empty()) {
            ImGui::SetTooltip("step keys\n%s", hint.c_str());
        }
    }
}

void draw_brush_color(AppState& state, CommandTable& commands) {
    ImGui::TextUnformatted("Color");
    float rgba[4] = {static_cast<float>(state.color.r) / 255.0f,
                     static_cast<float>(state.color.g) / 255.0f,
                     static_cast<float>(state.color.b) / 255.0f,
                     static_cast<float>(state.color.a) / 255.0f};
    const bool changed = ImGui::ColorEdit4("##value", rgba, ImGuiColorEditFlags_NoInputs |
                                            ImGuiColorEditFlags_NoLabel | ImGuiColorEditFlags_AlphaBar);
    record_tool_option(state, "color");
    if (!changed) return;
    const auto channel = [](float value) {
        return static_cast<std::int64_t>(std::lround(std::clamp(value, 0.0f, 1.0f) * 255.0f));
    };
    commands.invoke("tool.set", json{{"color", json::array({channel(rgba[0]), channel(rgba[1]),
                                                              channel(rgba[2]), channel(rgba[3])})}});
}
}

// Size gets the longest track in every width class. The other controls wrap into
// separate rows instead of shrinking every slider to an unreadable few pixels.
namespace {
enum class ToolOptionField { size, hardness, opacity, flow, spacing, color };

struct ToolOptionColumn {
    ToolOptionField field;
    float width;
};

const char* option_field_name(ToolOptionField field) {
    switch (field) {
        case ToolOptionField::size: return "size";
        case ToolOptionField::hardness: return "hardness";
        case ToolOptionField::opacity: return "opacity";
        case ToolOptionField::flow: return "flow";
        case ToolOptionField::spacing: return "spacing";
        case ToolOptionField::color: return "color";
    }
    return "";
}

void draw_option_row(AppState& state, CommandTable& commands, const char* id,
                     std::initializer_list<ToolOptionColumn> columns) {
    constexpr ImGuiTableFlags flags = ImGuiTableFlags_SizingStretchProp |
                                      ImGuiTableFlags_NoSavedSettings;
    if (!ImGui::BeginTable(id, static_cast<int>(columns.size()), flags)) return;
    for (const ToolOptionColumn& column : columns) {
        const bool color = column.field == ToolOptionField::color;
        ImGui::TableSetupColumn(option_field_name(column.field),
                                color ? ImGuiTableColumnFlags_WidthFixed
                                      : ImGuiTableColumnFlags_WidthStretch,
                                column.width);
    }
    ImGui::TableNextRow();
    for (const ToolOptionColumn& column : columns) {
        ImGui::TableNextColumn();
        const char* name = option_field_name(column.field);
        const double Brush::*member = nullptr;
        switch (column.field) {
            case ToolOptionField::size:
            case ToolOptionField::color: break;
            case ToolOptionField::hardness: member = &Brush::hardness; break;
            case ToolOptionField::opacity: member = &Brush::opacity; break;
            case ToolOptionField::flow: member = &Brush::flow; break;
            case ToolOptionField::spacing: member = &Brush::spacing; break;
        }
        ImGui::PushID(name);
        if (column.field == ToolOptionField::size) {
            draw_brush_size(state, commands);
        } else if (column.field == ToolOptionField::color) {
            draw_brush_color(state, commands);
        } else {
            draw_brush_slider(state, commands, BrushSlider{name, member});
        }
        ImGui::PopID();
    }
    ImGui::EndTable();
}
} // namespace

void draw_tool_options(AppState& state, CommandTable& commands) {
    state.tool_option_controls.clear();
    const std::optional<ToolAction> action = tool_action(state.tool);
    const bool paints = action == ToolAction::paint || action == ToolAction::coverage;
    const bool has_color = action == ToolAction::paint &&
                           tool_mode(state.tool) == PaintMode::Over && !state.editing_mask();
    const float width = ImGui::GetContentRegionAvail().x;
    const bool wide = width >= (has_color ? 900.0f : 830.0f);
    const bool narrow = width < 520.0f;
    const float height = paints ? (wide ? 85.0f : narrow ? 177.0f : 133.0f) : 67.0f;
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoScrollbar |
                                   ImGuiWindowFlags_NoScrollWithMouse;
    if (ImGui::BeginChild("##tool_options", ImVec2(0.0f, height),
                          ImGuiChildFlags_Borders, flags)) {
        const ImVec2 pos = ImGui::GetWindowPos();
        const ImVec2 size = ImGui::GetWindowSize();
        state.tool_options_window = ScreenRect{pos.x, pos.y, size.x, size.y};
        if (!paints) {
            for (const ToolInfo& tool : kTools) {
                if (state.tool == tool.id) {
                    ImGui::TextUnformatted(tool.title);
                    break;
                }
            }
            if (action == ToolAction::select) {
                if (ImGui::SmallButton("All")) commands.invoke("select.all", json::object());
                ImGui::SameLine();
                if (ImGui::SmallButton("None")) commands.invoke("select.clear", json::object());
                ImGui::SameLine();
                if (ImGui::SmallButton("Invert")) commands.invoke("select.invert", json::object());
            }
        } else {
            const char* target = action == ToolAction::coverage ? "AI selection" :
                                 state.editing_mask() ? "layer mask" : "layer pixels";
            ImGui::TextDisabled("%s  |  %s", state.tool.c_str(), target);
            using Field = ToolOptionField;
            if (wide) {
                if (has_color) {
                    draw_option_row(state, commands, "##wide_row", {
                        {Field::size, 3.4f}, {Field::hardness, 1.0f},
                        {Field::opacity, 1.0f}, {Field::flow, 1.0f},
                        {Field::spacing, 1.0f}, {Field::color, 42.0f}});
                } else {
                    draw_option_row(state, commands, "##wide_row", {
                        {Field::size, 3.4f}, {Field::hardness, 1.0f},
                        {Field::opacity, 1.0f}, {Field::flow, 1.0f},
                        {Field::spacing, 1.0f}});
                }
            } else if (!narrow) {
                draw_option_row(state, commands, "##main_row", {
                    {Field::size, 2.4f}, {Field::hardness, 1.0f}});
                if (has_color) {
                    draw_option_row(state, commands, "##detail_row", {
                        {Field::opacity, 1.0f}, {Field::flow, 1.0f},
                        {Field::spacing, 1.0f}, {Field::color, 42.0f}});
                } else {
                    draw_option_row(state, commands, "##detail_row", {
                        {Field::opacity, 1.0f}, {Field::flow, 1.0f},
                        {Field::spacing, 1.0f}});
                }
            } else {
                if (has_color) {
                    draw_option_row(state, commands, "##size_row", {
                        {Field::size, 1.0f}, {Field::color, 42.0f}});
                } else {
                    draw_option_row(state, commands, "##size_row", {{Field::size, 1.0f}});
                }
                draw_option_row(state, commands, "##detail_row_1", {
                    {Field::hardness, 1.0f}, {Field::opacity, 1.0f}});
                draw_option_row(state, commands, "##detail_row_2", {
                    {Field::flow, 1.0f}, {Field::spacing, 1.0f}});
            }
        }
    }
    ImGui::EndChild();
}


void draw_verb_section(AppState& state, CommandTable& commands, const json& intent, io::Theme theme);
void draw_ai_status_line(AppState& state, CommandTable& commands, const json& view);

// Every widget here is a front-end for a command: the panel, a key and a wire client cannot
// drift apart because there is nothing else to keep in step.
void draw_tool_panel(AppState& state, CommandTable& commands, const json& intent,
                     ReferenceThumbs& reference_thumbs) {
    ImGui::Begin(kWindowTool, nullptr, ImGuiWindowFlags_NoTitleBar);
    draw_file_row(state, commands);
    draw_latest_ai_landing_error(state);
    draw_image_size_controls(state, commands);
    draw_canvas_controls(state, commands);
    draw_edge_controls(state, commands);
    ImGui::Separator();
    draw_coverage_controls(state, commands);
    draw_ai_references(state, commands, reference_thumbs, intent);
    draw_verb_section(state, commands, intent, state.ui_theme);
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
        const bool refused = landing.value("land_refused", false);
        const std::string layer = landing.value("landed_layer", std::string());
        if (refused) {
            ImGui::TextColored(colors.danger, "result not applied (see Tools)");
        } else if (layer.empty()) {
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
