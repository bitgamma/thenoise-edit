// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "ai/recipes.h"
#include "core/command.h"

namespace noise {
struct AppState;
}

namespace noise::app {

// What the user does, as opposed to what a recipe is: `Recipe::operation` is the policy the AI path
// compiles under, this is the thing a key or a button means. Each verb drives exactly one recipe, and
// the mapping is checked against that recipe at load (`verb_table_error`) rather than at the press.
//
// `extend` and `upscale` are deliberately absent: the canvas does not grow and the document does not
// resize yet, so a row here would be a key that promises a mechanism which does not exist.
enum class VerbId {
    heal,      // the marked area is repaired into its surroundings, nothing is asked
    remove,    // the marked content goes, nothing is asked
    fill,      // the marked area is destroyed and filled with what one field describes
    change,    // the marked content is edited in place, the pixels being the reference
    generate,  // pixels from words, with no source image
    blend,     // the active layer's own coverage is repainted into the scene around it
};

struct VerbInfo {
    VerbId id;
    // The policy this verb drives. Its `label`, its `ask` and its region are what the interface shows -
    // restating them here would be a second copy of a fact the recipe owns.
    const char* recipe;
    // Canonical shortcut text (core/shortcut.h) carried into the command row, so the key a person
    // presses and the key `command.list` reports are the same one fact.
    const char* key;
};

extern const std::vector<VerbInfo> kVerbs;

const char* verb_name(VerbId id);
std::optional<VerbId> verb_from_name(std::string_view name);
// "remove, fill, change, generate" - what a refusal offers instead of the name it was given.
std::string verb_names();

// The verb's own contract, stated over the recipe's facts. A row that points at a policy which stopped
// being what the key says is the bug this catches: the button would still read "Remove" while sending
// something else.
bool verb_matches(const VerbInfo& verb, const ai::Recipe& recipe);

// Empty when every row has its recipe and means what it says. Non-empty is a failure of the whole
// table, the same way one unparseable recipe file is: a verb surface that half works is a set of
// buttons whose labels cannot be trusted. `verbs` is a parameter rather than `kVerbs` so a test can
// hand it a table that contradicts itself; the app always passes `kVerbs`.
std::string verb_table_error(const ai::RecipeSet& recipes, const std::vector<VerbInfo>& verbs);

// The `VerbInfo` for an id, and the recipe it drives - nullopt when the table or its recipe is gone.
const VerbInfo* find_verb(VerbId id);
const ai::Recipe* verb_recipe(noise::AppState& state, const VerbInfo& verb);

// The recipe a press of this verb would run: the row's own, unless the intent holds another member of the
// row recipe's family (the style the panel chose). The row reports it as `active` and the command runs it,
// so a screenshot, `ai.verbs` and the key cannot disagree about what is selected.
const ai::Recipe* pressed_recipe(noise::AppState& state, const VerbInfo& verb);

}  // namespace noise::app
