// SPDX-License-Identifier: Apache-2.0
#include "app/ui_internal.h"

#include "core/log.h"
#include "imgui.h"
#include "imgui_internal.h"

namespace noise::app {

void draw_log_panel() {
    ImGuiWindowClass dock_class{};
    dock_class.DockNodeFlagsOverrideSet = ImGuiDockNodeFlags_NoWindowMenuButton;
    ImGui::SetNextWindowClass(&dock_class);
    if (!ImGui::Begin(kWindowLog)) {
        ImGui::End();
        return;
    }
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
