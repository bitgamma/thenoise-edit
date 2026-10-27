// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <filesystem>
#include <string>

namespace noise::platform {

// NFD returns UTF-8 paths; on Windows std::filesystem::path(std::string) would
// instead interpret those bytes in the process ANSI code page.
inline std::filesystem::path path_from_utf8(const std::string& text) {
#ifdef _WIN32
    std::u8string encoded;
    encoded.reserve(text.size());
    for (unsigned char character : text) {
        encoded.push_back(static_cast<char8_t>(character));
    }
    return std::filesystem::path(encoded);
#else
    return std::filesystem::path(text);
#endif
}

inline std::string path_to_utf8(const std::filesystem::path& path) {
#ifdef _WIN32
    const std::u8string encoded = path.u8string();
    return {reinterpret_cast<const char*>(encoded.data()), encoded.size()};
#else
    return path.string();
#endif
}

}  // namespace noise::platform
