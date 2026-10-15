// SPDX-License-Identifier: Apache-2.0
#include "app/command_groups.h"

#include "app/ai_intent.h"
#include "app/ai_landing.h"
#include "app/ai_session.h"
#include "app/command_args.h"
#include "app/state.h"

#include "ai/compose.h"
#include "ai/provider.h"
#include "ai/recipes.h"
#include "ai/region.h"
#include "ai/registry.h"
#include "core/log.h"
#include "core/resample.h"
#include "io/flat.h"
#include "io/png.h"

#include <nlohmann/json.hpp>

#include <map>
#include <optional>
#include <string>
#include <vector>

namespace noise::app {

namespace {

using nlohmann::json;

// What a recipe plus the document plus the caller's arguments amount to: a region, the bytes that would
// go in it, and the request that carries them. Built once and shared by `ai.preview` (which stops here)
// and `ai.run` (which queues it), so what a preview shows is literally what a run sends.
struct Assembly {
    const ai::Recipe* recipe = nullptr;
    ai::RegionPlan plan{};
    Mask coverage{};
    std::string coverage_source;
    std::string prompt;
    std::vector<std::uint8_t> png;
    // The prepped region, written out so a person can look at the exact image that is being uploaded.
    std::string prepped_path;
    ai::BuiltRequest request{};
};

struct Attempt {
    bool ok = false;
    Assembly assembled;
    CommandResult failure{CommandResult{}};
};

std::string join_ids(const ai::RecipeSet& recipes) {
    std::string out;
    for (const std::string& id : recipes.ids()) {
        if (!out.empty()) {
            out += ", ";
        }
        out += id;
    }
    return out.empty() ? "(none loaded)" : out;
}

std::string session_problem(const AppState& state) {
    if (!state.ai.registry) {
        return state.ai.roles_error.empty() ? "no role table is loaded"
                                           : "the role table did not load: " + state.ai.roles_error;
    }
    if (!state.ai.recipes_error.empty()) {
        return "the recipes did not load: " + state.ai.recipes_error;
    }
    if (state.ai.recipes.recipes.empty()) {
        return "no recipes are loaded from '" + state.ai.resource_dir + "/recipes'";
    }
    return {};
}

ai::RegionPolicy policy_for(const AppState& state, const ai::Recipe* recipe) {
    return recipe != nullptr ? state.ai.region_policy(*recipe) : state.ai.table_region_policy();
}

// The region the recipe asks for: the whole document, or the selection compiled to a crop with context.
// Two different claims, and the difference is the recipe's, not the caller's.
Attempt compile_region(const char* command, AppState& state, const ai::Recipe& recipe) {
    Attempt attempt;
    std::string coverage_source;
    Mask coverage;
    if (recipe.whole_document()) {
        coverage = whole_canvas(state.doc.size());
        coverage_source = "whole document (recipe region)";
    } else {
        if (const std::string why = missing_region(recipe, state); !why.empty()) {
            attempt.failure = fail(command, why);
            return attempt;
        }
        // The refusal above is what proves this holds: nothing reaches here for a selection-scope
        // recipe without a selection.
        const std::optional<Mask> selection = state.doc.selection();
        coverage = *selection;
        coverage_source = "selection";
    }

    const std::optional<ai::RegionPlan> plan =
        ai::plan_region(state.doc.size(), coverage, policy_for(state, &recipe));
    if (!plan.has_value()) {
        attempt.failure = fail(command,
                              "the coverage selects nothing, so there is no region to compile: " +
                                  coverage_source);
        return attempt;
    }
    attempt.assembled.recipe = &recipe;
    attempt.assembled.plan = *plan;
    attempt.assembled.coverage = coverage;
    attempt.assembled.coverage_source = coverage_source;
    attempt.ok = true;
    return attempt;
}

// Steps 6 and 7 of the compiler, on the document: the flattened base under the active layer, flattened
// over a colour from the ring, prepped, then resampled to the size the request will ask for. The resample
// is here rather than in `prep_region` because prep is checked against the statistics it preserves, and
// those belong to the crop it was given.
Attempt prep_bytes(const char* command, AppState& state, Assembly assembled, std::uint64_t seed) {
    Attempt attempt;
    const ai::RegionPlan& plan = assembled.plan;
    const Image base = state.doc.flatten_over(plan.crop, static_cast<std::size_t>(state.doc.active()));
    if (base.empty()) {
        attempt.failure = fail(command, "the region lies outside the document, so there is nothing to upload");
        return attempt;
    }
    // The same crop-local coverage for both: the colour the transparency is flattened over, and the hole
    // whose identity prep destroys. Everything else in the crop goes to the model as it stands.
    const Mask hole = assembled.coverage.region(plan.crop);
    const Image flattened = ai::flatten_over_ring(base, hole);
    const Image prepped = ai::prep_region(flattened, hole, assembled.recipe->prep, seed);
    const Image uploaded = resample_lanczos3(prepped, plan.requested);

    std::string error;
    assembled.prepped_path = captures_dir(state) + "/ai-prep-" +
                             uploaded.checksum_hex().substr(0, 12) + ".png";
    if (!io::save_png(assembled.prepped_path, uploaded, error)) {
        attempt.failure = fail(command, error);
        return attempt;
    }
    ++state.upload_encodes;
    assembled.png = io::encode_png(uploaded);
    attempt.assembled = std::move(assembled);
    attempt.ok = true;
    return attempt;
}

Attempt build_assembly(const char* command, AppState& state, const json& args, bool needs_server) {
    CommandResult guard = require_document(state);
    if (!guard.ok) {
        return Attempt{false, Assembly{}, guard};
    }
    const std::string problem = session_problem(state);
    if (!problem.empty()) {
        return Attempt{false, Assembly{}, fail(command, problem)};
    }
    if (needs_server && !state.ai.serving()) {
        return Attempt{false, Assembly{},
                       fail(command, "there is no server to ask: set $THENOISE_URL (and "
                                     "$THENOISE_API_KEY), or point $NOISE_AI_FIXTURES at recordings to "
                                     "replay")};
    }
    if (!args.contains("recipe") || !args.at("recipe").is_string()) {
        return Attempt{false, Assembly{},
                       fail(command, "expected string 'recipe' (one of " +
                                         join_ids(state.ai.recipes) + ")")};
    }
    const std::string recipe_id = args.at("recipe").get<std::string>();
    const ai::Recipe* recipe = state.ai.recipes.find(recipe_id);
    if (recipe == nullptr) {
        return Attempt{false, Assembly{},
                       fail(command, "no recipe '" + recipe_id + "'; loaded: " +
                                         join_ids(state.ai.recipes))};
    }

    Attempt region = compile_region(command, state, *recipe);
    if (!region.ok) {
        return region;
    }

    std::map<std::string, std::string> vars;
    std::string error;
    if (!arg_recipe_vars(args, "vars", vars, error)) {
        return Attempt{false, Assembly{}, fail(command, error)};
    }
    const std::optional<std::string> prompt = ai::build_prompt(*recipe, vars, error);
    if (!prompt.has_value()) {
        return Attempt{false, Assembly{}, fail(command, error)};
    }
    region.assembled.prompt = *prompt;

    // Absent means the session's seed, never a fresh random one: a request must be reproducible from
    // what the screen showed, and a number that appeared from nowhere is not.
    int seed = static_cast<int>(state.ai.seed);
    if (!arg_int(args, "seed", seed)) {
        return Attempt{false, Assembly{}, fail(command, "'seed' must be an integer")};
    }

    Attempt prepped = prep_bytes(command, state, region.assembled, static_cast<std::uint64_t>(seed));
    if (!prepped.ok) {
        return prepped;
    }
    Assembly& assembled = prepped.assembled;

    // The role resolves against what this server answered. Previewing without a connection is allowed
    // only when the caller supplies the numbers the model would have supplied, because guessing them
    // would be the app asserting a fact it has not read.
    const ai::Resolution resolution = state.ai.registry->resolve(recipe->role);
    const ai::ModelEntry* entry = state.ai.registry->resolved_model(recipe->role);
    std::optional<int> steps;
    if (args.contains("steps")) {
        int value = 0;
        if (!want_int(args, "steps", value)) {
            return Attempt{false, Assembly{}, fail(command, "'steps' must be an integer")};
        }
        steps = value;
    }
    std::optional<double> cfg;
    if (args.contains("cfg_scale")) {
        double value = 0.0;
        if (!want_number(args, "cfg_scale", value)) {
            return Attempt{false, Assembly{}, fail(command, "'cfg_scale' must be a number")};
        }
        cfg = value;
    }
    if (!resolution.resolved() && (needs_server || !steps.has_value() || !cfg.has_value())) {
        std::string message = "role '" + recipe->role + "' resolves to no model: " +
                              (resolution.reason.empty() ? "nothing matches" : resolution.reason);
        if (!resolution.fix_command.empty()) {
            message += "; " + resolution.fix_command;
        }
        if (!needs_server) {
            message += " (a preview without a server needs both 'steps' and 'cfg_scale')";
        }
        return Attempt{false, Assembly{}, fail(command, message)};
    }

    ai::RequestInput input;
    input.recipe = recipe;
    input.server_name = state.ai.client != nullptr ? state.ai.client->profile().name : "none";
    input.model_id = resolution.model_id;
    input.model = entry;
    input.steps_override = steps;
    input.cfg_scale_override = cfg;
    input.width = assembled.plan.requested.w;
    input.height = assembled.plan.requested.h;
    input.prompt = assembled.prompt;
    input.seed = static_cast<std::uint64_t>(seed);
    input.png = assembled.png;

    const std::optional<ai::BuiltRequest> request =
        ai::build_request(input, state.ai.roles.size_policy, error);
    if (!request.has_value()) {
        return Attempt{false, Assembly{}, fail(command, error)};
    }
    assembled.request = *request;
    Attempt done;
    done.ok = true;
    done.assembled = std::move(assembled);
    return done;
}

nlohmann::json assembly_json(const AppState& state, const Assembly& assembled,
                   const std::string& role_reason) {
    return json{{"recipe", assembled.recipe->id},
                {"role", assembled.recipe->role},
                {"role_resolution", role_reason},
                {"prompt", assembled.prompt},
                {"prepped", assembled.prepped_path},
                {"prep", ai::prep_name(assembled.recipe->prep)},
                {"region", region_plan_json(assembled.plan, assembled.coverage_source)},
                {"request",
                 ai::outgoing_preview(state.ai.client != nullptr
                                          ? state.ai.client->profile()
                                          : ai::ServerProfile{},
                                      assembled.request)}};
}

}  // namespace

std::optional<RequestQuote> quote_request(AppState& state, const ai::Recipe& recipe) {
    if (state.doc.empty()) {
        return std::nullopt;
    }
    Mask coverage;
    if (recipe.whole_document()) {
        coverage = whole_canvas(state.doc.size());
    } else {
        const std::optional<Mask> selection = state.doc.selection();
        if (!selection.has_value()) {
            return std::nullopt;
        }
        coverage = *selection;
    }
    const std::optional<ai::RegionPlan> plan =
        ai::plan_region(state.doc.size(), coverage, policy_for(state, &recipe));
    if (!plan.has_value()) {
        return std::nullopt;
    }
    RequestQuote quote;
    quote.endpoint = ai::request_path(recipe.endpoint);
    quote.size = plan->requested;
    if (state.ai.registry != nullptr) {
        const ai::ModelEntry* entry = state.ai.registry->resolved_model(recipe.role);
        if (entry != nullptr) {
            quote.model = entry->id;
            if (entry->image_defaults.steps > 0) {
                quote.steps = entry->image_defaults.steps;
            }
        }
    }
    return quote;
}

void register_ai_commands(AppState& state, CommandTable& table) {
    // The result compositor, reached through the same table the UI uses: an answer image from disk, the
    // document's own selection, and the report of what landed where. Before a transport exists this is the
    // only way to see a composite on a real document - and to see it as pixels on the wire, not as a claim.
    table.add("ai.compose", "Land an answer image on the document under the feathered mask", {},
              {"image", "apply", "feather", "colour_match", "lock_band"},
              [&state](const json& args) -> CommandResult {
        CommandResult guard = require_document(state);
        if (!guard.ok) {
            return guard;
        }
        if (!state.ai.registry) {
            return fail("ai.compose",
                        state.ai.roles_error.empty()
                            ? "no role table is loaded, so there is no size policy to compile a region under"
                            : state.ai.roles_error);
        }
        if (!args.contains("image") || !args.at("image").is_string()) {
            return fail("ai.compose", "expected string 'image' (the answer, in any flat format)");
        }
        const std::string path = args.at("image").get<std::string>();
        bool apply = false;
        if (!arg_bool(args, "apply", apply)) {
            return fail("ai.compose", "'apply' must be a boolean");
        }
        ai::CompositePolicy policy = ai::kCompositePolicy;
        if (!arg_feather(args, "feather", policy.feather_px)) {
            return fail("ai.compose",
                        "'feather' must be a non-negative integer or \"auto\" (the recipe's word)");
        }
        if (!arg_bool(args, "colour_match", policy.colour_match)) {
            return fail("ai.compose", "'colour_match' must be a boolean");
        }
        if (args.contains("lock_band") && !want_int(args, "lock_band", policy.lock_band_px)) {
            return fail("ai.compose", "'lock_band' must be an integer");
        }
        if (policy.lock_band_px < 0) {
            return fail("ai.compose",
                        "'lock_band' must not be negative (got " +
                            std::to_string(policy.lock_band_px) + ")");
        }

        // No selection is the whole canvas, which is what a full-frame recipe means; a selection that
        // covers nothing is the other thing the document distinguishes, and it compiles to no region.
        std::string coverage_source = "selection";
        Mask coverage = state.doc.selection().value_or(whole_canvas(state.doc.size()));
        if (!state.doc.selection().has_value()) {
            coverage_source = "whole document (no selection)";
        }
        // No recipe is involved in `ai.compose`, so the size and context policy come from the table
        // directly - the same one a recipe with no `context` block inherits.
        const std::optional<ai::RegionPlan> plan =
            ai::plan_region(state.doc.size(), coverage, policy_for(state, nullptr));
        if (!plan.has_value()) {
            return fail("ai.compose",
                        "the selection covers nothing, so there is no region to compose into");
        }

        std::string error;
        const Image answer = io::load_flat_image(path, error);
        if (answer.empty()) {
            return fail("ai.compose", error + " (" + path + ")");
        }
        const AnswerLanding landing{
            .answer = answer,
            .coverage = coverage,
            .plan = *plan,
            .policy = policy,
            .apply = apply,
            .preview_stem = "ai-compose",
            .coverage_source = coverage_source,
            .above_layer = state.doc.active(),
            .layer_name = "answer " + answer.checksum_hex().substr(0, 8)};
        return land_answer("ai.compose", state, landing);
    });

    table.add("ai.roles", "Report what each role resolved to on the connected server", {}, {},
              [&state](const json&) -> CommandResult {
        if (!state.ai.registry) {
            return fail("ai.roles",
                        state.ai.roles_error.empty() ? "no role table is loaded"
                                                     : state.ai.roles_error);
        }
        return command_ok(state.ai.registry->json());
    });

    table.add("ai.recipes", "List the loaded recipes and the policy each one states", {}, {},
              [&state](const json&) -> CommandResult {
        if (!state.ai.recipes_error.empty()) {
            return fail("ai.recipes", state.ai.recipes_error);
        }
        json rows = json::array();
        for (const ai::Recipe& recipe : state.ai.recipes.recipes) {
            const ai::Resolution resolution = state.ai.registry
                                                  ? state.ai.registry->resolve(recipe.role)
                                                  : ai::Resolution{};
            rows.push_back(json{{"id", recipe.id},
                                {"label", recipe.label},
                                {"source", recipe.source},
                                {"endpoint", ai::endpoint_name(recipe.endpoint)},
                                {"role", recipe.role},
                                // The operation, because it is the fact that decides whether the upload
                                // has a hole in it, and a tool that offers recipes has to be able to say
                                // which of them destroy something.
                                {"operation", ai::operation_name(recipe.operation)},
                                {"region", recipe.whole_document() ? "document" : "selection"},
                                {"prep", ai::prep_name(recipe.prep)},
                                {"ask", recipe.ask},
                                {"enhance", recipe.enhance},
                                {"composite", composite_policy_json(recipe.composite)},
                                {"model", resolution.model_id},
                                {"model_reason", resolution.reason}});
        }
        return command_ok(json{{"recipes", rows},
                               {"directory", state.ai.resource_dir + "/recipes"}});
    });

    // The compiled request, without sending it: the field list, the resolved size, the prepped PNG that
    // would be uploaded and its blob digest. `AGENTS.md` §3 asks for every feature to be observable;
    // this is the request half, observable before it costs anyone's GPU time.
    table.add("ai.preview", "Compile a recipe against the document and report the exact request", {},
              {"recipe", "vars", "seed", "steps", "cfg_scale"},
              [&state](const json& args) -> CommandResult {
        const Attempt attempt = build_assembly("ai.preview", state, args, /*needs_server=*/false);
        if (!attempt.ok) {
            return attempt.failure;
        }
        const ai::Recipe& recipe = *attempt.assembled.recipe;
        const ai::Resolution resolution = state.ai.registry->resolve(recipe.role);
        json result = assembly_json(state, attempt.assembled, resolution.reason);
        result["model"] = resolution.model_id;
        result["policy"] = composite_policy_json(recipe.composite);
        return command_ok(result);
    });

    table.add("ai.run", "Queue a recipe against the document", {},
              {"recipe", "vars", "seed", "steps", "cfg_scale"},
              [&state](const json& args) -> CommandResult {
        const Attempt attempt = build_assembly("ai.run", state, args, /*needs_server=*/true);
        if (!attempt.ok) {
            return attempt.failure;
        }
        const Assembly& assembled = attempt.assembled;
        const ai::Recipe& recipe = *assembled.recipe;

        ai::Landing landing;
        landing.crop = assembled.plan.crop;
        landing.requested = assembled.plan.requested;
        landing.canvas = state.doc.size();
        landing.feather_px = recipe.composite.feather_px;
        landing.colour_match = recipe.composite.colour_match;
        landing.lock_band_px = recipe.composite.lock_band_px;
        landing.target_layer = state.doc.active();
        landing.document_checksum_before = state.doc.composite().checksum_hex();
        landing.document_epoch = state.document_epoch;

        const std::string id = state.ai.jobs->enqueue(assembled.request, recipe.id, recipe.role,
                                                      landing, coverage_png(assembled.coverage));
        log::info("queued " + recipe.id + " as job " + id + " (" +
                  std::to_string(assembled.plan.requested.w) + "x" +
                  std::to_string(assembled.plan.requested.h) + " at " + assembled.request.path + ")");
        const std::optional<ai::JobRecord> record = state.ai.jobs->find(id);
        return command_ok(json{{"job", record.has_value() ? record->json() : json::object()},
                               {"prepped", assembled.prepped_path},
                               {"region", region_plan_json(assembled.plan, assembled.coverage_source)}});
    });
}

}  // namespace noise::app
