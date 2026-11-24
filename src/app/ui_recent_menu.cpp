// SPDX-License-Identifier: Apache-2.0
#include "app/ui_internal.h"
#include "app/ui_text.h"

#include "imgui.h"
#include "platform/paths.h"

#include <algorithm>
#include <filesystem>
#include <limits>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace noise::app {
namespace {

constexpr float kRecentMenuMaxWidthPx = 380.0f;
constexpr float kRecentMenuMaxLabelWidthPx = 300.0f;
constexpr float kRecentHintWidthFraction = 0.35f;
constexpr char kRecentHintSeparator[] = " — ";
std::string shorten_label(std::string_view label, float max_width) {
    if (ImGui::CalcTextSize(label.data(), label.data() + label.size()).x <= max_width) {
        return std::string(label);
    }
    const std::size_t separator = label.find(kRecentHintSeparator);
    if (separator == std::string_view::npos) {
        return ui_text::truncate_filename(label, max_width);
    }
    const std::string_view filename = label.substr(0, separator);
    const std::string_view hint = label.substr(separator + sizeof(kRecentHintSeparator) - 1);
    const float hint_width = std::min(
        ImGui::CalcTextSize(hint.data(), hint.data() + hint.size()).x,
        max_width * kRecentHintWidthFraction);
    const float name_width = std::max(
        ImGui::CalcTextSize("…").x,
        max_width - ImGui::CalcTextSize(kRecentHintSeparator).x - hint_width);
    return ui_text::truncate_filename(filename, name_width) + kRecentHintSeparator +
           ui_text::truncate_text(hint, hint_width);
}

std::string label_for_path(const std::string& path, std::size_t index,
                           std::size_t basename_count,
                           std::unordered_set<std::string>& used_labels) {
    const std::filesystem::path file = platform::path_from_utf8(path);
    const std::string filename = platform::path_to_utf8(file.filename());
    if (basename_count == 1 && !used_labels.contains(filename)) {
        used_labels.insert(filename);
        return filename;
    }

    std::string hint;
    std::filesystem::path parent = file.parent_path();
    while (!parent.empty()) {
        const std::string part = platform::path_to_utf8(parent.filename());
        if (!part.empty()) {
            hint = hint.empty() ? part : part + "/" + hint;
            const std::string label = filename + kRecentHintSeparator + hint;
            if (!used_labels.contains(label)) {
                used_labels.insert(label);
                return label;
            }
        }
        const std::filesystem::path next = parent.parent_path();
        if (next == parent) {
            break;
        }
        parent = next;
    }

    const std::string label = filename + " (#" + std::to_string(index + 1) + ")";
    used_labels.insert(label);
    return label;
}

struct RecentFile {
    std::string path;
    std::string label;
};

std::vector<RecentFile> recent_files(const nlohmann::json& entries) {
    std::unordered_map<std::string, std::size_t> counts;
    std::vector<std::string> paths;
    paths.reserve(entries.size());
    for (const nlohmann::json& entry : entries) {
        paths.push_back(entry.get<std::string>());
        const std::filesystem::path file = platform::path_from_utf8(paths.back());
        ++counts[platform::path_to_utf8(file.filename())];
    }

    std::unordered_set<std::string> used_labels;
    std::vector<RecentFile> files;
    files.reserve(paths.size());
    for (std::size_t index = 0; index < paths.size(); ++index) {
        const std::filesystem::path file = platform::path_from_utf8(paths[index]);
        const std::string filename = platform::path_to_utf8(file.filename());
        files.push_back(RecentFile{paths[index],
                                   label_for_path(paths[index], index, counts[filename],
                                                  used_labels)});
    }
    return files;
}

}

void draw_recent_files_menu(CommandTable& commands) {
    const nlohmann::json entries = commands.invoke("file.recent", nlohmann::json::object())
                                       .value.value("files", nlohmann::json::array());
    if (entries.empty()) {
        return;
    }
    ImGui::SetNextWindowSizeConstraints(
        ImVec2(0.0f, 0.0f),
        ImVec2(kRecentMenuMaxWidthPx, std::numeric_limits<float>::max()));
    if (!ImGui::BeginMenu("Open Recent")) {
        return;
    }
    const std::vector<RecentFile> files = recent_files(entries);
    for (std::size_t index = 0; index < files.size(); ++index) {
        const RecentFile& file = files[index];
        const std::string shown = shorten_label(file.label, kRecentMenuMaxLabelWidthPx);
        const std::string id = shown + "##recent_" + std::to_string(index);
        if (ImGui::MenuItem(id.c_str())) {
            commands.invoke("doc.open", nlohmann::json{{"path", file.path}});
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
            ImGui::SetTooltip("%s", file.path.c_str());
        }
    }
    ImGui::EndMenu();
}

void draw_recent_files_list(CommandTable& commands, std::string& open_error) {
    const nlohmann::json entries = commands.invoke("file.recent", nlohmann::json::object())
                                       .value.value("files", nlohmann::json::array());
    if (entries.empty()) {
        ImGui::TextDisabled("No recent files yet.");
        return;
    }
    const std::vector<RecentFile> files = recent_files(entries);
    constexpr float kRecentRowHeightPx = 30.0f;
    constexpr float kRecentRowPaddingPx = 12.0f;
    for (std::size_t index = 0; index < files.size(); ++index) {
        const RecentFile& file = files[index];
        const float width = std::max(0.0f, ImGui::GetContentRegionAvail().x -
                                             kRecentRowPaddingPx);
        const std::string shown = shorten_label(file.label, width);
        ImGui::PushID(static_cast<int>(index));
        if (ImGui::Selectable(shown.c_str(), false, ImGuiSelectableFlags_None,
                              ImVec2(0.0f, kRecentRowHeightPx))) {
            const CommandResult result = commands.invoke(
                "doc.open", nlohmann::json{{"path", file.path}});
            if (result.ok) {
                open_error.clear();
                commands.invoke("front_door", nlohmann::json{{"show", "close"}});
            } else {
                open_error = result.error;
            }
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
            ImGui::SetTooltip("%s", file.path.c_str());
        }
        ImGui::PopID();
    }
}

}
