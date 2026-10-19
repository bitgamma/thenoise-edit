// SPDX-License-Identifier: Apache-2.0
#include "app/ai_intent.h"

#include "app/ai_landing.h"

#include "ai/queue.h"
#include "ai/recipes.h"
#include "ai/registry.h"
#include "ai/server.h"
#include "app/command_args.h"
#include "app/state.h"
#include "core/brush.h"
#include "core/log.h"

#include <algorithm>
#include <optional>
#include <string_view>
#include <vector>

namespace noise::app {
namespace {

// The flagship of the P0 flow, and the panel's starting point. Where no such recipe is loaded the
// panel takes the first intent the table offers rather than showing nothing; which operation is the
// product's headline is a decision, so it is written down once, here.
constexpr const char* kDefaultIntent = "inpaint-remove";

const ai::Recipe* intent_recipe(AppState& state) {
    if (!state.ai_intent.recipe.empty()) {
        if (const ai::Recipe* found = state.ai.recipes.find(state.ai_intent.recipe)) {
            return found;
        }
    }
    if (const ai::Recipe* flagship = state.ai.recipes.find(kDefaultIntent)) {
        return flagship;
    }
    const std::vector<const ai::Recipe*> intents = state.ai.recipes.selection_intents();
    return intents.empty() ? nullptr : intents.front();
}

std::string joined(const std::vector<std::string>& names) {
    std::string out;
    for (const std::string& name : names) {
        if (!out.empty()) {
            out += ", ";
        }
        out += name;
    }
    return out;
}

// The names a value may be given for a recipe: the fields it asks a person to fill, and the defaults
// it fills for itself (`context_desc`, until `enhance` does). Anything else is a name this recipe never
// mentioned, and the refusal says so with both lists.
bool accepts(const ai::Recipe& recipe, const std::string& name) {
    return recipe.variables.contains(name) ||
           std::find(recipe.ask.begin(), recipe.ask.end(), name) != recipe.ask.end();
}

std::string declared_names(const ai::Recipe& recipe) {
    std::vector<std::string> names;
    for (const std::string& name : recipe.ask) {
        names.push_back(name + " (asked for)");
    }
    for (const auto& [name, value] : recipe.variables) {
        names.push_back(name);
    }
    return joined(names);
}

std::string recipe_ids(const AppState& state) {
    std::vector<std::string> ids;
    for (const ai::Recipe* intent : state.ai.recipes.selection_intents()) {
        ids.push_back(intent->id);
    }
    return joined(ids);
}

// The values in force: the recipe's defaults with what the caller gave on top. Names the recipe does
// not declare never get here - the command refuses them where they were typed.
nlohmann::json effective_vars(const ai::Recipe& recipe,
                              const std::map<std::string, std::string>& given) {
    json out = json::object();
    for (const auto& [name, value] : recipe.variables) {
        out[name] = value;
    }
    for (const auto& [name, value] : given) {
        if (accepts(recipe, name)) {
            out[name] = value;
        }
    }
    return out;
}



// The box's own measured time for this exact (model, size, steps). Nothing is invented when it has not
// answered one yet - the status bar says there is no estimate, which is why `server.state` reports the
// samples it is built from.
std::optional<double> estimate_seconds(AppState& state, const std::string& model,
                                       const std::optional<RequestQuote>& quote) {
    if (!quote.has_value() || !quote->steps.has_value() || model.empty() ||
        state.ai.jobs == nullptr) {
        return std::nullopt;
    }
    return state.ai.jobs->latency().predict(model, quote->size, *quote->steps);
}

// What the intent's key or row could run. Each row carries the model its role resolves to and whether
// choosing it would mean loading a different one: on a shared box the swap is the expensive part, not the
// style.
nlohmann::json family_json(AppState& state, const ai::Recipe& recipe) {
    // The head's family, so choosing a style does not narrow the offer to the thing already chosen.
    const ai::Recipe* head = state.ai.recipes.family_head(recipe.id);
    if (head == nullptr) {
        return json::array();
    }
    json members = json::array();
    for (const ai::Recipe* member : state.ai.recipes.family(head->id)) {
        const Readiness member_readiness = assess(state, member);
        members.push_back(json{{"id", member->id},
                              {"label", member->label},
                              {"model", member_readiness.model},
                              {"swap", needs_swap(state, member_readiness)}});
    }
    return members;
}

// The Run button. A swap is the expensive thing in this design, so it is confirmed rather than
// discovered: the refusal says which model, which model is resident and what it costs, and the same
// call with `confirm` goes ahead.
CommandResult start_intent(AppState& state, CommandTable& table, const ai::Recipe* recipe,
                          const Readiness& readiness, bool confirm) {
    const std::string id = recipe != nullptr ? recipe->id : std::string();
    if (recipe == nullptr) {
        return fail("ai.intent", "nothing to run: " + readiness.reason);
    }
    if (!readiness.ready) {
        return fail("ai.intent", "cannot run '" + id + "': " + readiness.reason +
                                     (readiness.fix.empty() ? "" : " - " + readiness.fix));
    }
    // Rendered before the swap is priced, because the swap is the expensive refusal and this one is
    // free: a request with no words in it must never ask a shared box to load a model first. The dev
    // box found the reverse order - a bare *Generate* refused for the resident model, which read as if
    // loading Flux would have made the button work.
    std::string words_error;
    if (!ai::build_prompt(*recipe, state.ai_intent.given, words_error).has_value()) {
        return fail("ai.intent", words_error);
    }
    if (needs_swap(state, readiness) && !confirm) {
        return fail("ai.intent",
                    "'" + id + "' needs " + readiness.model + " and the server is running " +
                        state.ai.health.model_loaded + "; loading it costs tens of seconds on a "
                        "shared box. Re-run with confirm, or load it yourself with server.load");
    }
    // Passed to `ai.run` rather than read back out of the intent, so the run door takes what a press
    // meant.
    json run{{"recipe", recipe->id}, {"vars", effective_vars(*recipe, state.ai_intent.given)}};
    if (state.ai_intent.aspect.has_value()) {
        run["aspect"] = aspect_name(*state.ai_intent.aspect);
    }
    const CommandResult started = table.invoke("ai.run", run);
    if (!started.ok) {
        return fail("ai.intent", "could not queue '" + id + "': " + started.error);
    }
    state.ai_intent.job = started.value.contains("job") &&
                                  started.value["job"].contains("id")
                              ? started.value["job"]["id"].get<std::string>()
                              : std::string();
    log::info("intent " + id + " queued as " + state.ai_intent.job);
    return started;
}

}  // namespace

// Whether this intent could run here, and what stops it if it cannot. The panel greys the button from
// this and the refusal repeats it, so a disabled control and an error message cannot disagree.
std::string missing_region(const ai::Recipe& recipe, noise::AppState& state) {
    if (recipe.whole_document() || state.doc.selection().has_value()) {
        return {};
    }
    return "recipe '" + recipe.id + "' works on a selection and there is none: mark the area (" +
           std::string(tool_names()) + " draw one, or `select.rect`), or use a recipe whose region "
           "is the document";
}

bool needs_swap(const noise::AppState& state, const Readiness& readiness) {
    return readiness.ready && !readiness.model.empty() && !state.ai.health.model_loaded.empty() &&
           readiness.model != state.ai.health.model_loaded;
}

Readiness assess(noise::AppState& state, const ai::Recipe* recipe) {
    Readiness out;
    if (!state.ai.roles_error.empty()) {
        out.reason = state.ai.roles_error;
        out.fix = "$NOISE_EDIT_ASSETS";
        return out;
    }
    if (!state.ai.recipes_error.empty()) {
        out.reason = state.ai.recipes_error;
        out.fix = "$NOISE_EDIT_ASSETS";
        return out;
    }
    if (state.doc.empty()) {
        out.reason = "no document is open";
        return out;
    }
    if (recipe == nullptr) {
        out.reason = "no recipe that works on a selection is loaded";
        out.fix = "assets/recipes/*.json";
        return out;
    }
    if (!missing_region(*recipe, state).empty()) {
        out.reason = missing_region(*recipe, state);
        out.fix = "select.rect";
        return out;
    }
    if (!state.ai.connected) {
        out.reason = state.ai.connect_error.empty() ? "not connected to a server"
                                                   : state.ai.connect_error;
        out.fix = "server.connect";
        return out;
    }
    if (state.ai.registry == nullptr) {
        out.reason = "no role table is loaded";
        return out;
    }
    const ai::Resolution resolution = state.ai.registry->resolve(recipe->role);
    if (!resolution.resolved()) {
        out.reason = "nothing fills the '" + recipe->role + "' role: " + resolution.reason;
        out.fix = resolution.fix_command;
        return out;
    }
    out.ready = true;
    out.model = resolution.model_id;
    return out;
}

nlohmann::json intent_json(AppState& state) {
    const ai::Recipe* recipe = intent_recipe(state);
    const AiIntent& intent = state.ai_intent;
    const Readiness readiness = assess(state, recipe);

    json intents = json::array();
    for (const ai::Recipe* candidate : state.ai.recipes.selection_intents()) {
        intents.push_back(json{{"id", candidate->id}, {"label", candidate->label}});
    }

    json selection{{"whole_document", !state.doc.selection().has_value()},
                   {"bounds", json::array({0, 0, 0, 0})},
                   {"pixels", 0}};
    if (const std::optional<Mask>& mask = state.doc.selection(); mask.has_value()) {
        selection["bounds"] = json::array({mask->bounds().x, mask->bounds().y, mask->bounds().w,
                                           mask->bounds().h});
        selection["pixels"] = mask->covered();
    }

    json job = nullptr;
    if (!intent.job.empty() && state.ai.jobs != nullptr) {
        if (const std::optional<ai::JobRecord> record = state.ai.jobs->find(intent.job);
            record.has_value()) {
            job = record->json();
        }
    }

    const std::optional<RequestQuote> quote =
        recipe != nullptr ? quote_request(state, *recipe, intent.aspect) : std::nullopt;
    json quote_json = nullptr;
    if (quote.has_value()) {
        quote_json = json{{"endpoint", quote->endpoint},
                          {"size", json::array({quote->size.w, quote->size.h})},
                          {"steps", quote->steps.has_value() ? json(*quote->steps) : json(nullptr)}};
    }
    const std::optional<double> estimate = estimate_seconds(state, readiness.model, quote);

    return json{{"recipe", recipe != nullptr ? json(recipe->id) : json(nullptr)},
                {"label", recipe != nullptr ? json(recipe->label) : json(nullptr)},
                {"ask", recipe != nullptr ? json(recipe->ask) : json::array()},
                {"vars", recipe != nullptr ? effective_vars(*recipe, intent.given)
                                           : json::object()},
                {"intents", intents},
                {"aspects", recipe != nullptr ? json([&] {
                               json names = json::array();
                               for (const AspectRatio& offered : recipe->aspects) {
                                   names.push_back(aspect_name(offered));
                               }
                               return names;
                           }())
                                           : json::array()},
                {"aspect", intent.aspect.has_value() ? json(aspect_name(*intent.aspect))
                                                     : json(nullptr)},
                {"family", recipe != nullptr ? family_json(state, *recipe) : json::array()},
                {"seed", static_cast<std::int64_t>(state.ai.seed)},
                {"selection", selection},
                {"connected", state.ai.connected},
                // Whether a server is configured at all: `$THENOISE_URL` unset means there is nothing
                // to connect to, which the panel says instead of offering a button that cannot work.
                {"configured",
                 state.ai.client != nullptr && !state.ai.client->profile().base_url.empty()},
                {"server",
                 state.ai.client != nullptr ? state.ai.client->profile().name : std::string()},
                {"connect_attempted", state.ai.connect_attempted},
                {"ready", readiness.ready},
                {"reason", readiness.reason},
                {"fix", readiness.fix},
                {"model", readiness.model},
                {"resident", state.ai.health.model_loaded},
                {"swap", needs_swap(state, readiness)},
                {"estimate_s", estimate.has_value() ? json(*estimate) : json(nullptr)},
                // Geometry and tables only: what a press would send and roughly how long it takes. The
                // request itself - and the digest a repeat is served from - exists once something is
                // compiled: `ai.preview` on purpose, or a run.
                {"quote", quote_json},
                {"upload_encodes", state.upload_encodes},
                {"job", job},
                {"error", intent.error}};
}

namespace {

// What `ai.intent` does with its arguments. Nothing here records the refusal on the intent: the caller
// that owns the command does, so there is one place where a failed write becomes something the panel can
// show and one place where a successful one clears it.
CommandResult apply_intent(AppState& state, CommandTable& table, const json& args) {
    // The job row is read from the queue's record, so the queue is advanced first: a panel that
    // only read would otherwise show a job as running until some other command happened to pump.
    if (state.ai.jobs != nullptr) {
        state.ai.jobs->pump();
        pump_ai_landings(state);
    }

    AiIntent& intent = state.ai_intent;
    if (!args.empty()) {
        intent.error.clear();
    }

    if (args.contains("recipe")) {
        if (!args.at("recipe").is_string()) {
            return fail("ai.intent", "'recipe' must be a recipe id from its 'intents' list");
        }
        const std::string requested = args.at("recipe").get<std::string>();
        if (requested != intent.recipe) {
            const ai::Recipe* chosen = state.ai.recipes.find(requested);
            if (chosen == nullptr) {
                const std::string known = recipe_ids(state);
                return fail("ai.intent", "no intent '" + requested + "' on a selection (" +
                                             (known.empty() ? "this table offers none"
                                                             : "this table offers " + known) +
                                             ")");
            }
            intent.recipe = requested;
            // Values typed for one intent carry over only where the next asks the same question,
            // so a `replacement` cannot ride silently into a recipe that never asked for one.
            std::erase_if(intent.given, [chosen](const auto& entry) {
                return !accepts(*chosen, entry.first);
            });
            // Carried over only where the next recipe offers the same frame, for the reason the words are.
            if (!aspect_refusal(*chosen, intent.aspect).empty()) {
                intent.aspect = std::nullopt;
            }
        }
    }

    if (args.contains("vars")) {
        if (!args.at("vars").is_object()) {
            return fail("ai.intent", "'vars' must be an object of string values");
        }
        const ai::Recipe* chosen = intent_recipe(state);
        for (auto it = args.at("vars").begin(); it != args.at("vars").end(); ++it) {
            if (!it.value().is_string()) {
                return fail("ai.intent", "vars." + it.key() + " must be a string (a prompt is text)");
            }
            if (chosen == nullptr || !accepts(*chosen, it.key())) {
                return fail("ai.intent", "recipe '"
                                             + (chosen != nullptr ? chosen->id : intent.recipe) +
                                             "' neither asks for nor declares '" + it.key() +
                                             "' (it has " +
                                             (chosen != nullptr ? declared_names(*chosen) : "none") +
                                             ")");
            }
            intent.given[it.key()] = it.value().get<std::string>();
        }
    }

    // Not a `vars` entry, so it is validated against the recipe rather than passed through.
    std::optional<AspectRatio> aspect = intent.aspect;
    if (!arg_aspect(args, "aspect", aspect)) {
        return fail("ai.intent",
                    "'aspect' must be an \"W:H\" ratio the recipe offers, or null to put the choice back");
    }
    if (args.contains("aspect")) {
        const ai::Recipe* chosen = intent_recipe(state);
        if (chosen == nullptr) {
            return fail("ai.intent", "nothing is being asked for, so there is no aspect to choose");
        }
        const std::string refusal = aspect_refusal(*chosen, aspect);
        if (!refusal.empty()) {
            return fail("ai.intent", refusal);
        }
    }
    intent.aspect = aspect;

    // The seed the panel shows is the session's, so typing one changes what every later run
    // without an explicit seed would send. That is deliberate: the number on the screen is the
    // number the request carried.
    int seed = static_cast<int>(state.ai.seed);
    if (!arg_int(args, "seed", seed)) {
        return fail("ai.intent", "'seed' must be an integer");
    }
    if (seed < 0) {
        return fail("ai.intent", "'seed' must not be negative (got " + std::to_string(seed) + ")");
    }
    state.ai.seed = static_cast<std::uint64_t>(seed);

    if (args.contains("job")) {
        if (!args.at("job").is_string()) {
            return fail("ai.intent", "'job' must be a job id");
        }
        intent.job = args.at("job").get<std::string>();
    }

    // The panel's first-frame handshake, and the Connect button - the same `server.connect` the
    // wire drives. `once` is why it is a command rather than a line in the widget: the attempt
    // has to be recorded somewhere the tests can see, and the answer must say what happened.
    bool connect = false;
    if (!arg_bool(args, "connect", connect)) {
        return fail("ai.intent", "'connect' must be a boolean");
    }
    if (connect) {
        // No "already tried" guard here: the caller decides. The host asked once at startup, and
        // after that only the button asks, and a refresh of a session that exists is worth having.
        state.ai.connect_attempted = true;
        const CommandResult handshake = table.invoke("server.connect", json::object());
        if (!handshake.ok) {
            intent.error = handshake.error;
        }
    }

    bool run = false;
    if (!arg_bool(args, "run", run)) {
        return fail("ai.intent", "'run' must be a boolean");
    }
    bool confirm = false;
    if (!arg_bool(args, "confirm", confirm)) {
        return fail("ai.intent", "'confirm' must be a boolean");
    }
    if (run) {
        const CommandResult started =
            start_intent(state, table, intent_recipe(state), assess(state, intent_recipe(state)),
                         confirm);
        if (!started.ok) {
            return started;
        }
    }

    // Reading costs nothing, and a reader exists on every frame (the status line). So nothing here
    // compiles: no prep, no resample, no PNG encode. The price on screen is `quote_request` - the
    // region compiler's geometry and the model's own defaults - and the exact request, its digest
    // and its bytes appear when someone asks for them (`ai.preview`) or runs.
    return command_ok(intent_json(state));

}

}  // namespace

void register_ai_intent_commands(AppState& state, CommandTable& table) {
    // The panel's whole surface. Reading it with no arguments is free; passing anything recompiles
    // the preview, because the request is the price of the action and the panel has to show it before
    // the button is pressed rather than after.
    //
    // A refusal is recorded here rather than inside `apply_intent`, so that there is one place where a
    // failed write becomes something the panel can show. Without it a words box that will not take what
    // was typed - a field its recipe never asked for, a recipe that no longer exists - draws nothing,
    // and the person is left typing into a widget connected to nothing.
    table.add("ai.intent", "Read or set what the selection panel is asking for", {},
              {"recipe", "vars", "aspect", "seed", "job", "run", "confirm", "connect"},
              [&state, &table](const json& args) -> CommandResult {
        const CommandResult applied = apply_intent(state, table, args);
        if (!applied.ok && !args.empty()) {
            state.ai_intent.error = applied.error;
        }
        return applied;
    });
}

}  // namespace noise::app
