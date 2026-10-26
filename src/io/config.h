// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace noise::io {

enum class Theme { dark, light };

// The app's own settings: `<config_dir>/config.json`, which `$NOISE_CONFIG_DIR` relocates. Preferences
// and session facts only — anything that describes a picture belongs in the document, and anything the
// machine can work out belongs nowhere.
inline constexpr std::size_t kRecentFileLimit = 8;

// Documents this app opened or saved, newest first. A missing or unreadable file is "no recents"
// rather than an error: nothing a person does next may depend on the list.
std::vector<std::string> load_recent_files();

// Puts `path` first, deduplicated by canonical path, capped at `kRecentFileLimit` — and written now,
// because a recent list that survives only until the next crash is not one.
void remember_recent_file(const std::string& path);

Theme load_theme();
std::string theme_name(Theme theme);
bool theme_from_name(const std::string& name, Theme& theme);
bool save_theme(Theme theme, std::string& error);

}  // namespace noise::io
