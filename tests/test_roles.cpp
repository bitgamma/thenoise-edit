// SPDX-License-Identifier: Apache-2.0
#include "test.h"

#include "ai/profile.h"
#include "ai/registry.h"

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

using noise::ai::ContextPolicy;
using noise::ai::load_roles;
using noise::ai::ModelEntry;
using noise::ai::parse_health;
using noise::ai::parse_models;
using noise::ai::parse_roles;
using noise::ai::Registry;
using noise::ai::RoleTable;
using nlohmann::json;

namespace {

ModelEntry entry(const std::string& id, const std::string& recipe,
                 const std::vector<std::string>& labels, bool downloaded, double size_gb) {
    ModelEntry model;
    model.id = id;
    model.recipe = recipe;
    model.labels = labels;
    model.downloaded = downloaded;
    model.size_gb = size_gb;
    model.image_defaults = noise::ai::ImageDefaults{4, 1.0, 1024, 1024};
    return model;
}

RoleTable table_or_fail(const json& body) {
    std::string error;
    const std::optional<RoleTable> table = parse_roles(body, "test", error);
    CHECK(table.has_value());
    if (!table.has_value()) {
        std::fprintf(stderr, "  roles parse: %s\n", error.c_str());
        return RoleTable{};
    }
    return *table;
}

// The table the app ships, because it is the file every command reads: its policy numbers are what the
// region compiler hands the engine, and a silent edit to one of them changes every request.
void test_the_shipped_table_states_the_policy_the_compiler_uses() {
    std::string error;
    const std::optional<RoleTable> loaded =
        load_roles(std::string(NOISE_TEST_ASSETS_DIR) + "/roles.json", error);
    REQUIRE(loaded.has_value());
    const RoleTable& table = *loaded;
    CHECK_EQ(table.size_policy.multiple, 16);
    CHECK_EQ(table.size_policy.max_area_px, 1024 * 1024);
    CHECK_NEAR(table.context_policy.ratio, 0.35, 1e-12);
    CHECK_EQ(table.context_policy.min_px, 64);
    for (const char* role :
         {"generate.photo", "generate.illustration", "edit", "upscale", "enhance"}) {
        CHECK(table.find(role) != nullptr);
    }
    // Model ids live here and nowhere else: `edit` names candidates rather than trusting a label.
    CHECK(table.find("edit")->preference.size() > 1);
    // `enhance` is the one role the MVP does not use, and it is declared by capability only: which
    // runner a vision model happens to use is the server's business.
    CHECK(table.find("enhance")->recipe.empty());
    CHECK(table.find("upscale")->requires_labels == std::vector<std::string>{"upscaling"});
}

void test_a_role_table_refuses_to_be_half_read() {
    // Every one of these would otherwise leave a role that resolves to nothing while the file looks
    // fine, which is the failure a table of data has to be able to refuse.
    std::string error;
    CHECK(!parse_roles(json{{"roles", json::array()}}, "t", error).has_value());
    CHECK(error.find("roles") != std::string::npos);
    // A role with no predicate is an arbitrary pick, so the file has to state one.
    CHECK(!parse_roles(json{{"roles", json::array({json{{"id", "a"}, {"recipe", "r"}}})}}, "t", error)
                 .has_value());
    CHECK(error.find("requires") != std::string::npos);
    error.clear();
    CHECK(!parse_roles(json{{"roles",
                             json::array({json{{"id", "a"},
                                               {"recipe", "r"},
                                               {"requires", json::array()},
                                               {"preference", json::array()}}})}},
                        "t",
                        error)
                 .has_value());
    CHECK(error.find("requires") != std::string::npos);

    error.clear();
    CHECK(!parse_roles(json{{"roles",
                             json::array({json{{"id", "a"},
                                               {"recipe", "r"},
                                               {"requires", json::array({"l"})},
                                               {"preference", "Klein"}}})}},
                        "t",
                        error)
                 .has_value());
    CHECK(error.find("preference") != std::string::npos);

    error.clear();
    const json duplicated =
        json{{"roles", json::array({json{{"id", "a"}, {"recipe", "r"}, {"requires", json::array({"l"})}},
                                    json{{"id", "a"}, {"recipe", "r"}, {"requires", json::array({"l"})}}})}};
    CHECK(!parse_roles(duplicated, "t", error).has_value());
    CHECK(error.find("duplicate") != std::string::npos);

    error.clear();
    CHECK(!parse_roles(json{{"size_policy", {{"multiple", 0}}},
                            {"roles", json::array({json{{"id", "a"}, {"recipe", "r"}}})}},
                        "t",
                        error)
                 .has_value());
    CHECK(error.find("multiple") != std::string::npos);
}

// Resolution order: assignment, then preference, then capability. The last one exists because the list
// is the box's, today: a role that waited for a file edit before using a capable model would make the
// file a second source of truth about what can run.
void test_resolution_follows_assignment_then_preference_then_capability() {
    RoleTable table = table_or_fail(json{{"size_policy", {{"multiple", 16}}},
                                         {"roles",
                                          json::array({json{{"id", "edit"},
                                                            {"recipe", "thenoise"},
                                                            {"requires", json::array({"image", "edit"})},
                                                            {"preference",
                                                             json::array({"Klein-9B-KV", "Klein-9B"})}},
                                                       json{{"id", "upscale"},
                                                            {"recipe", "thenoise"},
                                                            {"requires", json::array({"upscaling"})},
                                                            {"preference", json::array()}}})}});
    Registry registry(table);

    registry.update({entry("Klein-9B", "thenoise", {"image", "edit"}, true, 25.0),
                     entry("Qwen", "thenoise", {"image", "edit"}, true, 17.0),
                     entry("SD-Turbo", "sd-cpp", {"image", "edit"}, true, 3.0)});
    const auto edit = registry.resolve("edit");
    CHECK(edit.resolved());
    CHECK_EQ(edit.model_id, std::string("Klein-9B"));
    CHECK(edit.from_preference);
    // A `sd-cpp` model with the right labels serves nothing: the golden rule is engine-specific.
    CHECK_EQ(edit.candidates.size(), std::size_t(2));

    registry.set_assignment("edit", "Qwen");
    CHECK_EQ(registry.resolve("edit").model_id, std::string("Qwen"));
    CHECK(registry.resolve("edit").reason.find("assignment") != std::string::npos);

    // An assignment that stops being available falls back rather than resolving to nothing: boxes swap
    // what they have downloaded, and the user's choice should not become a permanent failure.
    registry.update({entry("Klein-9B", "thenoise", {"image", "edit"}, true, 25.0)});
    CHECK_EQ(registry.resolve("edit").model_id, std::string("Klein-9B"));

    // Nothing in the preference list, but something that can serve: resolved, with the reason saying so.
    registry.set_assignment("edit", "");
    registry.update({entry("New-Editor", "thenoise", {"image", "edit"}, true, 9.0)});
    const auto off = registry.resolve("edit");
    CHECK_EQ(off.model_id, std::string("New-Editor"));
    CHECK(!off.from_preference);
    CHECK(off.reason.find("capability") != std::string::npos);

    // Among several capable models nobody named, the smaller download wins: nothing here may prefer a
    // model by a quality claim the discovery data does not carry.
    registry.update({entry("Big", "thenoise", {"image", "edit"}, true, 30.0),
                     entry("Small", "thenoise", {"image", "edit"}, true, 12.0)});
    CHECK_EQ(registry.resolve("edit").model_id, std::string("Small"));

    // Unresolved: the reason and the fix must both be actionable, because this is what a disabled
    // command shows.
    // Nothing downloaded, and the thing it wants is offered by the server: the answer is a command.
    registry.update({entry("Qwen", "thenoise", {"image", "edit"}, false, 17.0),
                     entry("Klein-9B-KV", "thenoise", {"image", "edit"}, false, 27.0)});
    const auto none = registry.resolve("edit");
    CHECK(!none.resolved());
    CHECK(none.candidates.empty());
    CHECK_EQ(none.fix_command, std::string("lemonade import Klein-9B-KV"));
    CHECK(none.reason.find("not downloaded") != std::string::npos);

    // Nothing downloaded and the preference is not even offered: no command, because a command that
    // fails is worse than the instruction to look at the list.
    registry.update({entry("Qwen", "thenoise", {"image", "edit"}, false, 17.0)});
    const auto unknown = registry.resolve("edit");
    CHECK(!unknown.resolved());
    CHECK(unknown.fix_command.empty());
    CHECK(unknown.reason.find("offered") != std::string::npos);
}

void test_a_model_that_cannot_serve_a_role_is_not_a_candidate() {
    RoleTable table = table_or_fail(json{{"roles",
                                          json::array({json{{"id", "edit"},
                                                            {"recipe", "thenoise"},
                                                            {"requires", json::array({"image", "edit"})},
                                                            {"preference", json::array()}},
                                                       json{{"id", "vision"},
                                                            {"requires", json::array({"chat", "vision"})},
                                                            {"preference", json::array()}}})}});
    Registry registry(table);
    registry.update({entry("Klein", "thenoise", {"image"}, true, 12.0),
                     entry("Gemma", "llama-cpp", {"chat", "vision"}, true, 4.0)});
    // An image model with no `edit` label is not an editor, however capable it looks.
    CHECK(!registry.resolve("edit").resolved());
    // A role that states no recipe is decided by labels alone, so the runner does not matter.
    CHECK_EQ(registry.resolve("vision").model_id, std::string("Gemma"));
    // Asking for a role the table does not define is a refusal, not a guess.
    CHECK(!registry.resolve("make-me-a-sandwich").resolved());
}

void test_discovery_reads_what_the_server_actually_sends() {
    std::string error;
    const json body = json::parse(R"({"data":[
        {"id":"Klein","recipe":"thenoise","labels":["image","edit"],"downloaded":true,"size":32.5,
         "image_defaults":{"steps":4,"cfg_scale":1.0,"width":1024,"height":1024}},
        {"id":"Noprimes","recipe":"thenoise","labels":["image"],"downloaded":0,"size":"3"},
        {"recipe":"thenoise","labels":["image"]},
        {"name":"NamedOnly","recipe":"thenoise","labels":[]}
    ]})");
    const std::vector<ModelEntry> models = parse_models(body, error);
    CHECK_EQ(models.size(), std::size_t(3));
    CHECK(!error.empty());  // the row with no id is reported, not fatal
    CHECK(models[0].downloaded);
    CHECK_EQ(models[0].image_defaults.steps, 4);
    // `downloaded` arrives as 0/1 on some rows and a bool on others; a string size is not a number.
    CHECK(!models[1].downloaded);
    CHECK_EQ(models[1].size_gb, 0.0);
    CHECK_EQ(models[2].id, std::string("NamedOnly"));
    CHECK(models[0].is_image_model());
    CHECK(!models[2].is_image_model());

    std::string list_error;
    CHECK(parse_models(json{{"models", json::array()}}, list_error).empty());
    CHECK(!list_error.empty());

    std::string health_error;
    const noise::ai::ServerHealth health = parse_health(
        json{{"status", "ok"},
             {"version", "2026.41.0"},
             {"model_loaded", "Klein"},
             {"max_models", {{"image", 1}}},
             {"all_models_loaded", json::array({json{{"model_name", "Klein"}, {"pinned", true}}})}},
        health_error);
    CHECK(health.ok);
    CHECK_EQ(health.model_loaded, std::string("Klein"));
    CHECK_EQ(health.max_models_image, 1);
    CHECK_EQ(health.loaded.size(), std::size_t(1));
    CHECK(health_error.empty());

    std::string bad_error;
    CHECK(!parse_health(json{{"status", "no"}}, bad_error).ok);
    CHECK(!bad_error.empty());
}

}  // namespace

int main() {
    test_the_shipped_table_states_the_policy_the_compiler_uses();
    test_a_role_table_refuses_to_be_half_read();
    test_resolution_follows_assignment_then_preference_then_capability();
    test_a_model_that_cannot_serve_a_role_is_not_a_candidate();
    test_discovery_reads_what_the_server_actually_sends();
    return noise::test::summary(__FILE__);
}
