// SPDX-License-Identifier: Apache-2.0
#include "core/base64.h"

namespace noise {

namespace {

constexpr char kAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

}

std::string base64_encode(std::span<const std::uint8_t> bytes) {
    std::string out;
    out.reserve(((bytes.size() + 2u) / 3u) * 4u);
    std::size_t i = 0;
    while (i + 3u <= bytes.size()) {
        const std::uint32_t v = (static_cast<std::uint32_t>(bytes[i]) << 16) |
                                (static_cast<std::uint32_t>(bytes[i + 1]) << 8) |
                                static_cast<std::uint32_t>(bytes[i + 2]);
        out.push_back(kAlphabet[(v >> 18) & 0x3fu]);
        out.push_back(kAlphabet[(v >> 12) & 0x3fu]);
        out.push_back(kAlphabet[(v >> 6) & 0x3fu]);
        out.push_back(kAlphabet[v & 0x3fu]);
        i += 3u;
    }
    const std::size_t rest = bytes.size() - i;
    if (rest == 1) {
        const std::uint32_t v = static_cast<std::uint32_t>(bytes[i]) << 16;
        out.push_back(kAlphabet[(v >> 18) & 0x3fu]);
        out.push_back(kAlphabet[(v >> 12) & 0x3fu]);
        out.append("==");
    } else if (rest == 2) {
        const std::uint32_t v = (static_cast<std::uint32_t>(bytes[i]) << 16) |
                                (static_cast<std::uint32_t>(bytes[i + 1]) << 8);
        out.push_back(kAlphabet[(v >> 18) & 0x3fu]);
        out.push_back(kAlphabet[(v >> 12) & 0x3fu]);
        out.push_back(kAlphabet[(v >> 6) & 0x3fu]);
        out.push_back('=');
    }
    return out;
}

}
