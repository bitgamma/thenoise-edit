// SPDX-License-Identifier: Apache-2.0
#include "app/ui_internal.h"

#include "app/ui_theme.h"

#include "imgui.h"

#include <algorithm>
#include <cfloat>
#include <string>
#include <vector>

namespace noise::app {

using nlohmann::json;

void draw_verb_section(AppState& state, CommandTable& commands, const json& intent,
                       io::Theme theme);

namespace {

constexpr float kAiWorkspaceWidthPx = 390.0f;
constexpr float kAiWorkspaceHeightPx = 680.0f;
constexpr float kAiWorkspaceMinimumWidthPx = 280.0f;
constexpr float kAiWorkspaceMinimumHeightPx = 300.0f;

void draw_latest_ai_landing_error(const AppState& state) {
    if (state.ai.jobs == nullptr || state.doc.empty()) return;
    const std::vector<ai::JobRecord> jobs = state.ai.jobs->all();
    for (auto it = jobs.rbegin(); it != jobs.rend(); ++it) {
        if (it->landing.document_epoch != state.document_epoch) continue;
        if (it->landing.land_refused) {
            ImGui::PushStyleColor(ImGuiCol_Text, ui_theme::palette(state.ui_theme).danger);
            ImGui::TextWrapped("AI result %s could not be applied: %s", it->id.c_str(),
                               it->landing.land_error.c_str());
            ImGui::PopStyleColor();
        }
        break;
    }
}

}

void draw_ai_workspace(AppState& state, CommandTable& commands, const json& intent,
                       ReferenceThumbs& thumbs, ImGuiID default_dock) {
    ImGui::SetNextWindowSize(ImVec2(kAiWorkspaceWidthPx, kAiWorkspaceHeightPx),
                             ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSizeConstraints(
        ImVec2(kAiWorkspaceMinimumWidthPx, kAiWorkspaceMinimumHeightPx),
        ImVec2(FLT_MAX, FLT_MAX));
    if (default_dock != 0 && !state.ai_workspace_placement_initialized) {
        ImGui::SetNextWindowDockID(default_dock, ImGuiCond_Always);
        state.ai_workspace_placement_initialized = true;
        state.ai_workspace_settings_dirty = true;
    }
    if (state.ai_workspace_focus_requested) {
        ImGui::SetNextWindowFocus();
        state.ai_workspace_focus_requested = false;
    }
    const bool was_open = state.ai_workspace_open;
    const bool visible = ImGui::Begin(kWindowAiWorkspace, &state.ai_workspace_open,
                                     ImGuiWindowFlags_NoCollapse);
    state.ai_workspace_visible = visible;
    state.ai_workspace_docked = ImGui::IsWindowDocked();
    const ImVec2 pos = ImGui::GetWindowPos();
    const ImVec2 size = ImGui::GetWindowSize();
    state.ai_workspace_window = ScreenRect{pos.x, pos.y, size.x, size.y};
    if (was_open != state.ai_workspace_open) {
        state.ai_workspace_settings_dirty = true;
    }
    if (!visible) {
        ImGui::End();
        return;
    }
    draw_ai_status_line(state, commands, intent);
    draw_latest_ai_landing_error(state);
    ImGui::Separator();
    if (!state.doc.empty()) {
        draw_ai_references(state, commands, thumbs, intent);
        ImGui::Separator();
        draw_verb_section(state, commands, intent, state.ui_theme);
    } else {
        ImGui::TextDisabled("Open or create a document to use AI editing.");
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
        ImGui::TextDisabled("%d × %d%s", quote["size"][0].get<int>(),
                            quote["size"][1].get<int>(),
                            quote["resampled"].get<bool>() ? " rescaled" : "");
    }
    if (view["swap"].get<bool>()) {
        ImGui::SameLine();
        ImGui::TextColored(colors.warning,
                           "load %s (currently %s)", view["model"].get<std::string>().c_str(),
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
            ImGui::TextDisabled("working: %s · %d s", job.value("recipe", std::string()).c_str(),
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
            ImGui::TextColored(colors.danger, "result not applied (see AI Workspace)");
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

}

void draw_ai_status_line(AppState& state, CommandTable& commands, const json& view) {
    const ui_theme::Palette& colors = ui_theme::palette(state.ui_theme);
    if (!view["configured"].get<bool>()) {
        ImGui::TextDisabled("AI offline · set $THENOISE_URL");
        return;
    }
    if (!view["connected"].get<bool>()) {
        ImGui::TextColored(colors.danger, "AI offline");
        ImGui::SameLine();
        if (ImGui::SmallButton("Connect")) {
            commands.invoke("ai.intent", json{{"connect", true}});
        }
        return;
    }
    if (view["label"].is_null() || view["model"].get<std::string>().empty()) {
        ImGui::TextDisabled("AI: %s", view["reason"].get<std::string>().c_str());
        return;
    }
    ImGui::Text("AI: %s", view["label"].get<std::string>().c_str());
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
    if (view["job"].is_object()) draw_job_status(state, commands, view["job"], colors);
    const std::string error = view["error"].get<std::string>();
    if (!error.empty()) {
        ImGui::SameLine();
        ImGui::TextColored(colors.danger, "%s", error.c_str());
    }
}

}
