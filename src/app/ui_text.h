// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <string>
#include <string_view>

namespace noise::app::ui_text {

std::string truncate_text(std::string_view text, float max_width);
std::string truncate_filename(std::string_view filename, float max_width);

}
