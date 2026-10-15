// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <nlohmann/json.hpp>

#include <cstdint>
#include <map>
#include <string>

#include "ai/recipes.h"
#include "core/command.h"
#include "core/geom.h"

// Declared in the namespace `AppState` actually lives in (app/state.h): a forward declaration in
// `noise::app` would shadow it for every command file and leave them holding an incomplete type.
namespace noise {
struct AppState;
}

namespace noise::app {

// What a compiled preview is valid against. The document revision moves with every pixel write and the
// selection moves with none of them, so the two together notice a changed canvas and a changed region;
// a selection change that leaves the bounds, the count and the layer identical changes nothing a region
// compile reads.
// What the selection panel is being asked to do: one selection-scoped recipe, the variable values a
// person typed over the recipe's defaults, and the job those queued. The panel owns no behaviour:
// `ai.intent` reads and writes exactly this, so a driver and a click cannot disagree about what
// "Remove the lamp" meant.
struct AiIntent {
    std::string recipe;
    // Only what the caller supplied. The recipe's own defaults fill the rest, which is how a headless
    // `ai.run` and the panel end up compiling the same prompt.
    std::map<std::string, std::string> given;
    // The aspect chosen for a recipe that offers them; no value is the canvas. Not a `vars` entry: a
    // variable goes into the words, this into the size, and the field policy has no "aspect" field.
    std::optional<AspectRatio> aspect;
    std::string job;
    // The last refusal this intent produced, so the panel can show why Run is greyed out and a wire
    // client reads the same sentence. Cleared by the next attempt.
    std::string error;
};

void register_ai_intent_commands(AppState& state, CommandTable& table);

// Whether a recipe could run here, in the words `ai.intent` reports: the same assessment behind a
// greyed Run button, a refusal and the verb list, so those three cannot disagree about why not.
struct Readiness {
    bool ready = false;
    std::string reason;
    std::string fix;
    std::string model;
};

Readiness assess(AppState& state, const ai::Recipe* recipe);

// A recipe whose region is the selection, with nothing marked. The region door refuses it and the
// readiness forecast asks the same question, so a greyed row and a refused press cannot say different
// things - and a wordless verb over an unmarked frame cannot quietly become a whole-document
// reconstruction. Empty when nothing is missing.
std::string missing_region(const ai::Recipe& recipe, AppState& state);

// Whether running this recipe would first have to load a different model onto the box - tens of
// seconds on a shared machine, so it is priced before the press rather than discovered after it.
bool needs_swap(const AppState& state, const Readiness& readiness);

// Everything the panel shows, from the intent and the document: the intents the selection offers, the
// values in force, whether the action could run and what it would cost. Reads the stored preview;
// `stale` says the canvas or the selection moved under it.
nlohmann::json intent_json(AppState& state);

}  // namespace noise::app
