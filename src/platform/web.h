// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <string>

namespace noise::platform {

std::string browser_config();
bool save_browser_config(const std::string& text, std::string& error);
bool download_file(const std::string& path, std::string& error);
void enable_browser_drop();
void set_browser_theme(bool light);

}
