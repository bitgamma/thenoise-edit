// SPDX-License-Identifier: Apache-2.0
#include "core/shortcut.h"

#include <cctype>

namespace noise {

namespace {

constexpr std::string_view kModifiers[] = {"Ctrl", "Alt", "Shift"};

constexpr std::string_view kNamedKeys[] = {
    "Space",
    "Enter",
    "Tab",
    "Escape",
    "Backspace",
    "Delete",
    "Insert",
    "Home",
    "End",
    "PageUp",
    "PageDown",
    "Left",
    "Right",
    "Up",
    "Down",
    "F1",
    "F2",
    "F3",
    "F4",
    "F5",
    "F6",
    "F7",
    "F8",
    "F9",
    "F10",
    "F11",
    "F12",
};

// Case-insensitive membership, so "ctrl+shift+f" and "CTRL+Shift+F" both land on the
// canonical form rather than on two bindings that look different.
int matched_index(std::string_view token, const std::string_view* table, std::size_t count) {
    for (std::size_t i = 0; i < count; ++i) {
        const std::string_view entry = table[i];
        if (entry.size() != token.size()) {
            continue;
        }
        bool same = true;
        for (std::size_t c = 0; c < entry.size(); ++c) {
            if (std::tolower(static_cast<unsigned char>(entry[c])) !=
                std::tolower(static_cast<unsigned char>(token[c]))) {
                same = false;
                break;
            }
        }
        if (same) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

bool is_key_token(std::string_view token) {
    if (token.size() == 1u) {
        const unsigned char c = static_cast<unsigned char>(token[0]);
        return c >= '!' && c <= '~';
    }
    return matched_index(token, kNamedKeys, std::size(kNamedKeys)) >= 0;
}

std::string canonical_key(std::string_view token) {
    if (token.size() == 1u) {
        return std::string(1, static_cast<char>(
                                std::toupper(static_cast<unsigned char>(token[0]))));
    }
    return std::string(kNamedKeys[static_cast<std::size_t>(
        matched_index(token, kNamedKeys, std::size(kNamedKeys)))]);
}

}

std::optional<std::string> normalize_shortcut(std::string_view text) {
    std::string prefix;
    bool seen[std::size(kModifiers)] = {false, false, false};
    std::string_view rest = text;

    while (true) {
        const std::size_t plus = rest.find('+');
        if (plus == std::string_view::npos) {
            break;
        }
        const int modifier = matched_index(rest.substr(0, plus), kModifiers,
                                           std::size(kModifiers));
        if (modifier < 0) {
            break;  // not a modifier: the remainder is the key, and may itself be `+`
        }
        if (seen[modifier]) {
            return std::nullopt;
        }
        for (std::size_t later = static_cast<std::size_t>(modifier) + 1; later < std::size(kModifiers);
             ++later) {
            if (seen[later]) {
                return std::nullopt;  // modifiers must appear Ctrl, Alt, Shift
            }
        }
        seen[modifier] = true;
        prefix += std::string(kModifiers[static_cast<std::size_t>(modifier)]) + "+";
        rest = rest.substr(plus + 1);
    }

    if (!is_key_token(rest)) {
        return std::nullopt;
    }
    return prefix + canonical_key(rest);
}

const std::vector<std::string>& named_key_names() {
    static const std::vector<std::string> names([] {
        std::vector<std::string> out;
        for (const std::string_view name : kNamedKeys) {
            out.emplace_back(name);
        }
        return out;
    }());
    return names;
}

}
