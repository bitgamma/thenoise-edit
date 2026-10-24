// SPDX-License-Identifier: Apache-2.0
#include "app/command_groups.h"

#include "app/ai_landing.h"
#include "app/ai_session.h"
#include "app/command_args.h"
#include "app/state.h"

#include "ai/recipes.h"
#include "ai/registry.h"
#include "ai/server.h"
#include "core/log.h"
#include "io/file.h"
#include "io/png.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <string>
#include <vector>

namespace noise::app {

namespace {

using nlohmann::json;

CommandResult need_queue(const char* command, const AppState& state) {
    if (state.ai.jobs == nullptr) {
        return fail(command, "there is no server to ask: set $THENOISE_URL (and $THENOISE_API_KEY), or "
                             "point $NOISE_AI_FIXTURES at recordings to replay");
    }
    return command_ok(json::object());
}

}  // namespace

void register_ai_job_commands(AppState& state, CommandTable& table) {
    // The handshake `ARCHITECTURE.md`'s AI path describes: alive? what version? what is loaded? then the model list,
    // then the roles resolve against it. Nothing here swaps a model - that is `server.load`, priced and
    // explicit, because a swap costs tens of seconds on a shared box.
    table.add("server.connect", "Shake hands with the server and resolve the roles", {}, {},
              [&state](const json&) -> CommandResult {
        // Recorded before anything can refuse, so "an attempt was made this run" is true of every call
        // and the host's once-only startup handshake cannot turn into a retry per frame.
        state.ai.connect_attempted = true;
        CommandResult guard = need_queue("server.connect", state);
        if (!guard.ok) {
            return guard;
        }
        if (!state.ai.registry) {
            return fail("server.connect",
                        state.ai.roles_error.empty() ? "no role table is loaded"
                                                     : state.ai.roles_error);
        }
        // Named before a request is attempted: an empty URL reaches libcurl as "unsupported protocol",
        // which is true of the string and useless about the cause.
        if (state.ai.client->profile().base_url.empty()) {
            return fail("server.connect",
                        "no server is configured - set $THENOISE_URL (and $THENOISE_API_KEY "
                        "if the server demands one)");
        }
        ai::AiError error;
        const std::optional<ai::ServerHealth> health = state.ai.client->health(error);
        if (!health.has_value()) {
            state.ai.connected = false;
            state.ai.connect_error = error.message;
            return fail("server.connect", error.message + (error.hint.empty() ? "" : " - " + error.hint));
        }
        const std::optional<std::vector<ai::ModelEntry>> models = state.ai.client->models(error);
        if (!models.has_value()) {
            state.ai.connected = false;
            state.ai.connect_error = error.message;
            return fail("server.connect", error.message + (error.hint.empty() ? "" : " - " + error.hint));
        }
        state.ai.health = *health;
        state.ai.connected = true;
        state.ai.connect_error.clear();
        state.ai.registry->update(*models);
        log::info(std::string(state.ai.replaying
                                  ? "handshake answered by the recordings in $NOISE_AI_FIXTURES"
                                  : "connected to " + state.ai.client->profile().base_url) +
                  " (lemonade " + health->version + "), " + std::to_string(models->size()) +
                  " models, " + std::to_string(health->max_models_image) + " resident");
        return command_ok(json{{"connected", true},
                               {"server", state.ai.client->profile().name},
                               {"base_url", state.ai.client->profile().base_url},
                               {"version", health->version},
                               {"model_loaded", health->model_loaded},
                               {"max_models_image", health->max_models_image},
                               {"models", static_cast<std::size_t>(models->size())},
                               {"models_warning", state.ai.client->models_warning()},
                               {"roles", state.ai.registry->json()}});
    });

    table.add("server.state", "Report the session: server, models resident, roles, latency", {}, {},
              [&state](const json&) -> CommandResult {
        json server{{"connected", state.ai.connected},
                    {"error", state.ai.connect_error},
                    {"tables", state.ai.tables_loaded()},
                    {"recipes_error", state.ai.recipes_error}};
        if (state.ai.client != nullptr) {
            server["server"] = state.ai.client->profile().name;
            server["base_url"] = state.ai.client->profile().base_url;
            server["has_key"] = !state.ai.client->profile().api_key.empty();
            server["prefix"] = state.ai.client->using_alternate_prefix() ? "/api/v1" : "/v1";
            server["model_loaded"] = state.ai.health.model_loaded;
            server["version"] = state.ai.health.version;
            server["request_timeout_s"] = state.ai.client->profile().request_timeout_s;
            server["transport"] = state.ai.transport == nullptr
                                      ? "none"
                                      : (state.ai.replaying ? "recordings" : "libcurl");
        }
        if (state.ai.registry != nullptr) {
            server["roles"] = state.ai.registry->json();
        }
        if (state.ai.jobs != nullptr) {
            server["latency"] = state.ai.jobs->latency().json();
        }
        // The seed the last request carried and whether the next one is pinned to a number or draws
        // its own, so "what made this picture" is answerable from the session rather than memory.
        server["seed"] = state.ai.last_seed;
        server["seed_pinned"] = state.ai.seed_pin.has_value();
        return command_ok(server);
    });

    // Explicit and priced, because a swap is the expensive thing in this design and never happens
    // silently: the commands resolve roles, and if the answer is a different model the user is asked.
    table.add("server.load", "Ask the server to make one model resident", {},
              {"model", "pinned"},
              [&state](const json& args) -> CommandResult {
        CommandResult guard = need_queue("server.load", state);
        if (!guard.ok) {
            return guard;
        }
        if (!args.contains("model") || !args.at("model").is_string()) {
            return fail("server.load", "expected string 'model' (a model id from server.state)");
        }
        bool pinned = false;
        if (!arg_bool(args, "pinned", pinned)) {
            return fail("server.load", "'pinned' must be a boolean");
        }
        const std::string model = args.at("model").get<std::string>();
        ai::AiError error;
        if (!state.ai.client->load_model(model, pinned, error)) {
            return fail("server.load", error.message + (error.hint.empty() ? "" : " - " + error.hint));
        }
        state.ai.health.model_loaded = model;
        log::info("loaded " + model + (pinned ? " (pinned)" : ""));
        return command_ok(json{{"model", model}, {"pinned", pinned}});
    });

    table.add("job.list", "List every job of this run (the queue is in memory, nothing survives it)",
              {}, {},
              [&state](const json&) -> CommandResult {
        CommandResult guard = need_queue("job.list", state);
        if (!guard.ok) {
            return guard;
        }
        state.ai.jobs->pump();
        pump_ai_landings(state);
        return command_ok(state.ai.jobs->json());
    });

    table.add("job.get", "One job: state, what it sent, what came back", {}, {"id"},
              [&state](const json& args) -> CommandResult {
        CommandResult guard = need_queue("job.get", state);
        if (!guard.ok) {
            return guard;
        }
        if (!args.contains("id") || !args.at("id").is_string()) {
            return fail("job.get", "expected string 'id'");
        }
        state.ai.jobs->pump();
        pump_ai_landings(state);
        const std::optional<ai::JobRecord> record = state.ai.jobs->find(args.at("id").get<std::string>());
        if (!record.has_value()) {
            return fail("job.get", "no job '" + args.at("id").get<std::string>() + "'");
        }
        return command_ok(record->json());
    });

    table.add("job.wait", "Block until a job is terminal or the wait runs out", {},
              {"id", "timeout"},
              [&state](const json& args) -> CommandResult {
        CommandResult guard = need_queue("job.wait", state);
        if (!guard.ok) {
            return guard;
        }
        if (!args.contains("id") || !args.at("id").is_string()) {
            return fail("job.wait", "expected string 'id'");
        }
        double timeout = 120.0;
        if (args.contains("timeout") && !want_number(args, "timeout", timeout)) {
            return fail("job.wait", "'timeout' must be a number of seconds");
        }
        const std::string id = args.at("id").get<std::string>();
        const bool terminal = state.ai.jobs->wait_for(id, timeout);
        state.ai.jobs->pump();
        pump_ai_landings(state);
        const std::optional<ai::JobRecord> record = state.ai.jobs->find(id);
        if (!record.has_value()) {
            return fail("job.wait", "no job '" + id + "'");
        }
        if (!terminal) {
            return fail("job.wait", "job " + id + " is still " +
                                        ai::job_state_name(record->state) + " after " +
                                        std::to_string(timeout) + "s");
        }
        return command_ok(record->json());
    });

    table.add("job.cancel", "Stop a job; an answer that already landed is kept", {}, {"id"},
              [&state](const json& args) -> CommandResult {
        CommandResult guard = need_queue("job.cancel", state);
        if (!guard.ok) {
            return guard;
        }
        if (!args.contains("id") || !args.at("id").is_string()) {
            return fail("job.cancel", "expected string 'id'");
        }
        const std::string id = args.at("id").get<std::string>();
        std::string error;
        const bool armed = state.ai.jobs->cancel(id, error);
        state.ai.jobs->pump();
        const std::optional<ai::JobRecord> record = state.ai.jobs->find(id);
        if (!armed) {
            return fail("job.cancel", error);
        }
        return command_ok(json{{"job", record.has_value() ? record->json() : json::object()}});
    });


    // The landing door on the wire. Answers land by themselves as soon as they are finished, so this is
    // the idempotent version of the same thing: "put that answer on the document" for a job that has
    // already landed says which layer holds it and adds nothing. There is no preview mode - the layer is
    // the preview - and no destination: rule A.
    table.add("job.apply", "Land a finished job's answer (idempotent: it lands by itself)", {},
              {"id", "feather", "colour_match"},
              [&state](const json& args) -> CommandResult {
        CommandResult guard = need_queue("job.apply", state);
        if (!guard.ok) {
            return guard;
        }
        guard = require_document(state);
        if (!guard.ok) {
            return guard;
        }
        if (!args.contains("id") || !args.at("id").is_string()) {
            return fail("job.apply", "expected string 'id'");
        }
        const std::string id = args.at("id").get<std::string>();
        std::optional<int> feather;
        if (!arg_feather(args, "feather", feather)) {
            return fail("job.apply", "'feather' must be a non-negative integer or \"auto\"");
        }
        std::optional<bool> colour_match;
        if (args.contains("colour_match")) {
            bool value = false;
            if (!arg_bool(args, "colour_match", value)) {
                return fail("job.apply", "'colour_match' must be a boolean");
            }
            colour_match = value;
        }

        state.ai.jobs->pump();
        pump_ai_landings(state);
        const std::optional<ai::JobRecord> found = state.ai.jobs->find(id);
        if (!found.has_value()) {
            return fail("job.apply", "no job '" + id + "'");
        }
        const ai::JobRecord& record = *found;
        // "Landed" is a claim about the document, so it is checked against the document: undo took the
        // layer away and the answer is still on disk, which is the one case where asking again is the
        // right thing to do. The beat does not do this - an undo is a decision, and an auto-lander that
        // undoes the undo is a fight with the person using the app.
        const bool layer_still_there = std::any_of(
            state.doc.layers().begin(), state.doc.layers().end(),
            [&record](const Layer& layer) { return layer.name == record.landing.landed_layer; });
        if (record.landing.landed && layer_still_there) {
            json reply = record.landing.report.is_object() ? record.landing.report : json::object();
            reply["already_landed"] = true;
            reply["layer_name"] = record.landing.landed_layer;
            reply["job"] = record.json();
            return command_ok(reply);
        }
        if (record.state != ai::JobState::succeeded) {
            return fail("job.apply", "job " + id + " is " + ai::job_state_name(record.state) +
                                         ", so there is no answer to land");
        }
        if (record.landing.document_epoch != state.document_epoch) {
            return fail("job.apply",
                        "job " + id + " was asked for a different document (it was queued under epoch " +
                            std::to_string(record.landing.document_epoch) + ", this one is " +
                            std::to_string(state.document_epoch) +
                            "); its answer belongs to the file it came from");
        }
        CommandResult result = land_job(state, record, "job.apply", feather, colour_match,
                                       /*record_outcome=*/true);
        if (result.ok) {
            if (const std::optional<ai::JobRecord> after = state.ai.jobs->find(id);
                after.has_value()) {
                result.value["job"] = after->json();
            }
        }
        return result;
    });
}

}  // namespace noise::app
