// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <nlohmann/json.hpp>

#include <cstdint>
#include <map>
#include <string>

#include "core/command.h"
#include "core/geom.h"

// Declared in the namespace `AppState` actually lives in (app/state.h): a forward declaration in
// `noise::app` would shadow it for every command file and leave them holding an incomplete type.
namespace noise {
struct AppState;
}

namespace noise::app {

// What the selection panel is being asked to do: one selection-scoped recipe, the variable values a
// person typed over the recipe's defaults, and the job those queued. The panel owns no behaviour:
// `ai.intent` reads and writes exactly this, so a driver and a click cannot disagree about what
// "Remove the lamp" meant.
struct AiIntent {
    std::string recipe;
    // Only what the caller supplied. The recipe's own defaults fill the rest, which is how a headless
    // `ai.run` and the panel end up compiling the same prompt.
    std::map<std::string, std::string> given;
    std::string job;
    // Set by a `connect`, reported back so the panel's automatic ask stays a single attempt: a server
    // that is down costs a connect timeout, and a widget retrying it every frame is a hang.
    bool connect_attempted = false;
    // The compiled request, as `ai.preview` reported it, beside the facts it was compiled against: a
    // panel reads this every frame and must not re-encode a PNG to do it.
    nlohmann::json preview;
    std::uint64_t compiled_revision = 0;
    bool compiled_selection = false;
    Rect compiled_bounds{};
    std::size_t compiled_covered = 0;
    std::size_t compiled_layer = 0;
    bool has_preview = false;
    // The last refusal this intent produced, so the panel can show why Run is greyed out and a wire
    // client reads the same sentence. Cleared by the next attempt.
    std::string error;
};

void register_ai_intent_commands(AppState& state, CommandTable& table);

// Everything the panel shows, from the intent and the document: the intents the selection offers, the
// values in force, whether the action could run and what it would cost. Reads the stored preview;
// `stale` says the canvas or the selection moved under it.
nlohmann::json intent_json(AppState& state);

}  // namespace noise::app
