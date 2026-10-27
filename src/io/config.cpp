// SPDX-License-Identifier: Apache-2.0
#include "io/config.h"

#include "io/file.h"
#include "platform/platform.h"
#include "platform/paths.h"
#ifdef __EMSCRIPTEN__
#include "platform/web.h"
#endif

#include <nlohmann/json.hpp>

#include <filesystem>
#include <cstdlib>

namespace noise::io {

namespace {

using json = nlohmann::json;

#ifndef __EMSCRIPTEN__
std::filesystem::path config_path() {
    return platform::path_from_utf8(platform::config_dir()) / "config.json";
}
#endif

json read_config() {
#ifdef __EMSCRIPTEN__
    const json parsed = json::parse(platform::browser_config(), nullptr, false);
    return parsed.is_object() ? parsed : json::object();
#else
    std::vector<std::uint8_t> bytes;
    std::string error;
    if (!read_file(platform::path_to_utf8(config_path()), bytes, error)) {
        return json::object();
    }
    const json parsed = json::parse(bytes, nullptr, false);
    return parsed.is_object() ? parsed : json::object();
#endif
}

bool write_config(const json& config, std::string& error) {
    const std::string text = config.dump(2) + "\n";
#ifdef __EMSCRIPTEN__
    return platform::save_browser_config(text, error);
#else
    const std::vector<std::uint8_t> bytes(text.begin(), text.end());
    return write_file_atomic(platform::path_to_utf8(config_path()), bytes, error);
#endif
}

// Two spellings of one file are one recent entry: `./a.noise` and its absolute path are the same
// document, and listing it twice reads as two things the person has to tell apart.
#ifndef __EMSCRIPTEN__
std::string canonical(const std::string& path) {
    std::error_code ec;
    const std::filesystem::path fixed = std::filesystem::canonical(platform::path_from_utf8(path), ec);
    return ec ? path : platform::path_to_utf8(fixed);
}
#endif

}  // namespace

std::vector<std::string> load_recent_files() {
    const json config = read_config();
    if (!config.contains("recent_files") || !config.at("recent_files").is_array()) {
        return {};
    }
    std::vector<std::string> out;
    for (const json& entry : config.at("recent_files")) {
        if (entry.is_string()) {
            out.push_back(entry.get<std::string>());
        }
    }
    return out;
}

void remember_recent_file(const std::string& path) {
#ifdef __EMSCRIPTEN__
    // Uploaded files are transient MEMFS entries; saving them as recents leaves broken links.
    static_cast<void>(path);
    return;
#else
    const std::string wanted = canonical(path);
    std::vector<std::string> recent{wanted};
    for (const std::string& seen : load_recent_files()) {
        if (recent.size() >= kRecentFileLimit) {
            break;
        }
        if (canonical(seen) != wanted) {
            recent.push_back(seen);
        }
    }
    json config = read_config();
    config["recent_files"] = recent;
    std::string error;
    // Failure here costs a shorter list on the next launch and nothing else, so it is not worth
    // interrupting a save with.
    static_cast<void>(write_config(config, error));
#endif
}

Theme load_theme() {
    const json config = read_config();
    if (config.contains("ui_theme") && config.at("ui_theme").is_string()) {
        Theme theme = Theme::dark;
        if (theme_from_name(config.at("ui_theme").get<std::string>(), theme)) {
            return theme;
        }
    }
    return Theme::dark;
}

std::string theme_name(Theme theme) {
    switch (theme) {
        case Theme::dark:
            return "dark";
        case Theme::light:
            return "light";
    }
    std::abort();
}

bool theme_from_name(const std::string& name, Theme& theme) {
    if (name == "dark") {
        theme = Theme::dark;
        return true;
    }
    if (name == "light") {
        theme = Theme::light;
        return true;
    }
    return false;
}

bool save_theme(Theme theme, std::string& error) {
    json config = read_config();
    config["ui_theme"] = theme_name(theme);
    return write_config(config, error);
}

}  // namespace noise::io
