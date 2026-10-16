// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <optional>

#include <nlohmann/json.hpp>

#include "core/command.h"
#include "core/geom.h"

namespace noise {
struct AppState;
}

namespace noise::app {

// The verbs a marked area is an instruction for, offered on the canvas beside the mark rather than
// only in the tool panel. The bar owns no verb list and no command: it draws the rows `ai.verbs`
// reports and presses the command each row names, so a bar button and the panel button it mirrors
// cannot mean different things.
void register_verb_bar_commands(AppState& state, CommandTable& table);

// The mark the bar hangs from, in document pixels, and the rows it offers: `scope == "selection"` on
// `ai.verbs`, which is the recipe's own region rather than a list kept here.
std::optional<Rect> verb_bar_mark(const AppState& state);
nlohmann::json verb_bar_rows(CommandTable& table, std::string* why);

// Called from the frame after the canvas, so the placement uses the origin and view that frame drew
// with. The rectangle it draws into is published on `AppState` for the wire to read back.
void draw_verb_bar(AppState& state, CommandTable& commands);

}  // namespace noise::app
