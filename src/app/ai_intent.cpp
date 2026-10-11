// SPDX-License-Identifier: Apache-2.0
#include "app/ai_intent.h"

#include "ai/queue.h"
#include "ai/recipes.h"
#include "ai/registry.h"
#include "ai/server.h"
#include "app/command_args.h"
#include "app/state.h"
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

// The arguments that change what would be sent. `job` only points the panel at a job to display, so it
// must not pay for a region encode; the rest alter the request, and - unlike a document change - none of
// them move the compile key, which is why an asked-for compile ignores the settle rule.
bool changes_request(const nlohmann::json& args) {
    for (const char* name : {"recipe", "vars", "seed", "connect", "run", "confirm"}) {
        if (args.contains(name)) {
            return true;
        }
    }
    return false;
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

CompileKey compile_key(const AppState& state) {
    CompileKey key;
    key.revision = state.doc.revision();
    key.layer = static_cast<std::size_t>(state.doc.active());
    if (const std::optional<Mask>& selection = state.doc.selection(); selection.has_value()) {
        key.selection = true;
        key.bounds = selection->bounds();
        key.covered = selection->covered();
    }
    return key;
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

struct Readiness {
    bool ready = false;
    std::string reason;
    std::string fix;
    std::string model;
};

// Whether this intent could run here, and what stops it if it cannot. The panel greys the button from
// this and the refusal repeats it, so a disabled control and an error message cannot disagree.
Readiness assess(AppState& state, const ai::Recipe* recipe) {
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

bool needs_swap(const AppState& state, const Readiness& readiness) {
    return readiness.ready && !readiness.model.empty() && !state.ai.health.model_loaded.empty() &&
           readiness.model != state.ai.health.model_loaded;
}

std::string field_text(const nlohmann::json& preview, std::string_view name) {
    if (!preview.is_object() || !preview.contains("request") ||
        !preview["request"].contains("fields")) {
        return {};
    }
    for (const json& pair : preview["request"]["fields"]) {
        if (pair.is_array() && pair.size() == 2 && pair[0].is_string() &&
            pair[0].get<std::string>() == name) {
            return pair[1].is_string() ? pair[1].get<std::string>() : pair[1].dump();
        }
    }
    return {};
}

std::optional<Extent> preview_size(const nlohmann::json& preview) {
    if (!preview.is_object() || !preview.contains("request") ||
        !preview["request"].contains("size") || !preview["request"]["size"].is_string()) {
        return std::nullopt;
    }
    const std::string size = preview["request"]["size"].get<std::string>();
    const std::string::size_type cross = size.find('x');
    if (cross == std::string::npos) {
        return std::nullopt;
    }
    try {
        return Extent{std::stoi(size.substr(0, cross)), std::stoi(size.substr(cross + 1))};
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

// The box's own measured time for this exact (model, size, steps). Nothing is invented when it has
// not answered one yet - the panel says there is no estimate, which is why `server.state` reports the
// samples it is built from.
std::optional<double> estimate_seconds(AppState& state, const std::string& model,
                                       const nlohmann::json& preview) {
    const std::optional<Extent> size = preview_size(preview);
    const std::string steps = field_text(preview, "steps");
    if (!size.has_value() || steps.empty() || model.empty() || state.ai.jobs == nullptr) {
        return std::nullopt;
    }
    try {
        return state.ai.jobs->latency().predict(model, *size, std::stoi(steps));
    } catch (const std::exception&) {
        return std::nullopt;
    }
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
    if (needs_swap(state, readiness) && !confirm) {
        return fail("ai.intent",
                    "'" + id + "' needs " + readiness.model + " and the server is running " +
                        state.ai.health.model_loaded + "; loading it costs tens of seconds on a "
                        "shared box. Re-run with confirm, or load it yourself with server.load");
    }
    const CommandResult started = table.invoke(
        "ai.run", json{{"recipe", recipe->id}, {"vars", effective_vars(*recipe, state.ai_intent.given)}});
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

    const std::optional<double> estimate =
        estimate_seconds(state, readiness.model, intent.preview);

    return json{{"recipe", recipe != nullptr ? json(recipe->id) : json(nullptr)},
                {"label", recipe != nullptr ? json(recipe->label) : json(nullptr)},
                {"ask", recipe != nullptr ? json(recipe->ask) : json::array()},
                {"vars", recipe != nullptr ? effective_vars(*recipe, intent.given)
                                           : json::object()},
                {"intents", intents},
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
                {"preview", intent.has_preview ? intent.preview : json(nullptr)},
                {"stale", !intent.has_preview || !(intent.compiled == compile_key(state))},
                {"job", job},
                {"error", intent.error}};
}

void register_ai_intent_commands(AppState& state, CommandTable& table) {
    // The panel's whole surface. Reading it with no arguments is free; passing anything recompiles
    // the preview, because the request is the price of the action and the panel has to show it
    // before the button is pressed rather than after.
    table.add("ai.intent", "Read or set what the selection panel is asking for", {},
              {"recipe", "vars", "seed", "job", "run", "confirm", "connect"},
              [&state, &table](const json& args) -> CommandResult {
        // The job row is read from the queue's record, so the queue is advanced first: a panel that
        // only read would otherwise show a job as running until some other command happened to pump.
        if (state.ai.jobs != nullptr) {
            state.ai.jobs->pump();
        }

        AiIntent& intent = state.ai_intent;
        // A read does not forget the refusal it is being asked to display: the panel calls this every
        // frame, so clearing on a read would make the reason flash once and vanish.
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
                intent.error = started.error;
                return started;
            }
        }

        // Reading costs nothing: a panel asks this every frame. The preview is compiled by whoever
        // changed something, and a reader that found it out of date is told through `stale` rather
        // than paying for a region encode on a frame that only wanted the selection bounds.
        // A refusal is the intent's own error, and `error` carries it to the panel.
        const ai::Recipe* recipe = intent_recipe(state);
        const Readiness readiness = assess(state, recipe);
        // The preview is what makes Run live, so it has to follow the document without being asked -
        // a rectangle drawn on the canvas is not an argument to this command, and a panel that only
        // recompiled on its own widgets left Run greyed forever behind a perfectly good selection.
        // What it must not do is encode a region on every frame of a marquee drag, so a change that
        // this command was not told about compiles once its key has held still for one read. An
        // argument means someone changed a value, which does not move the key at all, so that always
        // recompiles; a compile that failed is attempted once per key rather than per frame.
        const CompileKey key = compile_key(state);
        const bool asked = changes_request(args);
        const bool region_moved = !intent.has_preview || !(intent.compiled == key);
        const bool attempted = intent.has_attempted && intent.attempted == key;
        const bool settled = intent.has_seen && intent.seen == key;
        if (recipe != nullptr && readiness.ready &&
            (asked || (region_moved && settled && !attempted))) {
            const CommandResult compiled = table.invoke(
                "ai.preview", json{{"recipe", recipe->id}, {"vars", effective_vars(*recipe, intent.given)}});
            intent.has_attempted = true;
            intent.attempted = key;
            if (compiled.ok) {
                intent.preview = compiled.value;
                intent.compiled = key;
                intent.has_preview = true;
                intent.error.clear();
            } else {
                intent.has_preview = false;
                intent.preview = json();
                intent.error = compiled.error;
            }
        }
        intent.seen = key;
        intent.has_seen = true;
        return command_ok(intent_json(state));
    });
}

}  // namespace noise::app
