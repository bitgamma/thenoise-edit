// SPDX-License-Identifier: Apache-2.0
#include "io/config.h"

#include "io/file.h"
#include "platform/platform.h"

#include <nlohmann/json.hpp>

#include <filesystem>

namespace noise::io {

namespace {

std::filesystem::path config_path() {
    return std::filesystem::path(platform::config_dir()) / "config.json";
}

// Two spellings of one file are one recent entry: `./a.noise` and its absolute path are the same
// document, and listing it twice reads as two things the person has to tell apart.
std::string canonical(const std::string& path) {
    std::error_code ec;
    const std::filesystem::path fixed = std::filesystem::canonical(path, ec);
    return ec ? path : fixed.string();
}

}  // namespace

std::vector<std::string> load_recent_files() {
    std::vector<std::uint8_t> bytes;
    std::string error;
    if (!read_file(config_path().string(), bytes, error)) {
        return {};
    }
    const nlohmann::json parsed = nlohmann::json::parse(bytes, nullptr, false);
    if (parsed.is_discarded() || !parsed.is_object() || !parsed.contains("recent_files") ||
        !parsed.at("recent_files").is_array()) {
        return {};
    }
    std::vector<std::string> out;
    for (const nlohmann::json& entry : parsed.at("recent_files")) {
        if (entry.is_string()) {
            out.push_back(entry.get<std::string>());
        }
    }
    return out;
}

void remember_recent_file(const std::string& path) {
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
    const nlohmann::json out{{"recent_files", recent}};
    const std::string text = out.dump(2) + "\n";
    const std::vector<std::uint8_t> bytes(text.begin(), text.end());
    std::string error;
    // Failure here costs a shorter list on the next launch and nothing else, so it is not worth
    // interrupting a save with.
    static_cast<void>(write_file_atomic(config_path().string(), bytes, error));
}

}  // namespace noise::io
