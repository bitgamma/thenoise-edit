// SPDX-License-Identifier: Apache-2.0
#include "app/ui_internal.h"
#include "app/ui_dialog_fields.h"

#include "app/ai_intent.h"
#include "app/ui_icons.h"

#include "imgui.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace noise::app {

using nlohmann::json;

namespace {

constexpr float kDialogWidthPx = 410.0f;
constexpr int kScalePresets[]{2, 4};
constexpr ImGuiWindowFlags kDialogFlags = ImGuiWindowFlags_AlwaysAutoResize |
                                          ImGuiWindowFlags_NoCollapse;
constexpr ImGuiWindowFlags kDialogHostFlags = ImGuiWindowFlags_NoTitleBar |
                                               ImGuiWindowFlags_NoResize |
                                               ImGuiWindowFlags_NoMove |
                                               ImGuiWindowFlags_NoScrollbar |
                                               ImGuiWindowFlags_NoSavedSettings |
                                               ImGuiWindowFlags_NoBringToFrontOnFocus |
                                               ImGuiWindowFlags_NoNavFocus |
                                               ImGuiWindowFlags_NoDocking |
                                               ImGuiWindowFlags_NoBackground |
                                               ImGuiWindowFlags_NoInputs;

std::optional<ImageDialog> requested_dialog;
std::optional<ImageDialog> active_dialog;

const char* dialog_name(ImageDialog dialog) {
    switch (dialog) {
        case ImageDialog::image_size: return "Image Size";
        case ImageDialog::upscale: return "Upscale";
        case ImageDialog::canvas_size: return "Canvas Size";
        case ImageDialog::crop: return "Crop";
    }
    return "Image";
}

void muted_wrapped(const std::string& message) {
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextWrapped("%s", message.c_str());
    ImGui::PopStyleColor();
}

void close_dialog() {
    active_dialog.reset();
    ImGui::CloseCurrentPopup();
}

void reset_image_form(AppState& state) {
    ImageSizeForm& form = state.image_size_form;
    const Extent source = state.doc.size();
    form.source = source;
    form.source_revision = state.doc.geometry_revision();
    form.width = source.w;
    form.height = source.h;
    form.whole_stack = false;
    form.lock_aspect = true;
    form.resize_error.clear();
    form.upscale_error.clear();
    form.deferred_error.clear();
    form.use_for_next_edit = false;
}

void initialize_image_form(AppState& state) {
    const ImageSizeForm& form = state.image_size_form;
    if (form.source != state.doc.size() ||
        form.source_revision != state.doc.geometry_revision()) {
        reset_image_form(state);
    }
}

void update_aspect(ImageSizeForm& form, Extent source, bool width_changed,
                   bool height_changed) {
    if (!form.lock_aspect || (!width_changed && !height_changed)) return;
    if (width_changed && form.width > 0) {
        form.height = std::max(1, static_cast<int>(std::lround(
            static_cast<double>(form.width) * source.h / source.w)));
    } else if (height_changed && form.height > 0) {
        form.width = std::max(1, static_cast<int>(std::lround(
            static_cast<double>(form.height) * source.w / source.h)));
    }
}

bool draw_image_dimensions(ImageSizeForm& form, Extent source) {
    if (!dialog_fields::begin_dimensions("##image_dimensions")) return false;
    const bool width_changed = dialog_fields::dimension_input(
        "Width", "##image_width", form.width);
    const bool height_changed = dialog_fields::dimension_input(
        "Height", "##image_height", form.height);
    ImGui::EndTable();
    update_aspect(form, source, width_changed, height_changed);
    if (width_changed || height_changed) {
        form.resize_error.clear();
        form.upscale_error.clear();
        form.deferred_error.clear();
    }
    return width_changed || height_changed;
}

bool valid_image_target(const ImageSizeForm& form) {
    return form.width > 0 && form.height > 0 &&
        static_cast<std::int64_t>(form.width) * form.height <= kMaxCanvasPixels;
}

void draw_scale_presets(ImageSizeForm& form, Extent source) {
    ImGui::Checkbox("Keep aspect ratio", &form.lock_aspect);
    for (const int factor : kScalePresets) {
        if (factor == 4) ImGui::SameLine();
        ImGui::PushID(factor);
        if (ImGui::SmallButton(factor == 2 ? "2×" : "4×")) {
            const std::int64_t width = static_cast<std::int64_t>(source.w) * factor;
            const std::int64_t height = static_cast<std::int64_t>(source.h) * factor;
            if (width * height <= kMaxCanvasPixels) {
                form.width = static_cast<int>(width);
                form.height = static_cast<int>(height);
                form.resize_error.clear();
                form.upscale_error.clear();
                form.deferred_error.clear();
            } else {
                form.resize_error = "Target exceeds the document pixel limit";
            }
        }
        ImGui::PopID();
    }
}

std::string upscale_problem(AppState& state, const ImageSizeForm& form,
                            const std::string& model, bool valid_model,
                            int native, bool running) {
    const Extent original = state.doc.size();
    const std::int64_t source_area = static_cast<std::int64_t>(original.w) * original.h;
    const std::int64_t target_area = static_cast<std::int64_t>(form.width) * form.height;
    const bool valid = valid_image_target(form);
    const std::int64_t cross = std::abs(static_cast<std::int64_t>(form.width) * original.h -
                                        static_cast<std::int64_t>(form.height) * original.w);
    if (!state.ai.connected) return "Connect to Lemonade to use AI upscaling.";
    if (state.ai.recipes.find("upscale") == nullptr || !state.ai.serving()) {
        return "The AI upscale service is unavailable.";
    }
    if (!valid_model) return "Choose an available AI Upscaler.";
    const ai::ModelEntry* entry = state.ai.registry ? state.ai.registry->model(model) : nullptr;
    if (entry != nullptr && !entry->downloaded) {
        return "Download this AI Upscaler before running it.";
    }
    if (!valid) {
        return target_area > kMaxCanvasPixels
            ? "Target exceeds the document's " + std::to_string(kMaxCanvasPixels) +
              "-pixel limit."
            : "Enter a positive width and height.";
    }
    if (target_area <= source_area || form.width <= original.w || form.height <= original.h) {
        return "AI upscale needs a larger width and height than the source.";
    }
    if (cross > std::max(original.w, original.h)) {
        return "AI upscale must preserve the source aspect ratio.";
    }
    if (native == 0) return "This AI Upscaler has no known native scaling factor.";
    if (form.width > static_cast<std::int64_t>(original.w) * native ||
        form.height > static_cast<std::int64_t>(original.h) * native) {
        return "This AI Upscaler supports up to " + std::to_string(native) +
               "× the source dimensions.";
    }
    return running ? "An AI upscale is already in progress." : std::string();
}

std::vector<std::string> upscale_candidates(const AppState& state) {
    if (state.ai.registry == nullptr) return {};
    return state.ai.registry->candidates("upscale", true);
}

void draw_upscaler_picker(AppState& state, ImageSizeForm& form,
                          const std::vector<std::string>& candidates,
                          const std::string& selected_model) {
    ImGui::BeginDisabled(form.whole_stack);
    ImGui::TextUnformatted("AI Upscaler");
    if (candidates.empty()) {
        muted_wrapped("No AI upscalers available.");
    } else {
        ImGui::SetNextItemWidth(-1.0f);
        if (ImGui::BeginCombo("##ai_upscaler", selected_model.c_str())) {
            for (const std::string& candidate : candidates) {
                const int factor = ai::native_upscale_factor(candidate);
                const ai::ModelEntry* entry = state.ai.registry->model(candidate);
                const std::string label = candidate + (factor > 0
                    ? " (" + std::to_string(factor) + "× native)"
                    : " (scale unknown)") +
                    (entry != nullptr && !entry->downloaded ? " · not downloaded" : "");
                if (ImGui::Selectable(label.c_str(), candidate == selected_model)) {
                    form.model = candidate;
                    form.upscale_error.clear();
                }
            }
            ImGui::EndCombo();
        }
    }
    ImGui::EndDisabled();
}

bool upscale_running(const AppState& state) {
    if (!state.ai.jobs) return false;
    for (const ai::JobRecord& job : state.ai.jobs->all()) {
        if (job.recipe_id == "upscale" &&
            (job.state == ai::JobState::queued || job.state == ai::JobState::running)) {
            return true;
        }
    }
    return false;
}

void draw_deferred_upscale(AppState& state, ImageSizeForm& form,
                           CommandTable& commands) {
    if (form.whole_stack) return;
    ImGui::SeparatorText("Next AI Edit");
    ImGui::Checkbox("Use this size for the next AI edit", &form.use_for_next_edit);
    const bool armed = state.deferred_upscale &&
        state.deferred_upscale->document_epoch == state.document_epoch &&
        state.deferred_upscale->source == state.doc.size();
    if (armed) {
        const Extent target = state.deferred_upscale->target;
        muted_wrapped("Next Change, Fill, Remove or Heal generates " +
                      std::to_string(target.w) + " × " + std::to_string(target.h) +
                      " in one model request.");
    } else {
        muted_wrapped("The next AI edit generates the full image at the chosen size.");
    }
    if (form.use_for_next_edit) {
        const Extent source = state.doc.size();
        const std::int64_t target_area = static_cast<std::int64_t>(form.width) * form.height;
        const std::int64_t source_area = static_cast<std::int64_t>(source.w) * source.h;
        const bool valid_target = valid_image_target(form) && target_area > source_area;
        ImGui::BeginDisabled(!valid_target);
        if (ImGui::Button("Set Next Edit Size")) {
            const CommandResult result = commands.invoke("image.upscale.defer",
                json{{"width", form.width}, {"height", form.height}});
            form.deferred_error = result.ok ? "" : result.error;
            if (result.ok) close_dialog();
        }
        ImGui::EndDisabled();
        if (!valid_target) muted_wrapped("Choose a larger valid target size first.");
    } else if (armed && ImGui::Button("Clear Next Edit Size")) {
        const CommandResult result = commands.invoke("image.upscale.defer",
            json{{"enabled", false}});
        form.deferred_error = result.ok ? "" : result.error;
        if (result.ok) close_dialog();
    }
    if (!form.deferred_error.empty()) ImGui::TextWrapped("%s", form.deferred_error.c_str());
}

void draw_image_size_dialog(AppState& state, CommandTable& commands) {
    initialize_image_form(state);
    ImageSizeForm& form = state.image_size_form;
    const Extent source = state.doc.size();
    draw_image_dimensions(form, source);
    draw_scale_presets(form, source);
    if (ImGui::Checkbox("Entire layer stack", &form.whole_stack) && form.whole_stack) {
        form.use_for_next_edit = false;
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
        ImGui::SetTooltip("Preserve individually editable layers at the new size (Lanczos3 only)");
    }
    if (form.whole_stack) muted_wrapped("Entire layer stack is available with Lanczos3 only.");

    const bool valid = valid_image_target(form);
    const bool changed = form.width != source.w || form.height != source.h;
    ImGui::BeginDisabled(!valid || !changed);
    if (ui_icons::labeled_button(ui_icons::Icon::Scaling, "Resize (Lanczos3)",
                                 ImVec2(0.0f, 0.0f))) {
        const CommandResult result = commands.invoke("doc.resample",
            json{{"width", form.width}, {"height", form.height},
                 {"whole_stack", form.whole_stack}});
        form.resize_error = result.ok ? "" : result.error;
        if (result.ok) close_dialog();
    }
    ImGui::EndDisabled();
    if (!form.resize_error.empty()) muted_wrapped(form.resize_error);
    ImGui::Separator();
    if (ImGui::Button("Cancel")) close_dialog();
}

void draw_upscale_dialog(AppState& state, CommandTable& commands) {
    initialize_image_form(state);
    ImageSizeForm& form = state.image_size_form;
    const Extent source = state.doc.size();
    draw_image_dimensions(form, source);
    draw_scale_presets(form, source);

    const std::vector<std::string> candidates = upscale_candidates(state);
    const Readiness ready = assess(state, state.ai.recipes.find("upscale"));
    const std::string selected_model = !form.model.empty() ? form.model :
        std::find(candidates.begin(), candidates.end(), ready.model) != candidates.end()
            ? ready.model : candidates.empty() ? std::string{} : candidates.front();
    const bool valid_model = std::find(candidates.begin(), candidates.end(), selected_model) !=
                             candidates.end();
    const int native = ai::native_upscale_factor(selected_model);
    draw_upscaler_picker(state, form, candidates, selected_model);
    const bool running = upscale_running(state);
    const std::string problem = upscale_problem(state, form, selected_model, valid_model,
                                                native, running);
    ImGui::BeginDisabled(!problem.empty());
    if (ui_icons::labeled_button(ui_icons::Icon::ImageUpscale, "Upscale (AI)",
                                 ImVec2(0.0f, 0.0f))) {
        const CommandResult result = commands.invoke("image.upscale",
            json{{"width", form.width}, {"height", form.height}, {"model", selected_model}});
        form.upscale_error = result.ok ? "" : result.error;
        if (result.ok) close_dialog();
    }
    ImGui::EndDisabled();
    if (!problem.empty()) muted_wrapped(problem);
    if (!form.resize_error.empty()) muted_wrapped(form.resize_error);
    if (!form.upscale_error.empty()) ImGui::TextWrapped("AI upscale: %s", form.upscale_error.c_str());
    draw_deferred_upscale(state, form, commands);
    ImGui::Separator();
    if (ImGui::Button("Cancel")) close_dialog();
}

void begin_dialog_host(const ImGuiViewport* viewport) {
    ImGui::SetNextWindowPos(viewport->Pos);
    ImGui::SetNextWindowSize(viewport->Size);
    ImGui::SetNextWindowViewport(viewport->ID);
    ImGui::Begin("##image_dialog_host", nullptr, kDialogHostFlags);
}

}

void request_image_dialog(ImageDialog dialog) {
    requested_dialog = dialog;
}

void close_image_dialog() {
    close_dialog();
}

void draw_image_dialogs(AppState& state, CommandTable& commands) {
    if (state.doc.empty()) {
        requested_dialog.reset();
        active_dialog.reset();
        return;
    }
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    begin_dialog_host(viewport);
    if (requested_dialog.has_value()) {
        active_dialog = requested_dialog;
        requested_dialog.reset();
        if (*active_dialog == ImageDialog::image_size ||
            *active_dialog == ImageDialog::upscale) {
            reset_image_form(state);
            const bool armed = state.deferred_upscale &&
                state.deferred_upscale->document_epoch == state.document_epoch &&
                state.deferred_upscale->source == state.doc.size();
            state.image_size_form.use_for_next_edit =
                *active_dialog == ImageDialog::upscale && armed;
        } else {
            reset_canvas_dialog(state);
        }
        ImGui::OpenPopup(dialog_name(*active_dialog));
    }
    if (active_dialog.has_value()) {
        const ImVec2 center(viewport->Pos.x + viewport->Size.x * 0.5f,
                            viewport->Pos.y + viewport->Size.y * 0.5f);
        ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowSize(ImVec2(kDialogWidthPx, 0.0f), ImGuiCond_Appearing);
        bool open = true;
        if (ImGui::BeginPopupModal(dialog_name(*active_dialog), &open, kDialogFlags)) {
            switch (*active_dialog) {
                case ImageDialog::image_size: draw_image_size_dialog(state, commands); break;
                case ImageDialog::upscale: draw_upscale_dialog(state, commands); break;
                case ImageDialog::canvas_size: draw_canvas_size_dialog(state, commands); break;
                case ImageDialog::crop: draw_crop_dialog(state, commands); break;
            }
            ImGui::EndPopup();
        }
        if (!open) active_dialog.reset();
    }
    ImGui::End();
}

}
