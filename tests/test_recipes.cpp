// SPDX-License-Identifier: Apache-2.0
#include "test.h"

#include "ai/recipes.h"
#include "ai/registry.h"

#include "io/file.h"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <string>
#include <vector>

using noise::ai::build_prompt;
using noise::ai::Endpoint;
using noise::ai::load_recipes;
using noise::ai::load_roles;
using noise::ai::parse_recipe;
using noise::ai::Recipe;
using noise::ai::RecipeSet;
using noise::ai::RoleTable;
using noise::ai::RegionScope;
using nlohmann::json;

namespace {

json minimal(const char* id = "test-recipe") {
    return json{{"id", id},
               {"label", "Test intent"},
                {"endpoint", "edits"},
                {"role", "edit"},
                {"region", "selection"},
                {"operation", "reconstruct"},
                {"prep", "blur"},
                {"enhance", "never"},
                {"prompt", json::array({"Remove {{target}}.", "Match the {{context_desc}}."})},
                {"composite", json{{"feather", "auto"}}}};
}

Recipe parse_or_fail(const json& body) {
    std::string error;
    const std::optional<Recipe> recipe = parse_recipe(body, "test", error);
    CHECK(recipe.has_value());
    if (!recipe.has_value()) {
        std::fprintf(stderr, "  recipe parse: %s\n", error.c_str());
        return Recipe{};
    }
    return *recipe;
}

std::vector<std::uint8_t> bytes_of(const std::string& text) {
    return std::vector<std::uint8_t>(text.begin(), text.end());
}

std::string parse_refusal(const json& body) {
    std::string error;
    const std::optional<Recipe> recipe = parse_recipe(body, "recipes/test.json", error);
    CHECK(!recipe.has_value());
    return error;
}

// The pairing that keeps a Replace from uploading a blurred face. Asserted as refusals, because a
// recipe file that contradicts itself has to be caught when it is loaded, not while a person waits for
// an answer that cannot relate to their picture.
void test_an_operation_and_a_hole_are_the_same_concept_or_neither() {
    json edit_with_hole = minimal();
    edit_with_hole["operation"] = "in_place";
    CHECK(parse_refusal(edit_with_hole).find("must have prep 'none'") != std::string::npos);

    json reconstruct_without_hole = minimal();
    reconstruct_without_hole["prep"] = "none";
    CHECK(parse_refusal(reconstruct_without_hole).find("needs a prep for the hole") !=
          std::string::npos);

    json reconstruct_whole_canvas = minimal();
    reconstruct_whole_canvas["region"] = "document";
    CHECK(parse_refusal(reconstruct_whole_canvas).find("works on a selection") != std::string::npos);

    json generate_from_an_edit = minimal();
    generate_from_an_edit["operation"] = "from_words";
    generate_from_an_edit["prep"] = "none";  // so this is the contradiction reached, not the one before
    CHECK(parse_refusal(generate_from_an_edit).find("endpoint 'generations'") != std::string::npos);

    json nonsense = minimal();
    nonsense["operation"] = "smudge";
    CHECK(parse_refusal(nonsense).find("'operation' must be") != std::string::npos);

    // A recipe is not allowed to inherit a hole by omission: no `operation` means no parse.
    json unstated = minimal();
    unstated.erase("operation");
    CHECK(parse_refusal(unstated).find("'operation' is required") != std::string::npos);

    // The legal shapes still load, and say what they are.
    CHECK_EQ(operation_name(parse_or_fail(minimal()).operation), std::string("reconstruct"));
    json edit = minimal();
    edit["operation"] = "in_place";
    edit["prep"] = "none";
    CHECK(operation_name(parse_or_fail(edit).operation) == std::string("in_place"));
    json transform = minimal();
    transform["operation"] = "transform";
    transform["prep"] = "none";
    transform["endpoint"] = "upscale";
    transform["role"] = "upscale";
    CHECK(operation_name(parse_or_fail(transform).operation) == std::string("transform"));
    json generate = minimal();
    generate["operation"] = "from_words";
    generate["prep"] = "none";
    generate["endpoint"] = "generations";
    generate["role"] = "generation";
    generate["region"] = "document";
    CHECK(operation_name(parse_or_fail(generate).operation) == std::string("from_words"));
}

void test_a_recipe_reads_as_the_policy_it_claims_to_be() {
    const Recipe recipe = parse_or_fail(minimal());
    CHECK(recipe.endpoint == Endpoint::edits);
    CHECK(recipe.region == RegionScope::selection);
    CHECK(!recipe.whole_document());
    CHECK_EQ(recipe.enhance, std::string("never"));
    // Two lines, joined in order: a prompt written as a paragraph cannot be varied line by line, and a
    // recipe file that reads like prose is the thing the doc asked for.
    std::string error;
    const std::optional<std::string> prompt =
        build_prompt(recipe, {{"target", "the lamp"}, {"context_desc", "wall"}}, error);
    REQUIRE(prompt.has_value());
    // Lines are joined with newlines: the prompt the server sees keeps the recipe's line structure,
    // which is what makes "one line, one concern" in a recipe file mean something.
    CHECK_EQ(*prompt, std::string("Remove the lamp.\nMatch the wall."));
    CHECK(error.empty());

    json wide = minimal();
    // Whole-document and no hole, so this one has to be an edit: a `reconstruct` may not span the
    // document (there would be no area to reconstruct) and an edit may not carry a prep.
    wide["operation"] = "in_place";
    wide["prep"] = "none";
    wide["region"] = "document";
    wide["context"] = {{"ratio", 0.5}, {"min_px", 96}};
    wide["composite"] = {
        {"feather", 3}, {"colour_match", false}, {"lock_band_px", 4}};
    const Recipe grown = parse_or_fail(wide);
    CHECK(grown.whole_document());
    CHECK_NEAR(*grown.context_ratio, 0.5, 1e-12);
    CHECK_EQ(*grown.context_min_px, 96);
    CHECK_EQ(grown.composite.feather_px.value_or(-1), 3);
    CHECK(!grown.composite.colour_match);
    CHECK_EQ(grown.composite.lock_band_px, 4);

    // `"feather": "auto"` is the null the compositor asks for, not a number that happens to be zero.
    json auto_feather = minimal();
    auto_feather["composite"] = {{"feather", "auto"}};
    CHECK(!parse_or_fail(auto_feather).composite.feather_px.has_value());
}

void test_a_recipe_that_states_the_impossible_is_refused_by_name() {
    json steps = minimal();
    steps["steps"] = 20;
    CHECK(parse_refusal(steps).find("image_defaults") != std::string::npos);

    json model = minimal();
    model["model"] = "Flux-2-Klein-9B-TheNoise";
    CHECK(parse_refusal(model).find("registry") != std::string::npos);

    // The engine has no mask field: precision is region and prep, and a recipe carrying a mask would be
    // a promise nothing keeps.
    json masked = minimal();
    masked["mask"] = "always";
    CHECK(parse_refusal(masked).find("mask") != std::string::npos);

    // A prompt written as one string is the shape the doc rejected, and silently accepting it would let
    // a recipe stop being variable line by line.
    json prose = minimal();
    prose["prompt"] = "Remove the lamp. Match the wall.";
    CHECK(parse_refusal(prose).find("prompt") != std::string::npos);

    for (const char* field : {"endpoint", "region", "prep", "enhance"}) {
        json bad = minimal();
        bad[field] = "sometimes";
        const std::string error = parse_refusal(bad);
        CHECK(error.find(field) != std::string::npos);
    }

    // An unknown key is refused: a policy nobody reads is worse than a missing one.
    CHECK(parse_refusal([&] {
              json extra = minimal();
              extra["steps_per_second"] = 4;
              return extra;
          }())
              .find("steps_per_second") != std::string::npos);

    // A placeholder with no value and no default cannot be guessed.
    std::string error;
    CHECK(!build_prompt(parse_or_fail(minimal()), {{"target", "the lamp"}}, error).has_value());
    CHECK(error.find("context_desc") != std::string::npos);
}

// The shipped set is the product: it has to load, and it has to agree with the role table it names. A
// recipe pointing at a role nothing defines would sit in the palette and fail at the server.
void test_the_shipped_recipes_agree_with_the_shipped_roles() {
    std::string error;
    const std::optional<RecipeSet> set =
        load_recipes(std::string(NOISE_TEST_ASSETS_DIR) + "/recipes", error);
    REQUIRE(set.has_value());
    const std::optional<RoleTable> roles =
        load_roles(std::string(NOISE_TEST_ASSETS_DIR) + "/roles.json", error);
    REQUIRE(roles.has_value());

    const std::vector<Recipe>& recipes = set->recipes;
    CHECK(recipes.size() >= std::size_t(10));
    for (const Recipe& recipe : recipes) {
        // File name and id are the same fact twice; when they differ, `ai.recipes` lists one and a
        // caller types the other.
        CHECK_EQ(recipe.id, noise::ai::recipe_id_of(recipe.source));
        CHECK(roles->find(recipe.role) != nullptr);
        // An endpoint with nothing to resolve it is a recipe that cannot run.
        const bool generation = recipe.endpoint == Endpoint::generations;
        const bool upscale = recipe.endpoint == Endpoint::upscale;
        CHECK_EQ(static_cast<int>(generation) + static_cast<int>(upscale) +
                     static_cast<int>(recipe.endpoint == Endpoint::edits),
                 1);
        // `enhance` is unused in the MVP: a recipe that asked for it would be a command that silently
        // degrades, because the role has no preference list to resolve from.
        CHECK_EQ(recipe.enhance, std::string("never"));
        if (recipe.endpoint != Endpoint::upscale) {
            CHECK(!recipe.prompt_lines.empty());
        }
    }

    // The commands the doc promises, by id.
    for (const char* id : {"new-image", "new-image-photo", "new-image-anime", "inpaint-remove",
                           "inpaint-replace", "region-fill", "heal", "flatten-colorize", "reimagine",
                           "upscale"}) {
        CHECK(set->find(id) != nullptr);
    }
    CHECK(set->find("no-such-recipe") == nullptr);

    // The file name is the id: two ids for one policy means `ai.recipes` lists one thing and a caller
    // types another, so a set that disagrees with itself is refused rather than half-loaded.
    const std::filesystem::path scratch =
        std::filesystem::temp_directory_path() / "noise-recipes-test";
    std::filesystem::remove_all(scratch);
    std::filesystem::create_directories(scratch);
    std::string write_error;
    const std::vector<std::uint8_t> body = bytes_of(minimal("heal").dump());
    REQUIRE(noise::io::write_file_atomic((scratch / "heal.json").string(), body, write_error));
    REQUIRE(noise::io::write_file_atomic((scratch / "other.json").string(), body, write_error));
    std::string load_error;
    CHECK(!load_recipes(scratch.string(), load_error).has_value());
    CHECK(load_error.find("file name says") != std::string::npos);
    std::filesystem::remove_all(scratch);

    // `upscale` is the one recipe whose size is not its own: it asks for a scale, and the region
    // compiler is what turns that into pixels.
    const Recipe* upscale = set->find("upscale");
    REQUIRE(upscale != nullptr);
    CHECK(upscale->endpoint == Endpoint::upscale);
    CHECK(upscale->role == std::string("upscale"));
    CHECK(!upscale->whole_document());
}


// What the interface calls a recipe, and what it asks a person to type: both are data, so adding an
// operation means adding a file and not touching a panel. A missing label would be a button wearing
// its own id, and an `ask` name the prompt does not declare would be collected from the user and then
// refused at the request builder.
void test_a_recipe_says_what_it_is_called_and_what_it_asks() {
    CHECK(parse_refusal([&] {
        json body = minimal();
        body.erase("label");
        return body;
    }()).find("'label' is required") != std::string::npos);
    CHECK(parse_refusal([&] {
        json body = minimal();
        body["label"] = "";
        return body;
    }()).find("'label'") != std::string::npos);

    const std::string unknown = parse_refusal([&] {
        json body = minimal();
        body["variables"] = {{"target", "object"}};
        body["ask"] = json::array({"target", "colour"});
        return body;
    }());
    CHECK(unknown.find("'colour'") != std::string::npos);
    CHECK(unknown.find("ask") != std::string::npos);

    Recipe asked = parse_or_fail([&] {
        json body = minimal();
        body["variables"] = {{"target", "object"}, {"replacement", "another object"}};
        body["ask"] = json::array({"replacement", "target"});
        return body;
    }());
    CHECK_EQ(asked.label, std::string("Test intent"));
    // The order the file states: a form whose fields reorder themselves every load is a form that
    // puts words in the wrong box.
    REQUIRE(asked.ask.size() == std::size_t(2));
    CHECK_EQ(asked.ask[0], std::string("replacement"));
    CHECK_EQ(asked.ask[1], std::string("target"));
}

// The intents a selection offers are the recipes whose region is the selection and whose endpoint
// takes pixels: `new-image` and `reimagine` are not things you do to a rectangle. Derived from the
// set, so a new recipe file appears in the palette without anything else being edited - and a recipe
// that stopped being a selection operation disappears from it.
void test_a_selection_offers_the_selection_recipes() {
    std::string error;
    const std::optional<RecipeSet> set =
        load_recipes(std::string(NOISE_TEST_ASSETS_DIR) + "/recipes", error);
    REQUIRE(set.has_value());

    std::vector<std::string> offered;
    for (const Recipe* intent : set->selection_intents()) {
        offered.push_back(intent->id);
        CHECK(intent->region == RegionScope::selection);
        CHECK(intent->endpoint == Endpoint::edits || intent->endpoint == Endpoint::upscale);
    }
    const std::vector<std::string> wanted = {"heal", "inpaint-remove", "inpaint-replace",
                                             "region-fill", "upscale"};
    CHECK_EQ(offered, wanted);
    for (const char* id : {"new-image", "new-image-photo", "reimagine", "flatten-colorize"}) {
        CHECK(std::find(offered.begin(), offered.end(), std::string(id)) == offered.end());
    }

    // Words a person would recognise, not the file names, and the flagship asks the one question the
    // flagship needs; `Replace` asks the second one.
    for (const Recipe* intent : set->selection_intents()) {
        CHECK(!intent->label.empty());
        CHECK(intent->label != intent->id);
    }
    const Recipe* remove = set->find("inpaint-remove");
    const Recipe* replace = set->find("inpaint-replace");
    REQUIRE(remove != nullptr);
    REQUIRE(replace != nullptr);
    CHECK_EQ(remove->label, std::string("Remove"));
    CHECK_EQ(remove->ask, (std::vector<std::string>{"target"}));
    CHECK_EQ(replace->label, std::string("Replace"));
    CHECK_EQ(replace->ask, (std::vector<std::string>{"target", "replacement"}));
    // Asking nothing is legal and means nothing is asked: `heal` runs on what is under the selection
    // and `upscale` on the whole of it, with no words from anyone.
    CHECK(set->find("heal")->ask.empty());
    CHECK(set->find("upscale")->ask.empty());
}

}  // namespace

int main() {
    test_an_operation_and_a_hole_are_the_same_concept_or_neither();
    test_a_recipe_reads_as_the_policy_it_claims_to_be();
    test_a_recipe_that_states_the_impossible_is_refused_by_name();
    test_the_shipped_recipes_agree_with_the_shipped_roles();
    test_a_recipe_says_what_it_is_called_and_what_it_asks();
    test_a_selection_offers_the_selection_recipes();
    return noise::test::summary(__FILE__);
}
