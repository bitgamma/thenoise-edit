// SPDX-License-Identifier: Apache-2.0
#include "app/ui.h"

#include "app/ui_internal.h"
#include "app/ui_theme.h"
#include "app/verb_bar.h"

#include "imgui.h"
#include "imgui_internal.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <string>

namespace noise::app {

using nlohmann::json;

namespace {

constexpr float kRightPanelDefaultWidthPx = 320.0f;
constexpr float kRightPanelMinimumWidthPx = 280.0f;
constexpr float kRightPanelMaximumFraction = 0.45f;
constexpr float kRecoveredPanelHeightPx = 480.0f;
constexpr float kRecoveredPanelMarginPx = 12.0f;
constexpr float kZoomSelectorWidthPx = 86.0f;
constexpr float kTitlebarLeftPaddingPx = 14.0f;
constexpr float kTitlebarLogoSeparatorGapPx = 12.0f;
constexpr float kTitlebarMenuGapWidePx = 12.0f;
constexpr float kTitlebarMenuGapCompactPx = 7.0f;
constexpr float kTitlebarCompactWidthPx = 940.0f;
constexpr float kTitlebarMenuPaddingXPx = 10.0f;
constexpr float kTitlebarMenuSpacingYPx = 4.0f;
constexpr float kTitlebarZoomPaddingYPx = 7.0f;
constexpr float kTitlebarZoomPaddingXPx = 11.0f;
constexpr float kTitlebarPopupPaddingXPx = 10.0f;
constexpr float kTitlebarPopupPaddingYPx = 8.0f;
constexpr float kTitlebarMenuFontPx = 16.5f;
constexpr double kZoomAt100Percent = 1.0;
constexpr double kZoomAt200Percent = 2.0;
constexpr double kZoomSelectionTolerance = 0.005;
constexpr float kStatusBarHeightPx = 30.0f;
constexpr float kRootBorderPx = 0.0f;
constexpr float kRootRoundingPx = 0.0f;
constexpr const char* kDockspaceName = "TheNoise-Edit Dockspace";
constexpr std::size_t kPrimaryShortcutBinding = 0;
constexpr std::size_t kSecondaryShortcutBinding = 1;
constexpr std::array<const char*, 3> kAuxDockWindows = {kWindowLayers, kWindowLog,
                                                      kWindowAiWorkspace};

bool about_open = false;

void build_default_dock(ImGuiID dockspace, const ImVec2& size) {
    ImGui::DockBuilderRemoveNode(dockspace);
    ImGui::DockBuilderAddNode(dockspace, ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderSetNodeSize(dockspace, size);

    ImGuiID right = 0;
    ImGuiID down = 0;
    ImGuiID main_area = dockspace;
    const float right_fraction = std::min(
        kRightPanelDefaultWidthPx / std::max(size.x, 1.0f), kRightPanelMaximumFraction);
    ImGui::DockBuilderSplitNode(main_area, ImGuiDir_Right, right_fraction, &right,
                                &main_area);
    ImGui::DockBuilderSplitNode(main_area, ImGuiDir_Down,
                                std::clamp(kStatusBarHeightPx / std::max(size.y, 1.0f),
                                           0.02f, 0.15f),
                                &down, &main_area);
    ImGui::DockBuilderGetNode(down)->LocalFlags |= ImGuiDockNodeFlags_NoTabBar;
    ImGui::DockBuilderGetNode(main_area)->LocalFlags |= ImGuiDockNodeFlags_NoTabBar;

    ImGui::DockBuilderGetNode(right)->LocalFlags |= ImGuiDockNodeFlags_NoWindowMenuButton;
    ImGui::DockBuilderDockWindow(kWindowCanvas, main_area);
    ImGui::DockBuilderDockWindow(kWindowLayers, right);
    ImGui::DockBuilderDockWindow(kWindowAiWorkspace, right);
    ImGui::DockBuilderDockWindow(kWindowLog, right);
    ImGui::DockBuilderDockWindow(kWindowStatus, down);
    ImGui::DockBuilderFinish(dockspace);
}

ImGuiID recovery_dock(ImGuiID dockspace, ImGuiID canvas_dock, const char* name) {
    for (const char* candidate : kAuxDockWindows) {
        if (std::string_view(candidate) == name) continue;
        const ImGuiWindow* window = ImGui::FindWindowByName(candidate);
        if (window == nullptr || window->DockId == 0 || window->DockId == canvas_dock) continue;
        ImGuiDockNode* node = ImGui::DockBuilderGetNode(window->DockId);
        if (node != nullptr && ImGui::DockNodeGetRootNode(node)->ID == dockspace) {
            return window->DockId;
        }
    }
    return 0;
}

bool recover_canvas_docked_panels(ImGuiID dockspace) {
    const ImGuiDockNode* central = ImGui::DockBuilderGetCentralNode(dockspace);
    const ImGuiWindow* canvas = ImGui::FindWindowByName(kWindowCanvas);
    if (central == nullptr || canvas == nullptr || canvas->DockId != central->ID) return false;

    bool layers_recovered = false;
    for (const char* name : kAuxDockWindows) {
        const ImGuiWindow* panel = ImGui::FindWindowByName(name);
        if (panel == nullptr || panel->DockId != central->ID) continue;
        if (std::string_view(name) == kWindowLayers) layers_recovered = true;
        const ImGuiID target = recovery_dock(dockspace, central->ID, name);
        ImGui::DockBuilderDockWindow(name, target);
        if (target != 0) continue;
        const ImGuiViewport* viewport = ImGui::GetMainViewport();
        const ImVec2 size(std::min(kRightPanelDefaultWidthPx, viewport->WorkSize.x * 0.5f),
                          std::min(kRecoveredPanelHeightPx, viewport->WorkSize.y * 0.75f));
        ImGui::SetWindowSize(name, size, ImGuiCond_Always);
        ImGui::SetWindowPos(name,
            ImVec2(viewport->WorkPos.x + viewport->WorkSize.x - size.x - kRecoveredPanelMarginPx,
                   viewport->WorkPos.y + kClientTitlebarHeightPx + kRecoveredPanelMarginPx),
            ImGuiCond_Always);
    }
    return layers_recovered;
}

std::string shortcut_for(const CommandTable& commands, const char* id,
                         std::size_t binding) {
    const std::vector<std::string> shortcuts = commands.shortcuts_for(id);
    return binding < shortcuts.size() ? shortcuts[binding] : std::string();
}

bool menu_command(CommandTable& commands, const char* label, const char* id,
                  const json& args, bool enabled, std::size_t binding) {
    const std::string shortcut = shortcut_for(commands, id, binding);
    if (!ImGui::MenuItem(label, shortcut.empty() ? nullptr : shortcut.c_str(), false, enabled)) {
        return false;
    }
    commands.invoke(id, args);
    return true;
}

void pick_file(CommandTable& commands, const char* kind, const char* title) {
    commands.invoke("file.pick", json{{"for", kind}, {"title", title}});
}

void draw_file_menu(AppState& state, CommandTable& commands) {
    if (!ImGui::BeginMenu("File")) {
        return;
    }
    const bool has_document = !state.doc.empty();
    const bool can_save = has_document && !state.doc.path().empty();
    menu_command(commands, "New...", "file.new", json::object(), true,
                 kPrimaryShortcutBinding);
    if (ImGui::MenuItem("Open...")) {
        pick_file(commands, "open", "Open an image or a document");
    }
    menu_command(commands, "Save", "doc.save", json::object(), can_save,
                 kPrimaryShortcutBinding);
    if (ImGui::MenuItem("Save As...", nullptr, false, has_document)) {
        pick_file(commands, "project", "Save document");
    }
    if (ImGui::MenuItem("Export Image...", nullptr, false, has_document)) {
        pick_file(commands, "flat", "Export an image");
    }
    if (ImGui::MenuItem("Insert Image as Layer...", nullptr, false, has_document)) {
        pick_file(commands, "image", "Insert an image");
    }
    draw_recent_files_menu(commands);
    ImGui::Separator();
    menu_command(commands, "Quit", "app.quit", json::object(), true,
                 kSecondaryShortcutBinding);
    ImGui::EndMenu();
}

void draw_edit_menu(AppState& state, CommandTable& commands) {
    if (!ImGui::BeginMenu("Edit")) {
        return;
    }
    menu_command(commands, "Undo", "edit.undo", json::object(),
                 !state.doc.empty() && state.doc.can_undo(), kPrimaryShortcutBinding);
    menu_command(commands, "Redo", "edit.redo", json::object(),
                 !state.doc.empty() && state.doc.can_redo(), kPrimaryShortcutBinding);
    ImGui::EndMenu();
}

void draw_image_menu(AppState& state, CommandTable& commands) {
    if (!ImGui::BeginMenu("Image")) {
        return;
    }
    const bool has_document = !state.doc.empty();
    menu_command(commands, "Image Size...", "ui.image_size", json::object(), has_document,
                 kPrimaryShortcutBinding);
    menu_command(commands, "Canvas Size...", "ui.canvas_size", json::object(), has_document,
                 kPrimaryShortcutBinding);
    menu_command(commands, "Crop...", "ui.crop", json::object(), has_document,
                 kPrimaryShortcutBinding);
    menu_command(commands, "Upscale...", "ui.upscale", json::object(), has_document,
                 kPrimaryShortcutBinding);
    if (ImGui::BeginMenu("Rotate", has_document)) {
        if (ImGui::MenuItem("90° Counterclockwise")) {
            commands.invoke("doc.rotate", json{{"direction", "ccw"}});
        }
        if (ImGui::MenuItem("90° Clockwise")) {
            commands.invoke("doc.rotate", json{{"direction", "cw"}});
        }
        ImGui::EndMenu();
    }
    ImGui::EndMenu();
}

void draw_layer_menu(AppState& state, CommandTable& commands) {
    if (!ImGui::BeginMenu("Layer")) {
        return;
    }
    const bool has_document = !state.doc.empty();
    const bool has_multiple = has_document && state.doc.layers().size() > 1;
    const int active = has_document ? state.doc.active() : -1;
    menu_command(commands, "Add Layer", "layer.add", json::object(), has_document,
                 kPrimaryShortcutBinding);
    menu_command(commands, "Delete Layer", "layer.remove", json::object(), has_multiple,
                 kPrimaryShortcutBinding);
    menu_command(commands, "Merge Down", "layer.merge", json::object(), active > 0,
                 kPrimaryShortcutBinding);
    menu_command(commands, "Flatten Image", "layer.flatten", json::object(), has_multiple,
                 kPrimaryShortcutBinding);
    const bool has_mask = has_document && state.active_layer().mask.has_value();
    if (ImGui::BeginMenu("Mask", has_document)) {
        if (ImGui::MenuItem("Adjust Layer Edge...", nullptr, false, has_mask)) {
            const CommandResult result = commands.invoke(
                "layer.plane", json{{"index", active}, {"plane", "mask"}});
            if (result.ok) request_mask_edge_controls_open();
        }
        ImGui::EndMenu();
    }
    ImGui::EndMenu();
}

void draw_select_menu(AppState& state, CommandTable& commands) {
    if (!ImGui::BeginMenu("Select")) {
        return;
    }
    const bool has_document = !state.doc.empty();
    menu_command(commands, "All", "select.all", json::object(), has_document,
                 kPrimaryShortcutBinding);
    menu_command(commands, "None", "select.clear", json::object(), has_document,
                 kPrimaryShortcutBinding);
    menu_command(commands, "Invert", "select.invert", json::object(), has_document,
                 kPrimaryShortcutBinding);
    ImGui::Separator();
    const bool has_mask = has_document && state.active_layer().mask.has_value();
    menu_command(commands, "To Layer Mask", "mask.from_selection", json::object(),
                 has_document && state.doc.selection().has_value(), kPrimaryShortcutBinding);
    menu_command(commands, "From Layer Mask", "select.from_mask", json::object(), has_mask,
                 kPrimaryShortcutBinding);
    ImGui::EndMenu();
}

void set_zoom(AppState& state, CommandTable& commands, double scale);

void draw_view_menu(AppState& state, CommandTable& commands) {
    if (!ImGui::BeginMenu("View")) {
        return;
    }
    menu_command(commands, "Fit Image", "view.fit", json::object(), !state.doc.empty(),
                 kPrimaryShortcutBinding);
    ImGui::Separator();
    const bool has_document = !state.doc.empty();
    if (ImGui::MenuItem("100%", nullptr, false, has_document)) {
        set_zoom(state, commands, kZoomAt100Percent);
    }
    if (ImGui::MenuItem("200%", nullptr, false, has_document)) {
        set_zoom(state, commands, kZoomAt200Percent);
    }
    ImGui::EndMenu();
}

void set_zoom(AppState& state, CommandTable& commands, double scale) {
    if (state.doc.empty() || state.view.scale <= 0.0) return;
    commands.invoke("view.zoom", json{{"factor", scale / state.view.scale},
        {"anchor", json::array({static_cast<double>(state.viewport.w) * 0.5,
                                static_cast<double>(state.viewport.h) * 0.5})}});
}

void draw_zoom_selector(AppState& state, CommandTable& commands, bool client_titlebar) {
    const std::string label = state.doc.empty()
        ? "Zoom" : std::to_string(static_cast<int>(std::lround(state.view.scale * 100.0))) + "%";
    if (client_titlebar) {
        ImGui::PushStyleColor(ImGuiCol_FrameBg, ui_theme::palette(state.ui_theme).surface);
        ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ui_theme::palette(state.ui_theme).raised);
        ImGui::PushStyleColor(ImGuiCol_FrameBgActive, ui_theme::palette(state.ui_theme).raised);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 3.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,
                            ImVec2(kTitlebarZoomPaddingXPx, kTitlebarZoomPaddingYPx));
        ImGui::SetCursorPosY((kClientTitlebarHeightPx - ImGui::GetFrameHeight()) * 0.5f);
    }
    ImGui::SetNextItemWidth(kZoomSelectorWidthPx);
    ImGui::BeginDisabled(state.doc.empty());
    const bool is_open = ImGui::BeginCombo("##zoom_selector", label.c_str(),
                                         client_titlebar ? ImGuiComboFlags_NoArrowButton : 0);
    if (client_titlebar) {
        const ImVec2 min = ImGui::GetItemRectMin();
        const ImVec2 max = ImGui::GetItemRectMax();
        const float cx = max.x - 12.0f;
        const float cy = (min.y + max.y) * 0.5f;
        const ImU32 color = ImGui::GetColorU32(ImGuiCol_TextDisabled);
        ImDrawList* draw = ImGui::GetWindowDrawList();
        draw->AddLine(ImVec2(cx - 3.5f, cy - 1.5f), ImVec2(cx, cy + 2.0f), color, 1.5f);
        draw->AddLine(ImVec2(cx, cy + 2.0f), ImVec2(cx + 3.5f, cy - 1.5f), color, 1.5f);
    }
    if (is_open) {
        const ImVec2 row(0.0f, 28.0f);
        if (ImGui::Selectable("Fit", false, 0, row)) commands.invoke("view.fit", json::object());
        const bool at_100 = std::abs(state.view.scale - kZoomAt100Percent) <
                            kZoomSelectionTolerance;
        if (ImGui::Selectable("100%", at_100, 0, row)) {
            set_zoom(state, commands, kZoomAt100Percent);
        }
        const bool at_200 = std::abs(state.view.scale - kZoomAt200Percent) <
                            kZoomSelectionTolerance;
        if (ImGui::Selectable("200%", at_200, 0, row)) {
            set_zoom(state, commands, kZoomAt200Percent);
        }
        ImGui::EndCombo();
    }
    ImGui::EndDisabled();
    if (client_titlebar) {
        ImGui::PopStyleVar(3);
        ImGui::PopStyleColor(3);
    }
}

void draw_window_menu(AppState& state, CommandTable& commands) {
    if (!ImGui::BeginMenu("Window")) return;
    if (ImGui::MenuItem("AI Workspace", nullptr, state.ai_workspace_open)) {
        commands.invoke("ui.ai_workspace", json{{"open", true}, {"focus", true}});
    }
    ImGui::EndMenu();
}

void draw_help_menu() {
    if (!ImGui::BeginMenu("Help")) {
        return;
    }
    if (ImGui::MenuItem("About TheNoise-Edit")) {
        about_open = true;
    }
    ImGui::EndMenu();
}

void draw_application_menu(AppState& state, CommandTable& commands,
                           const gfx::Texture& brand_logo, bool client_titlebar,
                           std::string_view document_name) {
    if (client_titlebar) {
        const bool compact = ImGui::GetMainViewport()->WorkSize.x <
                             kTitlebarCompactWidthPx;
        const float menu_gap = compact ? kTitlebarMenuGapCompactPx :
                                         kTitlebarMenuGapWidePx;
        const float menu_padding_y = std::max(0.0f,
            (kClientTitlebarHeightPx - ImGui::GetFontSize()) * 0.25f);
        // The bar is edge-to-edge; its dropdown windows are not.
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,
                            ImVec2(kTitlebarPopupPaddingXPx, kTitlebarPopupPaddingYPx));
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,
                            ImVec2(kTitlebarMenuPaddingXPx, menu_padding_y));
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,
                            ImVec2(menu_gap, kTitlebarMenuSpacingYPx));
    }
    if (!ImGui::BeginMenuBar()) {
        if (client_titlebar) ImGui::PopStyleVar(3);
        return;
    }
    if (client_titlebar) {
        ImGui::SetCursorPosX(kTitlebarLeftPaddingPx);
    }
    draw_brand_mark(brand_logo, client_titlebar);
    if (client_titlebar) {
        ImGui::SameLine(0.0f, kTitlebarLogoSeparatorGapPx);
        const float separator_x = ImGui::GetCursorScreenPos().x;
        const float separator_y = ImGui::GetWindowPos().y +
                                  (kClientTitlebarHeightPx - 20.0f) * 0.5f;
        ImGui::GetWindowDrawList()->AddLine(
            ImVec2(separator_x, separator_y),
            ImVec2(separator_x, separator_y + 20.0f),
            ImGui::GetColorU32(ImGuiCol_Separator));
        ImGui::Dummy(ImVec2(1.0f, ImGui::GetFrameHeight()));
        ImGui::SameLine(0.0f, kTitlebarLogoSeparatorGapPx);
    } else {
        ImGui::SameLine();
        ImGui::SeparatorEx(ImGuiSeparatorFlags_Vertical);
        ImGui::SameLine();
    }
    const bool enlarged_menus = client_titlebar &&
        ImGui::GetMainViewport()->WorkSize.x >= kTitlebarCompactWidthPx;
    if (enlarged_menus) ImGui::PushFont(nullptr, kTitlebarMenuFontPx);
    const float menu_y = client_titlebar
        ? (kClientTitlebarHeightPx - ImGui::GetFrameHeight()) * 0.5f : 0.0f;
    const auto align_menu = [&]() {
        if (client_titlebar) ImGui::SetCursorPosY(menu_y);
    };
    align_menu(); draw_file_menu(state, commands);
    align_menu(); draw_edit_menu(state, commands);
    align_menu(); draw_image_menu(state, commands);
    align_menu(); draw_layer_menu(state, commands);
    align_menu(); draw_select_menu(state, commands);
    align_menu(); draw_view_menu(state, commands);
    align_menu(); draw_window_menu(state, commands);
    align_menu(); draw_help_menu();
    if (enlarged_menus) ImGui::PopFont();

    const float right_edge = ImGui::GetWindowContentRegionMax().x;
    const float controls_width = client_titlebar ? client_titlebar_controls_width() : 0.0f;
    const float zoom_left = right_edge - kZoomSelectorWidthPx - controls_width;
    if (client_titlebar) {
        ImGui::SameLine(0.0f, 0.0f);
        const float drag_width = std::max(0.0f, zoom_left - ImGui::GetCursorPosX());
        if (drag_width > 1.0f) {
            draw_client_titlebar_drag(document_name, drag_width);
        }
        ImGui::SameLine(0.0f, 0.0f);
    }
    if (zoom_left >= ImGui::GetCursorPosX()) {
        ImGui::SetCursorPosX(zoom_left);
        draw_zoom_selector(state, commands, client_titlebar);
    }
    if (client_titlebar) {
        ImGui::SameLine(0.0f, kClientTitlebarControlsGapPx);
        draw_client_window_controls(state, commands);
    }
    ImGui::EndMenuBar();
    if (client_titlebar) {
        ImGui::PopStyleVar(3);
    }
}

void draw_status_bar(AppState& state, const json& ai_view) {
    ImGui::Begin(kWindowStatus, nullptr, ImGuiWindowFlags_NoTitleBar |
                                         ImGuiWindowFlags_NoResize |
                                         ImGuiWindowFlags_NoMove |
                                         ImGuiWindowFlags_NoScrollbar);
    if (state.doc.empty()) {
        ImGui::TextDisabled("No document");
    } else {
        ImGui::Text("%d × %d px", state.doc.width(), state.doc.height());
        ImGui::SameLine();
        ImGui::TextDisabled("%.0f%%", state.view.scale * 100.0);
    }
    if (!state.doc.empty() && state.doc.selection()) {
        const Rect bounds = state.doc.selection()->bounds();
        ImGui::SameLine();
        ImGui::TextDisabled("Selection %d × %d at %d, %d", bounds.w, bounds.h,
                            bounds.x, bounds.y);
    }
    if (state.pointer.at.has_value()) {
        ImGui::SameLine();
        ImGui::TextDisabled("x %.0f · y %.0f", state.pointer.at->x, state.pointer.at->y);
    }
    if (state.stroke.active) {
        ImGui::SameLine();
        ImGui::TextDisabled("Painting · %zu dabs", state.stroke.dabs);
    }
    if (ai_view["job"].is_object()) {
        ImGui::SameLine();
        ImGui::TextDisabled("AI %s · %s", ai_view["label"].is_null()
            ? "ready" : ai_view["label"].get<std::string>().c_str(),
            ai_view["job"].value("state", std::string("working")).c_str());
    }
    ImGui::End();
}

void draw_about_window() {
    if (!about_open) {
        return;
    }
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(viewport->Pos.x + viewport->Size.x * 0.5f,
                                   viewport->Pos.y + viewport->Size.y * 0.5f),
                            ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(310.0f, 112.0f), ImGuiCond_Appearing);
    if (ImGui::Begin("About TheNoise-Edit", &about_open,
                     ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoDocking)) {
        ImGui::TextUnformatted("TheNoise-Edit");
        ImGui::TextDisabled("Version %s", kAppVersion);
        ImGui::TextDisabled("Layered raster editor");
    }
    ImGui::End();
}

void register_ai_workspace_command(AppState& state, CommandTable& commands) {
    commands.add("ui.ai_workspace", "Read, open or focus the AI Workspace", {},
                 {"open", "focus"}, [&state](const json& args) -> CommandResult {
        if (args.contains("open") && !args.at("open").is_boolean()) {
            return command_error("ui.ai_workspace: 'open' must be boolean");
        }
        if (args.contains("focus") && !args.at("focus").is_boolean()) {
            return command_error("ui.ai_workspace: 'focus' must be boolean");
        }
        const bool open = args.value("open", state.ai_workspace_open);
        const bool focus = args.value("focus", false);
        if (focus && !open) {
            return command_error("ui.ai_workspace: cannot focus a closed workspace");
        }
        if (state.ai_workspace_open != open) {
            state.ai_workspace_open = open;
            state.ai_workspace_settings_dirty = true;
        }
        if (focus) state.ai_workspace_focus_requested = true;
        if (!open) {
            state.ai_workspace_visible = false;
            state.ai_workspace_docked = false;
            state.ai_workspace_window = ScreenRect{};
            state.ai_workspace_focus_requested = false;
        }
        return command_ok(json{{"open", state.ai_workspace_open}});
    });
}

}

void register_ui_commands(AppState& state, CommandTable& commands) {
    register_ai_workspace_command(state, commands);
    commands.add("ui.image_size", "Open the Image Size dialog", {"Ctrl+Alt+I"}, {},
                 [](const json&) {
        request_image_dialog(ImageDialog::image_size);
        return command_ok(json{{"dialog", "image_size"}});
    });
    commands.add("ui.canvas_size", "Open the Canvas Size dialog", {"Ctrl+Alt+C"}, {},
                 [](const json&) {
        request_image_dialog(ImageDialog::canvas_size);
        return command_ok(json{{"dialog", "canvas_size"}});
    });
    commands.add("ui.crop", "Open the Crop dialog", {}, {}, [](const json&) {
        request_image_dialog(ImageDialog::crop);
        return command_ok(json{{"dialog", "crop"}});
    });
    commands.add("ui.upscale", "Open the AI Upscale dialog", {"Ctrl+Alt+U"}, {},
                 [](const json&) {
        request_image_dialog(ImageDialog::upscale);
        return command_ok(json{{"dialog", "upscale"}});
    });
}

void draw_ui(AppState& state, CommandTable& commands, gfx::Texture& canvas_texture,
             const gfx::Checker& checker, CoverageTint& tint, CoverageTint& mask_tint,
             LayerThumbs& thumbs, ReferenceThumbs& reference_thumbs,
             const gfx::Texture& brand_logo, bool client_titlebar,
             std::string_view document_name) {
    ui_theme::apply(state.ui_theme);
    if (state.layer_panel_settings_dirty || state.ai_workspace_settings_dirty) {
        ImGui::MarkIniSettingsDirty();
        state.layer_panel_settings_dirty = false;
        state.ai_workspace_settings_dirty = false;
    }
    state.ai_references_rect = ScreenRect{};
    draw_front_door(state, commands);
    draw_quit_guard(state, commands);

    ImGuiViewport* viewport = ImGui::GetMainViewport();
    if (client_titlebar) {
        begin_client_titlebar();
        draw_application_menu(state, commands, brand_logo, true, document_name);
        end_client_titlebar();
    }
    ImVec2 root_pos = viewport->WorkPos;
    ImVec2 root_size = viewport->WorkSize;
    if (client_titlebar) {
        root_pos.y += kClientTitlebarHeightPx;
        root_size.y = std::max(0.0f, root_size.y - kClientTitlebarHeightPx);
    }
    ImGui::SetNextWindowPos(root_pos);
    ImGui::SetNextWindowSize(root_size);
    ImGui::SetNextWindowViewport(viewport->ID);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, kRootRoundingPx);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, kRootBorderPx);
    ImGuiWindowFlags root_flags = ImGuiWindowFlags_NoTitleBar |
                                        ImGuiWindowFlags_NoResize |
                                        ImGuiWindowFlags_NoMove |
                                        ImGuiWindowFlags_NoCollapse |
                                        ImGuiWindowFlags_NoBringToFrontOnFocus |
                                        ImGuiWindowFlags_NoNavFocus |
                                        ImGuiWindowFlags_NoDocking |
                                        ImGuiWindowFlags_NoSavedSettings |
                                        ImGuiWindowFlags_NoBackground;
    if (!client_titlebar) root_flags |= ImGuiWindowFlags_MenuBar;
    ImGui::Begin("##noise_edit_root", nullptr, root_flags);
    if (!client_titlebar) {
        draw_application_menu(state, commands, brand_logo, false, document_name);
        ImGui::Separator();
    }
    const ImGuiID dockspace = ImGui::GetID(kDockspaceName);
    const ImVec2 dockspace_size = ImGui::GetContentRegionAvail();
    const ImGuiDockNode* saved_layout = ImGui::DockBuilderGetNode(dockspace);
    if (saved_layout == nullptr || !saved_layout->IsDockSpace()) {
        build_default_dock(dockspace, dockspace_size);
        state.layers_focus_requested = true;
        state.ai_workspace_placement_initialized = true;
        state.ai_workspace_settings_dirty = true;
    }
    const float minimum_dock_width = std::min(kRightPanelMinimumWidthPx,
                                               dockspace_size.x * 0.5f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowMinSize,
                        ImVec2(minimum_dock_width, ImGui::GetStyle().WindowMinSize.y));
    ImGui::DockSpace(dockspace, ImVec2(0.0f, 0.0f),
                     ImGuiDockNodeFlags_PassthruCentralNode |
                         ImGuiDockNodeFlags_NoDockingOverCentralNode);
    ImGui::PopStyleVar();
    ImGui::End();
    ImGui::PopStyleVar(3);
    if (recover_canvas_docked_panels(dockspace)) state.layers_focus_requested = true;

    draw_canvas(state, commands, canvas_texture, checker, tint, mask_tint);
    const json ai_view = commands.invoke("ai.intent", json::object()).value;
    draw_layers_panel(state, commands, thumbs);
    draw_log_panel();
    if (state.ai_workspace_open) {
        ImGuiID preferred_dock = 0;
        const ImGuiWindow* layers = ImGui::FindWindowByName(kWindowLayers);
        if (layers != nullptr) preferred_dock = layers->DockId;
        if (preferred_dock == 0) {
            const ImGuiWindow* log_window = ImGui::FindWindowByName(kWindowLog);
            if (log_window != nullptr) preferred_dock = log_window->DockId;
        }
        draw_ai_workspace(state, commands, ai_view, reference_thumbs, preferred_dock);
    } else {
        state.ai_workspace_visible = false;
        state.ai_workspace_docked = false;
        state.ai_workspace_window = ScreenRect{};
    }
    state.reference_thumb_rebuilds = reference_thumbs.rebuilds;
    draw_verb_bar(state, commands);
    draw_status_bar(state, ai_view);
    draw_image_dialogs(state, commands);
    draw_about_window();
    if (state.layers_focus_requested && state.front_door == FrontDoor::closed) {
        ImGui::SetWindowFocus(kWindowLayers);
        state.layers_focus_requested = false;
    }
}

}
