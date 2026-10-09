// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace noise {

// Canonical shortcut text is the one form a shortcut exists in: it is what the command
// table stores, what `command.list` reports and what the UI prints, so a binding cannot
// be restyled per call site. Grammar is `<mods>+<key>` with the modifiers in the order
// Ctrl, Alt, Shift, separated by `+`. The key is a single printable character, upper-cased
// (`F`, `1`, `[`), or one of the named keys: Space, Enter, Tab, Escape, Backspace, Delete,
// Insert, Home, End, PageUp, PageDown, Left, Right, Up, Down, F1..F12.
//
// Returns the canonical form, or nullopt for anything malformed, out of order, or naming a
// key this vocabulary does not have.
std::optional<std::string> normalize_shortcut(std::string_view text);

// The named tokens, in order. A single printable character needs no name, so this is the
// part of the vocabulary the host's key translation has to cover explicitly.
const std::vector<std::string>& named_key_names();

}
