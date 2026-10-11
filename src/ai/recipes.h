// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <nlohmann/json.hpp>

#include <map>
#include <optional>
#include <string>
#include <vector>

#include "ai/compose.h"
#include "ai/region.h"

namespace noise::ai {

// Where a recipe's pixels come from. `chat` is the `enhance` role's endpoint: designed in, switched
// off in every MVP recipe, and parsed so a recipe that declares it is not silently ignored.
enum class Endpoint {
    generations,
    edits,
    upscale,
    chat,
};

const char* endpoint_name(Endpoint endpoint);
std::optional<Endpoint> endpoint_from_name(const std::string& name);
std::string endpoint_names();

// Whether the region is the whole document or the selection's compiled box.
enum class RegionScope {
    document,
    selection,
};

// Which mask the compositor is handed as the extent. A recipe states this because it is a claim about
// the operation: a generation may fill pixels the layer never had, an edit cannot extend its
// silhouette.
enum class ExtentSource {
    layer_alpha,
    canvas,
};

const char* extent_source_name(ExtentSource source);
std::optional<ExtentSource> extent_source_from_name(const std::string& name);

// What the operation *does*, which decides what the request may contain. These are not flavours of one
// thing, and conflating them is what made a Replace upload a blurred face:
//
//  - `reconstruct` fills an area whose content is unwanted. The hole is destroyed in the upload (`prep`)
//    and the surroundings go up sharp, because they are the only instruction it has. Zero words needed.
//  - `in_place` changes a named thing. Nothing may be destroyed - destroying the subject destroys what
//    it is to be changed from - so `prep` must be `none`, and the words are the whole operation.
//  - `transform` changes the picture rather than its content (upscale). No hole, no words.
//  - `from_words` has no source image at all, so there is nothing to prep or protect.
//
// `role` is a different axis and shares no value with this one: the role says which model answers, the
// operation says what happens to the pixels. One role (`edit`) serves both `reconstruct` and `in_place`.
enum class Operation {
    reconstruct,
    in_place,
    transform,
    from_words,
};
const char* operation_name(Operation operation);
std::optional<Operation> operation_from_name(const std::string& name);

// One `recipes/*.json`. A recipe is an operation + prompt template + region policy + hole policy +
// composite policy and nothing else: no model id (a role), no step count (the model's `image_defaults`),
// no mask (the engine has none).
struct Recipe {
    std::string id;
    // What the interface offers this recipe as. The id is a key and greppable; a button is not.
    std::string label;
    // Which file this came from. `ai.recipes` shows it and every parse error names it, so a recipe that
    // behaves oddly can be traced to the bytes that say so.
    std::string source;
    Endpoint endpoint = Endpoint::edits;
    std::string role;
    Operation operation = Operation::in_place;
    RegionScope region = RegionScope::selection;
    // The hole's treatment, and only for `reconstruct`: the other operations are refused at parse if
    // they ask for one.
    Prep prep = Prep::none;
    // "never" | "always". A recipe that says "always" must still build a valid request when the
    // `enhance` role is unresolved, which is why the MVP ships "never" everywhere.
    std::string enhance = "never";
    std::vector<std::string> prompt_lines;
    // A recipe's own context growth, when it differs from `roles.json`. Null means "what the table
    // says", which is the common case and the reason the table exists.
    std::optional<double> context_ratio;
    std::optional<int> context_min_px;
    // Values `{{name}}` placeholders may take. A placeholder with neither a given value nor a default
    // is a refusal naming it.
    std::map<std::string, std::string> variables;
    // The subset of `variables` a person is asked to fill, in the order the interface asks. A name
    // outside `variables` is refused at parse, so the panel never collects a value the prompt
    // builder will reject.
    std::vector<std::string> ask;
    CompositePolicy composite{CompositePolicy{}};
    ExtentSource extent = ExtentSource::layer_alpha;
    // True when the region ignores the selection, which is what makes `new-image` work with nothing
    // selected and `reimagine` cover a whole frame.
    bool whole_document() const { return region == RegionScope::document; }
    // True when the upload is a picture of something that exists, so a hole in it is a claim about the
    // world rather than about a blank canvas.
    bool makes_a_hole() const { return operation == Operation::reconstruct; }
};

struct RecipeSet {
    const Recipe* find(const std::string& id) const;
    std::vector<std::string> ids() const;
    // Everything that works on the current selection, in table order - the intents a selection
    // offers. The scope and the endpoint are what decide, not a list of ids: adding a recipe adds a
    // row here without anything else being told.
    std::vector<const Recipe*> selection_intents() const;
    std::vector<Recipe> recipes;
};

// The id a file at `path` is required to declare: `heal.json` is `heal`. Enforced at parse, because a
// recipe whose listed id and file name differ is two ids for one policy.
std::string recipe_id_of(const std::string& path);

std::optional<Recipe> parse_recipe(const nlohmann::json& body, const std::string& origin,
                                   std::string& error);
// Every `*.json` in `dir`, sorted by path so the set is the same on every run. A file that fails to
// parse is a failure of the whole set: a half-loaded recipe table would let a command run with a
// recipe nobody reviewed.
std::optional<RecipeSet> load_recipes(const std::string& directory, std::string& error);
std::optional<RecipeSet> parse_recipes(const nlohmann::json& body, const std::string& origin,
                                       std::string& error);

// `{{name}}` substitution then the lines joined with '\n' - JSON has no multi-line strings that read
// well in review, which is why `prompt` is an array.
std::optional<std::string> build_prompt(const Recipe& recipe,
                                        const std::map<std::string, std::string>& given,
                                        std::string& error);

}  // namespace noise::ai
