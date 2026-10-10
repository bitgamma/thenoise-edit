// SPDX-License-Identifier: Apache-2.0
#include "core/base64.h"

#include <algorithm>

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



namespace {

// -1 outside the alphabet, -2 for the padding character.
int value_of(char c) {
    if (c == '=') {
        return -2;
    }
    const char* found = std::find(std::begin(kAlphabet), std::end(kAlphabet), c);
    if (found == std::end(kAlphabet)) {
        return -1;
    }
    return static_cast<int>(found - std::begin(kAlphabet));
}

bool is_ignored(char c) { return c == '\n' || c == '\r' || c == ' ' || c == '\t'; }

}  // namespace

std::optional<std::vector<std::uint8_t>> base64_decode(const std::string& text, std::string& error) {
    std::vector<std::uint8_t> out;
    out.reserve(text.size() / 4u * 3u);
    std::uint32_t accumulator = 0;
    int bits = 0;
    int padding = 0;
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (is_ignored(c)) {
            continue;
        }
        const int value = value_of(c);
        if (value == -1) {
            error = "base64: character '" + std::string(1, c) + "' at offset " +
                    std::to_string(i) + " is not in the alphabet";
            return std::nullopt;
        }
        if (value == -2) {
            // Padding ends the data: anything after it, however ignorable-looking, is a body that was
            // cut or joined.
            ++padding;
            if (padding > 2 || bits == 0) {
                error = "base64: padding at offset " + std::to_string(i) +
                        " does not close a quantum";
                return std::nullopt;
            }
            continue;
        }
        if (padding != 0) {
            error = "base64: data after padding at offset " + std::to_string(i);
            return std::nullopt;
        }
        accumulator = (accumulator << 6) | static_cast<std::uint32_t>(value);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<std::uint8_t>((accumulator >> bits) & 0xffu));
        }
    }
    // Leftover bits are a body whose length is not a whole quantum. A 4-bit remainder would be a byte
    // nobody encoded.
    if (bits >= 6) {
        error = "base64: length is not a whole number of quads";
        return std::nullopt;
    }
    return out;
}

}  // namespace noise
