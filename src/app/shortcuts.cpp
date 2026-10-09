// SPDX-License-Identifier: Apache-2.0
#include "app/shortcuts.h"

#include "core/shortcut.h"

#include "sokol_app.h"

#include <cctype>

namespace noise::app {

namespace {

// Every printable key arrives with its own ASCII value; SAPP_KEYCODE_* only names the ones
// a US layout needs, so the range is bounded by ASCII rather than by those names.
constexpr int kFirstPrintable = '!';
constexpr int kLastPrintable = '~';

constexpr const char* kFunctionKeys[] = {"F1",  "F2",  "F3",  "F4",  "F5",  "F6",
                                         "F7",  "F8",  "F9",  "F10", "F11", "F12"};

// nullptr for anything with no name in core/shortcut.h's vocabulary.
const char* named_key(sapp_keycode code) {
    if (code >= SAPP_KEYCODE_F1 && code <= SAPP_KEYCODE_F12) {
        return kFunctionKeys[static_cast<std::size_t>(code - SAPP_KEYCODE_F1)];
    }
    switch (code) {
        case SAPP_KEYCODE_SPACE: return "Space";
        case SAPP_KEYCODE_ESCAPE: return "Escape";
        case SAPP_KEYCODE_ENTER: return "Enter";
        case SAPP_KEYCODE_TAB: return "Tab";
        case SAPP_KEYCODE_BACKSPACE: return "Backspace";
        case SAPP_KEYCODE_INSERT: return "Insert";
        case SAPP_KEYCODE_DELETE: return "Delete";
        case SAPP_KEYCODE_HOME: return "Home";
        case SAPP_KEYCODE_END: return "End";
        case SAPP_KEYCODE_PAGE_UP: return "PageUp";
        case SAPP_KEYCODE_PAGE_DOWN: return "PageDown";
        case SAPP_KEYCODE_LEFT: return "Left";
        case SAPP_KEYCODE_RIGHT: return "Right";
        case SAPP_KEYCODE_UP: return "Up";
        case SAPP_KEYCODE_DOWN: return "Down";
        default: return nullptr;
    }
}

}

std::optional<std::string> shortcut_of(const sapp_event& event) {
    if (event.type != SAPP_EVENTTYPE_KEY_DOWN || event.key_repeat) {
        return std::nullopt;
    }

    std::string key;
    if (event.key_code >= kFirstPrintable && event.key_code <= kLastPrintable) {
        key = std::string(1, static_cast<char>(
                                 std::toupper(static_cast<unsigned char>(event.key_code))));
    } else if (const char* named = named_key(event.key_code)) {
        key = named;
    } else {
        return std::nullopt;
    }

    std::string text;
    if (event.modifiers & SAPP_MODIFIER_CTRL) {
        text += "Ctrl+";
    }
    if (event.modifiers & SAPP_MODIFIER_ALT) {
        text += "Alt+";
    }
    if (event.modifiers & SAPP_MODIFIER_SHIFT) {
        text += "Shift+";
    }
    text += key;
    return normalize_shortcut(text);
}

}
