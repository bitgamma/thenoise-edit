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

ai::RegionPlan plan_of_landing(const ai::JobRecord& record) {
    // The compositor maps by two rectangles and reads nothing else from a plan, so a reloaded job
    // carries exactly what it needs: the crop it covered and the size the answer was asked for.
    ai::RegionPlan plan;
    plan.crop = record.landing.crop;
    plan.requested = record.landing.requested;
    plan.mask_bounds = record.landing.crop;
    return plan;
}

Mask mask_for_landing(const ai::JobRecord& record, Extent canvas, std::string& error) {
    if (record.landing.coverage_png_path.empty()) {
        return whole_canvas(canvas);
    }
    std::vector<std::uint8_t> bytes;
    if (!io::read_file(record.landing.coverage_png_path, bytes, error)) {
        return Mask{};
    }
    std::string decode_error;
    const Image png = io::decode_png(bytes, decode_error);
    if (png.empty()) {
        error = "job " + record.id + ": its coverage PNG does not decode (" + decode_error + ") at " +
                record.landing.coverage_png_path;
        return Mask{};
    }
    if (png.size() != canvas) {
        error = "job " + record.id + ": its coverage is " + std::to_string(png.size().w) + "x" +
                std::to_string(png.size().h) + " and the document is " +
                std::to_string(canvas.w) + "x" + std::to_string(canvas.h) +
                "; a job lands under the mask it was asked for, so this document is not the one it came from";
        return Mask{};
    }
    return coverage_of_png(png);
}

}  // namespace

void register_ai_job_commands(AppState& state, CommandTable& table) {
    // The handshake `AI-BACKEND.md` describes: alive? what version? what is loaded? then the model list,
    // then the roles resolve against it. Nothing here swaps a model - that is `server.load`, priced and
    // explicit, because a swap costs tens of seconds on a shared box.
    table.add("server.connect", "Shake hands with the server and resolve the roles", {}, {},
              [&state](const json&) -> CommandResult {
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
        log::info("connected to " + state.ai.client->profile().base_url + " (lemonade " +
                  health->version + "), " + std::to_string(models->size()) + " models, " +
                  std::to_string(health->max_models_image) + " resident");
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
            server["transport"] = state.ai.transport != nullptr ? "configured" : "none";
        }
        if (state.ai.registry != nullptr) {
            server["roles"] = state.ai.registry->json();
        }
        if (state.ai.jobs != nullptr) {
            server["latency"] = state.ai.jobs->latency().json();
        }
        // The seed a request takes when its caller names none, so "what made this picture" is
        // answerable from the session rather than from memory.
        server["seed"] = state.ai.seed;
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

    table.add("job.list", "List every job this session and every durable one before it", {}, {},
              [&state](const json&) -> CommandResult {
        CommandResult guard = need_queue("job.list", state);
        if (!guard.ok) {
            return guard;
        }
        state.ai.jobs->pump();
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

    table.add("job.retry", "Send a finished job's request again under a new id", {}, {"id"},
              [&state](const json& args) -> CommandResult {
        CommandResult guard = need_queue("job.retry", state);
        if (!guard.ok) {
            return guard;
        }
        if (!args.contains("id") || !args.at("id").is_string()) {
            return fail("job.retry", "expected string 'id'");
        }
        const std::string from = args.at("id").get<std::string>();
        std::string error;
        const std::optional<std::string> id = state.ai.jobs->retry(from, error);
        if (!id.has_value()) {
            return fail("job.retry", error);
        }
        const std::optional<ai::JobRecord> record = state.ai.jobs->find(*id);
        return command_ok(json{{"job", record.has_value() ? record->json() : json::object()},
                               {"retried", from}});
    });

    // The landing door on the wire: the answer a job produced, under the coverage the job was queued
    // with, through the same function `ai.compose` uses. Previewing is the default for the same reason
    // it is there: an answer arriving in a tray should not move pixels by being looked at.
    table.add("job.apply", "Land a finished job's answer on the document", {},
              {"id", "apply", "into", "feather", "extent", "colour_match"},
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
        state.ai.jobs->pump();
        const std::optional<ai::JobRecord> found = state.ai.jobs->find(id);
        if (!found.has_value()) {
            return fail("job.apply", "no job '" + id + "'");
        }
        const ai::JobRecord& record = *found;
        if (record.state != ai::JobState::succeeded) {
            return fail("job.apply", "job " + id + " is " + ai::job_state_name(record.state) +
                                         ", so there is no answer to land");
        }

        bool apply = false;
        if (!arg_bool(args, "apply", apply)) {
            return fail("job.apply", "'apply' must be a boolean");
        }
        std::string into = "active";
        if (args.contains("into")) {
            if (!args.at("into").is_string()) {
                return fail("job.apply", "'into' must be 'active' or 'new_layer'");
            }
            into = args.at("into").get<std::string>();
        }
        if (into != "active" && into != "new_layer") {
            return fail("job.apply",
                        "'into' is 'active' (the layer the job was queued against) or 'new_layer' "
                        "(an empty layer above it, which is how a full-frame answer gets a local "
                        "opacity); got '" + into + "'");
        }
        if (into == "new_layer" && apply) {
            // The layer is made at landing time, not at queue time: an empty layer that sat there for
            // twenty minutes would be in the way of everything the user did meanwhile.
            state.doc.add_layer(record.recipe_id + " " + record.id, Rgba8{0, 0, 0, 0});
        } else if (into == "new_layer") {
            return fail("job.apply",
                        "a preview writes no layer; the answer is at " + record.result_png);
        }

        std::string error;
        std::vector<std::uint8_t> bytes;
        if (!state.ai.jobs->result_png(id, bytes, error)) {
            return fail("job.apply", error);
        }
        const Image answer = io::decode_png(bytes, error);
        if (answer.empty()) {
            return fail("job.apply", error + " (" + record.result_png + ")");
        }
        const Mask coverage = mask_for_landing(record, state.doc.size(), error);
        if (!coverage.sized()) {
            return fail("job.apply", error);
        }

        ai::CompositePolicy policy;
        policy.feather_px = record.landing.feather_px;
        policy.colour_match = record.landing.colour_match;
        policy.lock_band_px = record.landing.lock_band_px;
        if (!arg_feather(args, "feather", policy.feather_px)) {
            return fail("job.apply", "'feather' must be a non-negative integer or \"auto\"");
        }
        if (!arg_bool(args, "colour_match", policy.colour_match)) {
            return fail("job.apply", "'colour_match' must be a boolean");
        }
        std::string extent_error;
        const std::optional<std::string> extent_name =
            arg_extent_name(args, "extent", record.landing.extent, extent_error);
        if (!extent_name.has_value()) {
            return fail("job.apply", extent_error);
        }

        const AnswerLanding landing{
            .answer = answer,
            .coverage = coverage,
            .plan = plan_of_landing(record),
            .extent = ai::extent_source_from_name(*extent_name)
                          .value_or(ai::ExtentSource::layer_alpha),
            .policy = policy,
            .apply = apply,
            .preview_stem = "ai-job-" + id,
            .coverage_source = "job " + id};
        CommandResult result = land_answer("job.apply", state, landing);
        if (result.ok) {
            result.value["job"] = record.json();
        }
        return result;
    });

    table.add("cache.stats", "Report the request cache", {}, {}, [&state](const json&) -> CommandResult {
        if (state.ai.cache == nullptr) {
            return fail("cache.stats", "no cache is open");
        }
        return command_ok(state.ai.cache->json());
    });
}

}  // namespace noise::app
