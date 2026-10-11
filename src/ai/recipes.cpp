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
    static const std::vector<std::string> kKnown = {
        "id",    "label",    "endpoint", "role",      "region", "prep",   "enhance", "prompt",
        "ask",   "variables", "composite", "context", "steps",  "cfg_scale", "mask", "model"};
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

std::string endpoint_names() {
    return "generations | edits | upscale | chat";
}

const char* extent_source_name(ExtentSource source) {
    switch (source) {
        case ExtentSource::layer_alpha:
            return "layer";
        case ExtentSource::canvas:
            return "canvas";
    }
    return "layer";
}

std::optional<ExtentSource> extent_source_from_name(const std::string& name) {
    if (name == "layer") {
        return ExtentSource::layer_alpha;
    }
    if (name == "canvas") {
        return ExtentSource::canvas;
    }
    return std::nullopt;
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

std::vector<const Recipe*> RecipeSet::selection_intents() const {
    std::vector<const Recipe*> out;
    for (const Recipe& recipe : recipes) {
        // An edit or an upscale aimed at the selection. A generation has nothing to select: its
        // region is the document, and asking what to remove from it is the wrong question.
        if (recipe.region == RegionScope::selection &&
            (recipe.endpoint == Endpoint::edits || recipe.endpoint == Endpoint::upscale)) {
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
    } else {
        error = origin + ": 'region' must be 'selection' or 'document' (got '" + region + "')";
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
    recipe.enhance = string_at(body, "enhance", kEnhanceNever, origin, error, ok);
    if (!ok) {
        return std::nullopt;
    }
    if (recipe.enhance != kEnhanceNever && recipe.enhance != kEnhanceAlways) {
        error = origin + ": 'enhance' must be 'never' or 'always' (got '" + recipe.enhance + "')";
        return std::nullopt;
    }

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
    // Which of those defaults the user is asked to replace. Everything not listed runs on its
    // default, which is how a headless `ai.run` and the panel agree on one set of values - and how
    // `context_desc` stays unwritten until the `enhance` role fills it.
    if (body.contains("ask")) {
        if (!body.at("ask").is_array()) {
            error = origin + ": 'ask' must be an array of 'variables' names";
            return std::nullopt;
        }
        for (std::size_t i = 0; i < body.at("ask").size(); ++i) {
            const nlohmann::json& name = body.at("ask")[i];
            if (!name.is_string()) {
                error = origin + ": 'ask[" + std::to_string(i) + "]' must be a string";
                return std::nullopt;
            }
            const std::string variable = name.get<std::string>();
            if (!recipe.variables.contains(variable)) {
                error = origin + ": 'ask' names '" + variable +
                        "', which 'variables' gives no default for, so it would be asked for and "
                        "then refused as unknown";
                return std::nullopt;
            }
            recipe.ask.push_back(variable);
        }
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
                              {"feather", "colour_match", "lock_band_px", "extent"});
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
    const std::string extent = string_at(composite, "extent", "layer", composite_origin, error, ok);
    if (!ok) {
        return std::nullopt;
    }
    const std::optional<ExtentSource> parsed_extent = extent_source_from_name(extent);
    if (!parsed_extent.has_value()) {
        error = composite_origin +
                ": 'extent' must be 'layer' (an edit, received into the layer's own alpha) or "
                "'canvas' (a generation) - got '" +
                extent + "'";
        return std::nullopt;
    }
    recipe.extent = *parsed_extent;

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
    return set;
}

std::optional<std::string> build_prompt(const Recipe& recipe,
                                        const std::map<std::string, std::string>& given,
                                        std::string& error) {
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
