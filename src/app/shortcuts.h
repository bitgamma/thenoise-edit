// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <optional>
#include <string>

struct sapp_event;

namespace noise::app {

// The host's single translation from a platform key event to canonical shortcut text
// (core/shortcut.h): modifiers in table order, then the key. nullopt when the event cannot
// be a shortcut at all - a bare modifier, a key repeat, an unmapped code - so a caller can
// dispatch on it without guessing.
std::optional<std::string> shortcut_of(const sapp_event& event);

}
