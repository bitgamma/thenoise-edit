// SPDX-License-Identifier: Apache-2.0
#include "app/ui.h"

#include "imgui.h"
#include "imgui_internal.h"

#include <cstring>

namespace noise::app {
namespace {

constexpr char kSettingsType[] = "Workspace";
constexpr char kSettingsEntry[] = "AI";
constexpr char kOpenKey[] = "Open=";
bool* workspace_open = nullptr;
bool* placement_initialized = nullptr;

void reset_settings(ImGuiContext*, ImGuiSettingsHandler*) {
    if (workspace_open != nullptr) *workspace_open = true;
    if (placement_initialized != nullptr) *placement_initialized = false;
}

void* open_settings(ImGuiContext*, ImGuiSettingsHandler*, const char* name) {
    return std::strcmp(name, kSettingsEntry) == 0 ? workspace_open : nullptr;
}

void read_settings(ImGuiContext*, ImGuiSettingsHandler*, void* entry, const char* line) {
    if (workspace_open == nullptr || entry != workspace_open) return;
    if (std::strncmp(line, kOpenKey, sizeof(kOpenKey) - 1) == 0) {
        const char* value = line + sizeof(kOpenKey) - 1;
        if (std::strcmp(value, "0") == 0) *workspace_open = false;
        if (std::strcmp(value, "1") == 0) *workspace_open = true;
    }
    if (placement_initialized != nullptr &&
        std::strcmp(line, "DockInitialized=1") == 0) {
        *placement_initialized = true;
    }
}

void write_settings(ImGuiContext*, ImGuiSettingsHandler*, ImGuiTextBuffer* out) {
    if (workspace_open != nullptr) {
        out->appendf("[Workspace][AI]\nOpen=%d\nDockInitialized=%d\n\n",
                     *workspace_open ? 1 : 0,
                     placement_initialized != nullptr && *placement_initialized ? 1 : 0);
    }
}

} // namespace

void register_ai_workspace_settings(bool& open, bool& initialized) {
    workspace_open = &open;
    placement_initialized = &initialized;
    if (ImGui::FindSettingsHandler(kSettingsType) != nullptr) return;
    ImGuiSettingsHandler handler{};
    handler.TypeName = kSettingsType;
    handler.TypeHash = ImHashStr(kSettingsType);
    handler.ClearAllFn = reset_settings;
    handler.ReadOpenFn = open_settings;
    handler.ReadLineFn = read_settings;
    handler.WriteAllFn = write_settings;
    ImGui::AddSettingsHandler(&handler);
}

} // namespace noise::app
