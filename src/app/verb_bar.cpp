// SPDX-License-Identifier: Apache-2.0
#include "app/verb_bar.h"

#include "app/command_args.h"
#include "app/command_groups.h"

#include <nlohmann/json.hpp>

#include <optional>
#include <string>

#include "app/state.h"
#include "core/geom.h"

namespace noise::app {
namespace {

using nlohmann::json;

}  // namespace

// The rows the mark is an instruction for: `scope` is the recipe's own region, read off `ai.verbs`
// rather than decided here, so a recipe that stopped reading the marked area drops out of the bar and
// out of the panel's claim at the same moment.
json verb_bar_rows(CommandTable& table, std::string* why) {
    json rows = json::array();
    const CommandResult view = table.invoke("ai.verbs", json::object());
    if (!view.ok) {
        if (why != nullptr) {
            *why = view.error;
        }
        return rows;
    }
    for (const json& verb : view.value["verbs"]) {
        if (verb["scope"].get<std::string>() == "selection") {
            rows.push_back(json{{"verb", verb["id"]},
                                {"label", verb["label"].is_null() ? verb["id"] : verb["label"]},
                                {"key", verb["key"]},
                                {"command", verb["command"]},
                                {"recipe", verb["recipe"]},
                                {"ready", verb["ready"]},
                                {"reason", verb["reason"]}});
        }
    }
    return rows;
}

// The mark, in document pixels, when there is one. `selection_json` already answers with it, so the
// bar has no second notion of what "marked" means.
std::optional<Rect> verb_bar_mark(const AppState& state) {
    if (state.doc.empty()) {
        return std::nullopt;
    }
    const json selection = selection_json(state);
    if (!selection["active"].get<bool>()) {
        return std::nullopt;
    }
    const json& bounds = selection["bounds"];
    return Rect{bounds[0].get<int>(), bounds[1].get<int>(), bounds[2].get<int>(), bounds[3].get<int>()};
}

void register_verb_bar_commands(AppState& state, CommandTable& table) {
    table.add("verb.bar",
              "The verbs the marked area is an instruction for, and where the bar was last drawn",
              {}, {},
              [&state, &table](const json&) -> CommandResult {
        const std::optional<Rect> mark = verb_bar_mark(state);
        std::string why;
        const json rows = mark ? verb_bar_rows(table, &why) : json::array();
        if (!why.empty()) {
            return command_error("verb.bar: " + why);
        }
        // `shown` is the decision to draw; `screen` is where a frame actually did, and stays zero
        // outside one - a headless caller can read the first and must not trust the second.
        return command_ok(json{{"shown", mark.has_value() && !rows.empty()},
                               {"mark", mark == std::nullopt
                                            ? json(nullptr)
                                            : json::array({mark->x, mark->y, mark->w, mark->h})},
                               {"screen", json::array({state.verb_bar.x, state.verb_bar.y,
                                                       state.verb_bar.w, state.verb_bar.h})},
                               {"buttons", rows}});
    });
}

}  // namespace noise::app
