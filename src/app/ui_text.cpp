// SPDX-License-Identifier: Apache-2.0
#include "app/ui_text.h"

#include "imgui.h"

namespace noise::app::ui_text {
namespace {

constexpr std::string_view kEllipsis = "…";

float text_width(std::string_view text) {
    return ImGui::CalcTextSize(text.data(), text.data() + text.size()).x;
}

std::size_t next_utf8_boundary(std::string_view text, std::size_t start) {
    std::size_t next = start + 1;
    while (next < text.size() &&
           (static_cast<unsigned char>(text[next]) & 0xc0U) == 0x80U) {
        ++next;
    }
    return next;
}

}

std::string truncate_text(std::string_view text, float max_width) {
    if (max_width <= 0.0f) {
        return {};
    }
    if (text_width(text) <= max_width) {
        return std::string(text);
    }
    const float ellipsis_width = text_width(kEllipsis);
    if (ellipsis_width > max_width) {
        return {};
    }
    std::size_t prefix_end = 0;
    for (std::size_t end = next_utf8_boundary(text, 0); end <= text.size();
         end = next_utf8_boundary(text, end)) {
        if (text_width(text.substr(0, end)) + ellipsis_width > max_width) {
            break;
        }
        prefix_end = end;
    }
    return std::string(text.substr(0, prefix_end)) + std::string(kEllipsis);
}

std::string truncate_filename(std::string_view filename, float max_width) {
    if (max_width <= 0.0f) {
        return {};
    }
    if (text_width(filename) <= max_width) {
        return std::string(filename);
    }
    const std::size_t dot = filename.find_last_of('.');
    const std::string_view extension = dot != std::string_view::npos && dot > 0
        ? filename.substr(dot) : std::string_view{};
    const float fixed_width = text_width(extension) + text_width(kEllipsis);
    if (fixed_width >= max_width) {
        return truncate_text(filename, max_width);
    }
    const float prefix_width = max_width - fixed_width;
    std::size_t prefix_end = 0;
    for (std::size_t end = next_utf8_boundary(filename, 0); end <= filename.size();
         end = next_utf8_boundary(filename, end)) {
        if (text_width(filename.substr(0, end)) > prefix_width) {
            break;
        }
        prefix_end = end;
    }
    return std::string(filename.substr(0, prefix_end)) + std::string(kEllipsis) +
           std::string(extension);
}

}
