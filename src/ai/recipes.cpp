// SPDX-License-Identifier: Apache-2.0
#include "ai/recipes.h"

#include "io/file.h"

#include <algorithm>
#include <filesystem>

namespace noise::ai {

namespace {

constexpr const char* kEnhanceNever = "never";
constexpr const char* kEnhanceAlways = "always";
constexpr const char* kFeatherAuto = "auto";
constexpr const char* kFeatherNone = "none";

std::string join(const std::vector<std::string>& names, const char* separator) {
    std::string out;
    for (std::size_t i = 0; i < names.size(); ++i) {
        if (i != 0) {
            out += separator;
        }
        out += names[i];
    }
    return out;
}

bool key_known(const std::string& key) {
    static const std::vector<std::string> kKnown = {"id",        "label",     "operation", "endpoint",
                                                   "role",      "region",    "prep",      "enhance",
                                                   "prompt",    "ask",       "variables", "composite",
                                                   "context",   "aspects",   "variants",  "steps",
                                                   "cfg_scale", "mask",      "model",
                                                   "include_references_default"};
    return std::find(kKnown.begin(), kKnown.end(), key) != kKnown.end();
}

// A key that is known but belongs somewhere else is refused with the reason, because "unknown key"
// would send the next reader hunting for a typo rather than to the fact that the model owns the value.
bool key_owned_elsewhere(const std::string& key, std::string& why) {
    if (key == "steps" || key == "cfg_scale") {
        why = "'" + key + "' belongs to the model's image_defaults, which the app reads from "
                         "/v1/models; a recipe that carried it would restate a fact it could read";
        return true;
    }
    if (key == "mask") {
        why = "the engine has no mask field; region and prep are how spatial precision is expressed";
        return true;
    }
    if (key == "model") {
        why = "a recipe names a role and the registry names the model";
        return true;
    }
    return false;
}

// The keys of a nested block, named rather than inherited from the recipe's own table: a block that
// accepted an unknown key would silently ignore the policy someone meant to set.
std::string join(const std::vector<std::string>& names, const char* separator);

void reject_unknown_keys_named(const nlohmann::json& object, const std::string& origin,
                               std::string& error, bool& ok,
                               const std::vector<std::string>& names) {
    ok = true;
    for (auto it = object.begin(); it != object.end(); ++it) {
        if (std::find(names.begin(), names.end(), it.key()) == names.end()) {
            error = origin + ": unknown key '" + it.key() + "' (this block takes " +
                    join(names, ", ") + ")";
            ok = false;
            return;
        }
    }
}

void reject_unknown_keys(const nlohmann::json& object, const std::string& origin,
                         std::string& error, bool& ok) {
    ok = true;
    for (auto it = object.begin(); it != object.end(); ++it) {
        if (key_known(it.key())) {
            std::string why;
            if (key_owned_elsewhere(it.key(), why)) {
                error = origin + ": " + why;
                ok = false;
                return;
            }
            continue;
        }
        error = origin + ": unknown key '" + it.key() + "'";
        ok = false;
        return;
    }
}

std::string string_at(const nlohmann::json& object, const char* key, const std::string& fallback,
                      const std::string& origin, std::string& error, bool& ok) {
    ok = true;
    if (!object.contains(key)) {
        return fallback;
    }
    if (!object.at(key).is_string()) {
        error = origin + ": '" + std::string(key) + "' must be a string, not a " +
                std::string(object.at(key).type_name());
        ok = false;
        return fallback;
    }
    return object.at(key).get<std::string>();
}

bool bool_at(const nlohmann::json& object, const char* key, bool fallback,
             const std::string& origin, std::string& error, bool& ok) {
    ok = true;
    if (!object.contains(key)) {
        return fallback;
    }
    if (!object.at(key).is_boolean()) {
        error = origin + ": '" + std::string(key) + "' must be a boolean, not a " +
                std::string(object.at(key).type_name());
        ok = false;
        return fallback;
    }
    return object.at(key).get<bool>();
}

// `feather` is "auto", "none" or a pixel count. An integer is kept as the number the recipe insisted
// on, so a probe can tell a recipe's 3 from the compositor's own choice of 3.
std::optional<int> feather_at(const nlohmann::json& object, const std::string& origin,
                              std::string& error, bool& ok) {
    ok = true;
    if (!object.contains("feather")) {
        return std::nullopt;
    }
    const nlohmann::json& value = object.at("feather");
    if (value.is_string()) {
        const std::string name = value.get<std::string>();
        if (name == kFeatherAuto) {
            return std::nullopt;
        }
        if (name == kFeatherNone) {
            return 0;
        }
        error = origin + ": 'feather' must be 'auto', 'none' or an integer, not '" + name + "'";
        ok = false;
        return std::nullopt;
    }
    if (value.is_number_integer()) {
        const long long px = value.get<long long>();
        if (px < 0) {
            error = origin + ": 'feather' must not be negative (got " + std::to_string(px) + ")";
            ok = false;
            return std::nullopt;
        }
        return static_cast<int>(px);
    }
    error = origin + ": 'feather' must be 'auto', 'none' or an integer";
    ok = false;
    return std::nullopt;
}

}  // namespace

const char* endpoint_name(Endpoint endpoint) {
    switch (endpoint) {
        case Endpoint::generations:
            return "generations";
        case Endpoint::edits:
            return "edits";
        case Endpoint::upscale:
            return "upscale";
        case Endpoint::chat:
            return "chat";
    }
    return "edits";
}

std::optional<Endpoint> endpoint_from_name(const std::string& name) {
    if (name == "generations") {
        return Endpoint::generations;
    }
    if (name == "edits") {
        return Endpoint::edits;
    }
    if (name == "upscale") {
        return Endpoint::upscale;
    }
    if (name == "chat") {
        return Endpoint::chat;
    }
    return std::nullopt;
}

// The one place a scope becomes a word: the verb row, the region report and the panel all print this, so
// a scope cannot be called one thing in the table and another on the wire.
const char* region_scope_name(RegionScope scope) {
    switch (scope) {
        case RegionScope::document:
            return "document";
        case RegionScope::selection:
            return "selection";
        case RegionScope::layer:
            return "layer";
    }
    return "unknown";
}

const char* operation_name(Operation operation) {
    switch (operation) {
        case Operation::reconstruct:
            return "reconstruct";
        case Operation::in_place:
            return "in_place";
        case Operation::transform:
            return "transform";
        case Operation::from_words:
            return "from_words";
    }
    return "?";
}

std::optional<Operation> operation_from_name(const std::string& name) {
    for (Operation operation : {Operation::reconstruct, Operation::in_place, Operation::transform,
                                Operation::from_words}) {
        if (name == operation_name(operation)) {
            return operation;
        }
    }
    return std::nullopt;
}


std::string endpoint_names() {
    return "generations | edits | upscale | chat";
}


const Recipe* RecipeSet::find(const std::string& id) const {
    for (const Recipe& recipe : recipes) {
        if (recipe.id == id) {
            return &recipe;
        }
    }
    return nullptr;
}

std::vector<std::string> RecipeSet::ids() const {
    std::vector<std::string> out;
    out.reserve(recipes.size());
    for (const Recipe& recipe : recipes) {
        out.push_back(recipe.id);
    }
    return out;
}

std::vector<const Recipe*> RecipeSet::family(const std::string& id) const {
    std::vector<const Recipe*> out;
    const Recipe* head = find(id);
    if (head == nullptr) {
        return out;
    }
    out.push_back(head);
    for (const std::string& variant : head->variants) {
        if (const Recipe* found = find(variant); found != nullptr) {
            out.push_back(found);
        }
    }
    return out;
}

const Recipe* RecipeSet::family_head(const std::string& id) const {
    for (const Recipe& recipe : recipes) {
        if (std::find(recipe.variants.begin(), recipe.variants.end(), id) !=
            recipe.variants.end()) {
            return &recipe;
        }
    }
    return find(id);
}

std::vector<const Recipe*> RecipeSet::layer_intents() const {
    std::vector<const Recipe*> out;
    for (const Recipe& recipe : recipes) {
        if (recipe.region == RegionScope::layer) {
            out.push_back(&recipe);
        }
    }
    return out;
}

std::vector<const Recipe*> RecipeSet::selection_intents() const {
    std::vector<const Recipe*> out;
    for (const Recipe& recipe : recipes) {
        // An upscaler changes document geometry, not the marked region.
        if (recipe.region == RegionScope::selection && recipe.endpoint == Endpoint::edits) {
            out.push_back(&recipe);
        }
    }
    return out;
}

std::string recipe_id_of(const std::string& path) {
    const std::filesystem::path file{path};
    return file.stem().string();
}

std::optional<Recipe> parse_recipe(const nlohmann::json& body, const std::string& origin,
                                   std::string& error) {
    Recipe recipe;
    recipe.source = origin;
    if (!body.is_object()) {
        error = origin + ": expected an object";
        return std::nullopt;
    }
    bool ok = false;
    reject_unknown_keys(body, origin, error, ok);
    if (!ok) {
        return std::nullopt;
    }
    recipe.id = string_at(body, "id", {}, origin, error, ok);
    if (!ok) {
        return std::nullopt;
    }
    if (recipe.id.empty()) {
        error = origin + ": 'id' must not be empty";
        return std::nullopt;
    }
    // Required rather than defaulted to the id: a control that reads `inpaint-replace` is a control
    // nobody wrote, and the two-word name is the one thing a first-time user has to get right.
    recipe.label = string_at(body, "label", {}, origin, error, ok);
    if (!ok) {
        return std::nullopt;
    }
    if (recipe.label.empty()) {
        error = origin + ": 'label' is required (what the interface offers this recipe as)";
        return std::nullopt;
    }
    const std::string endpoint = string_at(body, "endpoint", "edits", origin, error, ok);
    if (!ok) {
        return std::nullopt;
    }
    const std::optional<Endpoint> parsed_endpoint = endpoint_from_name(endpoint);
    if (!parsed_endpoint.has_value()) {
        error = origin + ": 'endpoint' must be one of " + endpoint_names() + " (got '" + endpoint +
                "')";
        return std::nullopt;
    }
    recipe.endpoint = *parsed_endpoint;
    recipe.role = string_at(body, "role", {}, origin, error, ok);
    if (!ok) {
        return std::nullopt;
    }
    const std::string region = string_at(body, "region", "selection", origin, error, ok);
    if (!ok) {
        return std::nullopt;
    }
    if (region == "selection") {
        recipe.region = RegionScope::selection;
    } else if (region == "document") {
        recipe.region = RegionScope::document;
    } else if (region == "layer") {
        recipe.region = RegionScope::layer;
    } else {
        error = origin + ": 'region' must be 'selection', 'document' or 'layer' (got '" + region +
                "')";
        return std::nullopt;
    }
    if (recipe.endpoint == Endpoint::upscale && recipe.region != RegionScope::document) {
        error = origin + ": upscale changes the whole image size, so 'region' must be 'document'";
        return std::nullopt;
    }
    if (recipe.region == RegionScope::layer && recipe.operation == Operation::from_words) {
        error = origin + ": region 'layer' needs a picture to blend into the scene, so operation '" +
                operation_name(recipe.operation) + "' cannot have it";
        return std::nullopt;
    }
    const std::string prep = string_at(body, "prep", "none", origin, error, ok);
    if (!ok) {
        return std::nullopt;
    }
    const std::optional<Prep> parsed_prep = prep_from_name(prep);
    if (!parsed_prep.has_value()) {
        error = origin + ": 'prep' must be one of " + prep_names() + " (got '" + prep + "')";
        return std::nullopt;
    }
    recipe.prep = *parsed_prep;

    // What the operation is, and the pairing that keeps the two concepts from being mixed. A hole is
    // only meaningful when the area's content is unwanted; an in-place edit that asks for one is asking
    // to be shown a destroyed version of the thing it is meant to change, which is not a thing a model
    // can act on. Refused here rather than noticed by a person staring at a blurred face.
    const std::string operation = string_at(body, "operation", {}, origin, error, ok);
    if (!ok) {
        return std::nullopt;
    }
    if (operation.empty()) {
        error = origin + ": 'operation' is required (" + operation_name(Operation::reconstruct) +
                ", " + operation_name(Operation::in_place) + ", " +
                operation_name(Operation::transform) + ", " + operation_name(Operation::from_words) +
                ") - it decides whether the upload may have a hole in it";
        return std::nullopt;
    }
    const std::optional<Operation> parsed_operation = operation_from_name(operation);
    if (!parsed_operation.has_value()) {
        error = origin + ": 'operation' must be " + operation_name(Operation::reconstruct) + ", " +
                operation_name(Operation::in_place) + ", " + operation_name(Operation::transform) + " or " +
                operation_name(Operation::from_words) + " (got '" + operation + "')";
        return std::nullopt;
    }
    recipe.operation = *parsed_operation;
    if (recipe.operation != Operation::reconstruct && recipe.prep != Prep::none) {
        error = origin + ": operation '" + operation + "' must have prep 'none' - only " +
                operation_name(Operation::reconstruct) +
                " destroys an area on purpose, and a blurred subject is not an instruction";
        return std::nullopt;
    }
    if (recipe.operation == Operation::reconstruct && recipe.prep == Prep::none) {
        error = origin + ": operation '" + operation +
                "' needs a prep for the hole (blur, average, pixelate, noise or white), otherwise the "
                "upload still shows exactly what it is asking to have replaced";
        return std::nullopt;
    }
    if (recipe.operation == Operation::reconstruct && recipe.whole_document()) {
        error = origin + ": operation '" + operation +
                "' works on a selection - with no area there is nothing to reconstruct";
        return std::nullopt;
    }
    if (recipe.operation == Operation::from_words && recipe.endpoint != Endpoint::generations) {
        error = origin + ": operation '" + operation + "' must use endpoint 'generations'";
        return std::nullopt;
    }
    recipe.enhance = string_at(body, "enhance", kEnhanceNever, origin, error, ok);
    if (!ok) {
        return std::nullopt;
    }
    if (recipe.enhance != kEnhanceNever && recipe.enhance != kEnhanceAlways) {
        error = origin + ": 'enhance' must be 'never' or 'always' (got '" + recipe.enhance + "')";
        return std::nullopt;
    }
    if (!body.contains("include_references_default")) {
        error = origin + ": 'include_references_default' is required";
        return std::nullopt;
    }
    if (!body.at("include_references_default").is_boolean()) {
        error = origin + ": 'include_references_default' must be a boolean, not " +
                std::string(body.at("include_references_default").type_name());
        return std::nullopt;
    }
    recipe.include_references_default = body.at("include_references_default").get<bool>();

    if (body.contains("prompt")) {
        if (!body.at("prompt").is_array()) {
            error = origin + ": 'prompt' must be an array of lines";
            return std::nullopt;
        }
        for (std::size_t i = 0; i < body.at("prompt").size(); ++i) {
            const nlohmann::json& line = body.at("prompt")[i];
            if (!line.is_string()) {
                error = origin + ": 'prompt[" + std::to_string(i) +
                        "]' must be a string; a prompt is an array of lines, not one long one";
                return std::nullopt;
            }
            recipe.prompt_lines.push_back(line.get<std::string>());
        }
    }
    if (body.contains("variables")) {
        if (!body.at("variables").is_object()) {
            error = origin + ": 'variables' must be an object of string defaults";
            return std::nullopt;
        }
        for (auto it = body.at("variables").begin(); it != body.at("variables").end(); ++it) {
            if (!it.value().is_string()) {
                error = origin + ": 'variables." + it.key() + "' must be a string";
                return std::nullopt;
            }
            recipe.variables[it.key()] = it.value().get<std::string>();
        }
    }
    // What a person is asked to type. An asked-for name has **no default anywhere**: text sitting in the
    // box meant for the user is text nobody chose, and it spends a model's time on a sentence the
    // recipe wrote. Names not asked for run on their default, which is how a headless `ai.run` and the
    // window agree on one set of values - and how `context_desc` stays unwritten until `enhance` fills it.
    if (body.contains("ask")) {
        if (!body.at("ask").is_array()) {
            error = origin + ": 'ask' must be an array of names the prompt asks for";
            return std::nullopt;
        }
        for (std::size_t i = 0; i < body.at("ask").size(); ++i) {
            const nlohmann::json& name = body.at("ask")[i];
            if (!name.is_string()) {
                error = origin + ": 'ask[" + std::to_string(i) + "]' must be a string";
                return std::nullopt;
            }
            const std::string variable = name.get<std::string>();
            const auto prefilled = recipe.variables.find(variable);
            if (prefilled != recipe.variables.end()) {
                error = origin + ": 'ask' names '" + variable + "' while 'variables' prefills it with '" +
                        prefilled->second + "'; an asked-for field starts empty, so it belongs in 'ask' "
                        "and not in 'variables'";
                return std::nullopt;
            }
            const std::string placeholder = "{{" + variable + "}}";
            const bool referenced = std::any_of(recipe.prompt_lines.begin(), recipe.prompt_lines.end(),
                                                [&placeholder](const std::string& line) {
                                                    return line.find(placeholder) != std::string::npos;
                                                });
            if (!referenced) {
                error = origin + ": 'ask' collects '" + variable +
                        "' and no 'prompt' line mentions '" + placeholder +
                        "', so the answer to the question would be thrown away";
                return std::nullopt;
            }
            if (std::find(recipe.ask.begin(), recipe.ask.end(), variable) != recipe.ask.end()) {
                error = origin + ": 'ask' names '" + variable + "' twice";
                return std::nullopt;
            }
            recipe.ask.push_back(variable);
        }
    }
    // How many words an operation may need is a property of the operation, not of the prompt.
    // `from_words` has no source image at all, so its one field is the content; `transform` is a
    // resample and wants no prose; a reconstruction can be wordless (the mask says what to remove) and
    // an in-place edit can be wordless (re-render this frame), but neither takes a second question.
    switch (recipe.operation) {
        case Operation::reconstruct:
        case Operation::in_place:
            if (recipe.ask.size() > 1) {
                error = origin + ": operation '" + operation + "' asks for " +
                        std::to_string(recipe.ask.size()) +
                        " fields; one box at most, because two questions in one box is the "
                        "'replace the lamp with a boat' ambiguity this table exists to remove";
                return std::nullopt;
            }
            break;
        case Operation::from_words:
            if (recipe.ask.size() != 1) {
                error = origin + ": operation '" + operation + "' needs exactly one 'ask' field - it has "
                                "no source image, so without a description there is nothing to generate";
                return std::nullopt;
            }
            break;
        case Operation::transform:
            if (!recipe.ask.empty()) {
                error = origin + ": operation '" + operation + "' asks for '" + recipe.ask.front() +
                        "'; a transform is given pixels and a size, not a sentence";
                return std::nullopt;
            }
            break;
    }

    if (!body.contains("composite")) {
        error = origin + ": 'composite' is required - every answer is composited, so a recipe that "
                        "names no policy is a recipe nobody reviewed";
        return std::nullopt;
    }
    if (!body.at("composite").is_object()) {
        error = origin + ": 'composite' must be an object";
        return std::nullopt;
    }
    const nlohmann::json& composite = body.at("composite");
    const std::string composite_origin = origin + ".composite";
    reject_unknown_keys_named(composite, composite_origin, error, ok,
                              {"feather", "colour_match", "lock_band_px"});
    if (!ok) {
        return std::nullopt;
    }
    recipe.composite.feather_px = feather_at(composite, composite_origin, error, ok);
    if (!ok) {
        return std::nullopt;
    }
    recipe.composite.colour_match =
        bool_at(composite, "colour_match", true, composite_origin, error, ok);
    if (!ok) {
        return std::nullopt;
    }
    if (composite.contains("lock_band_px")) {
        if (!composite.at("lock_band_px").is_number_integer() ||
            composite.at("lock_band_px").get<long long>() < 0) {
            error = composite_origin + ": 'lock_band_px' must be a non-negative integer";
            return std::nullopt;
        }
        recipe.composite.lock_band_px =
            static_cast<int>(composite.at("lock_band_px").get<long long>());
    }

    if (body.contains("aspects")) {
        if (!body.at("aspects").is_array()) {
            error = origin + ": 'aspects' must be an array of \"W:H\" frames";
            return std::nullopt;
        }
        for (std::size_t i = 0; i < body.at("aspects").size(); ++i) {
            const nlohmann::json& entry = body.at("aspects")[i];
            if (!entry.is_string()) {
                error = origin + ": 'aspects[" + std::to_string(i) +
                        "]' must be an \"W:H\" string, not a " + std::string(entry.type_name());
                return std::nullopt;
            }
            const std::string name = entry.get<std::string>();
            const std::optional<AspectRatio> ratio = aspect_from_name(name);
            if (!ratio.has_value()) {
                error = origin + ": 'aspects[" + std::to_string(i) + "]' is '" + name +
                        "', which is not an \"W:H\" frame";
                return std::nullopt;
            }
            if (std::find(recipe.aspects.begin(), recipe.aspects.end(), *ratio) !=
                recipe.aspects.end()) {
                error = origin + ": 'aspects' offers '" + name + "' twice";
                return std::nullopt;
            }
            recipe.aspects.push_back(*ratio);
        }
        if (!recipe.aspects.empty() && (recipe.operation != Operation::from_words ||
                                       recipe.region != RegionScope::document)) {
            error = origin + ": 'aspects' offers " + aspect_names(recipe.aspects) +
                    ", and a frame is a choice only for a 'from_words' recipe whose region is the "
                    "document - here the region is pixels that already exist";
            return std::nullopt;
        }
    }

    // Which of them really stand in is `variant_errors`' question; one file can only say they are ids.
    if (body.contains("variants")) {
        if (!body.at("variants").is_array()) {
            error = origin + ": 'variants' must be an array of recipe ids";
            return std::nullopt;
        }
        for (std::size_t i = 0; i < body.at("variants").size(); ++i) {
            const nlohmann::json& entry = body.at("variants")[i];
            if (!entry.is_string()) {
                error = origin + ": 'variants[" + std::to_string(i) +
                        "]' must be a recipe id, not a " + std::string(entry.type_name());
                return std::nullopt;
            }
            const std::string variant = entry.get<std::string>();
            if (variant == recipe.id) {
                error = origin + ": 'variants' names '" + variant +
                        "', which is this recipe: a family is a head and the others";
                return std::nullopt;
            }
            if (std::find(recipe.variants.begin(), recipe.variants.end(), variant) !=
                recipe.variants.end()) {
                error = origin + ": 'variants' names '" + variant + "' twice";
                return std::nullopt;
            }
            recipe.variants.push_back(variant);
        }
    }

    if (body.contains("context")) {
        if (!body.at("context").is_object()) {
            error = origin + ": 'context' must be an object ('ratio', 'min_px')";
            return std::nullopt;
        }
        const nlohmann::json& context = body.at("context");
        const std::string context_origin = origin + ".context";
        reject_unknown_keys_named(context, context_origin, error, ok,
                                  {"ratio", "min_px"});
        if (!ok) {
            return std::nullopt;
        }
        if (context.contains("ratio")) {
            if (!context.at("ratio").is_number() || context.at("ratio").get<double>() < 0.0) {
                error = context_origin + ": 'ratio' must be a non-negative number";
                return std::nullopt;
            }
            recipe.context_ratio = context.at("ratio").get<double>();
        }
        if (context.contains("min_px")) {
            if (!context.at("min_px").is_number_integer() ||
                context.at("min_px").get<long long>() < 0) {
                error = context_origin + ": 'min_px' must be a non-negative integer";
                return std::nullopt;
            }
            recipe.context_min_px = static_cast<int>(context.at("min_px").get<long long>());
        }
    }
    if (recipe.role.empty() && recipe.endpoint != Endpoint::upscale) {
        error = origin + ": 'role' is required (the recipe names a role, never a model)";
        return std::nullopt;
    }
    if (recipe.prompt_lines.empty() &&
        (recipe.endpoint == Endpoint::generations || recipe.endpoint == Endpoint::edits)) {
        error = origin + ": 'prompt' must not be empty for " + endpoint_name(recipe.endpoint);
        return std::nullopt;
    }
    return recipe;
}

std::optional<RecipeSet> parse_recipes(const nlohmann::json& body, const std::string& origin,
                                       std::string& error) {
    RecipeSet set;
    const std::optional<Recipe> recipe = parse_recipe(body, origin, error);
    if (!recipe.has_value()) {
        return std::nullopt;
    }
    set.recipes.push_back(*recipe);
    return set;
}

// A variant answers to the head's key, so it has to be interchangeable there: same operation, endpoint,
// words asked and frames offered. Anything else needs a key of its own, and a table that let it in would
// run a recipe whose questions the row never asked. Checked here because a reference across files can
// only be resolved once the whole table exists.
std::string variant_errors(const RecipeSet& set) {
    for (const Recipe& recipe : set.recipes) {
        for (const std::string& variant : recipe.variants) {
            const Recipe* found = set.find(variant);
            if (found == nullptr) {
                return recipe.source + ": 'variants' names '" + variant +
                       "', which the table does not define (it has " + join(set.ids(), ", ") + ")";
            }
            if (!found->variants.empty()) {
                return recipe.source + ": 'variants' names '" + variant +
                       "', which declares variants of its own; a family has one head, or the panel is "
                       "offering a choice of choices";
            }
            if (found->operation != recipe.operation || found->endpoint != recipe.endpoint ||
                found->ask != recipe.ask || found->aspects != recipe.aspects) {
                return recipe.source + ": variant '" + variant + "' is not the same door as '" +
                       recipe.id + "' (operations " + operation_name(found->operation) + " vs " +
                       operation_name(recipe.operation) + ", endpoints " +
                       endpoint_name(found->endpoint) + " vs " + endpoint_name(recipe.endpoint) +
                       ", " + std::to_string(found->ask.size()) + " field(s) asked vs " +
                       std::to_string(recipe.ask.size()) + ", frames " +
                       (found->aspects == recipe.aspects
                            ? aspect_names(recipe.aspects)
                            : aspect_names(found->aspects) + " vs " + aspect_names(recipe.aspects)) +
                       ")";
            }
            for (const Recipe& other : set.recipes) {
                if (&other != &recipe && std::find(other.variants.begin(), other.variants.end(),
                                                   variant) != other.variants.end()) {
                    return "'" + variant + "' is a variant of both '" + recipe.id + "' and '" +
                           other.id + "'; one recipe answers at one door";
                }
            }
        }
    }
    return {};
}

std::optional<RecipeSet> load_recipes(const std::string& directory, std::string& error) {
    std::error_code ec;
    if (!std::filesystem::is_directory(directory, ec)) {
        error = "no recipe directory at '" + directory + "'";
        return std::nullopt;
    }
    std::vector<std::string> paths;
    for (const std::filesystem::directory_entry& entry :
         std::filesystem::directory_iterator(directory, ec)) {
        if (ec) {
            error = "reading '" + directory + "': " + ec.message();
            return std::nullopt;
        }
        if (entry.is_regular_file() && entry.path().extension() == ".json") {
            paths.push_back(entry.path().string());
        }
    }
    std::sort(paths.begin(), paths.end());

    RecipeSet set;
    for (const std::string& path : paths) {
        std::vector<std::uint8_t> bytes;
        if (!io::read_file(path, bytes, error)) {
            return std::nullopt;
        }
        nlohmann::json body;
        try {
            body = nlohmann::json::parse(bytes);
        } catch (const nlohmann::json::parse_error& exception) {
            error = path + ": " + exception.what();
            return std::nullopt;
        }
        const std::optional<Recipe> recipe = parse_recipe(body, path, error);
        if (!recipe.has_value()) {
            return std::nullopt;
        }
        const std::string stem = std::filesystem::path(path).stem().string();
        if (stem != recipe->id) {
            error = path + ": file name says '" + stem + "' but 'id' says '" + recipe->id +
                    "' - a recipe is found by its id, so the two may not disagree";
            return std::nullopt;
        }
        if (set.find(recipe->id) != nullptr) {
            error = path + ": duplicate recipe id '" + recipe->id + "'";
            return std::nullopt;
        }
        set.recipes.push_back(*recipe);
    }
    if (set.recipes.empty()) {
        error = "'" + directory + "' holds no *.json recipes";
        return std::nullopt;
    }
    const std::string family_error = variant_errors(set);
    if (!family_error.empty()) {
        error = family_error;
        return std::nullopt;
    }
    return set;
}

std::optional<std::string> build_prompt(const Recipe& recipe,
                                        const std::map<std::string, std::string>& given,
                                        std::string& error) {
    // An asked-for field *is* what the operation was told. Blank is not a weak instruction but no
    // instruction, and the model would invent one at the user's expense, so this refuses here - the one
    // place a prompt is rendered - rather than letting a door forget and send a sentence with a hole in it.
    for (const std::string& name : recipe.ask) {
        const auto supplied = given.find(name);
        if (supplied == given.end()) {
            error = "recipe '" + recipe.id + "' asks for '" + name + "' and was given nothing; pass "
                    "vars {\"" + name + "\": \"...\"} - an operation with no words is a coin toss";
            return std::nullopt;
        }
        if (supplied->second.find_first_not_of(" \t\r\n") == std::string::npos) {
            error = "recipe '" + recipe.id + "' asks for '" + name + "' and was given only whitespace";
            return std::nullopt;
        }
    }
    std::string out;
    for (std::size_t line = 0; line < recipe.prompt_lines.size(); ++line) {
        const std::string& source = recipe.prompt_lines[line];
        for (std::size_t at = 0; at < source.size();) {
            const std::size_t open = source.find("{{", at);
            if (open == std::string::npos) {
                out.append(source, at, std::string::npos);
                break;
            }
            out.append(source, at, open - at);
            const std::size_t close = source.find("}}", open + 2);
            if (close == std::string::npos) {
                error = "recipe '" + recipe.id + "': line " + std::to_string(line + 1) +
                        " opens '{{' without closing it";
                return std::nullopt;
            }
            const std::string name = source.substr(open + 2, close - open - 2);
            const auto supplied = given.find(name);
            if (supplied != given.end()) {
                out.append(supplied->second);
            } else {
                const auto fallback = recipe.variables.find(name);
                if (fallback == recipe.variables.end()) {
                    error = "recipe '" + recipe.id + "': placeholder '{{" + name +
                            "}}' has no value and no default in 'variables'";
                    return std::nullopt;
                }
                out.append(fallback->second);
            }
            at = close + 2;
        }
        if (line + 1 < recipe.prompt_lines.size()) {
            out.push_back('\n');
        }
    }
    return out;
}

}  // namespace noise::ai
