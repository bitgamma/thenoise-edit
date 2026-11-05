// SPDX-License-Identifier: Apache-2.0
#include "app/ai_session.h"

#include "app/ai_verbs.h"

#include "app/state.h"

#include "ai/provider.h"
#include "ai/recorded.h"
#ifndef __EMSCRIPTEN__
#include "ai/transport_curl.h"
#endif
#include "core/log.h"
#include "platform/platform.h"
#include "platform/paths.h"

#include <filesystem>
#include <vector>

#include <utility>

namespace noise::app {

namespace {

#ifndef __EMSCRIPTEN__
constexpr const char* kFixturesVariable = "NOISE_AI_FIXTURES";
// A recording has no host, but a request still needs a URL to be shaped: `.invalid` is reserved and
// can never resolve, so a replay session carries an address that says what it is and cannot leak a
// real one into a log.
constexpr const char* kRecordedUrl = "http://recordings.invalid/v1";

ai::ServerProfile session_profile(bool replaying) {
    std::string error;
    std::optional<ai::ServerProfile> profile = ai::profile_from_environment(error);
    if (profile.has_value()) {
        return *profile;
    }
    if (replaying) {
        ai::ServerProfile recorded;
        recorded.base_url = kRecordedUrl;
        return recorded;
    }
    log::warn("ai: " + error);
    return ai::ServerProfile{};
}
#endif

}  // namespace

std::uint64_t take_seed(AiSession& session) {
    // The draw is whole-width entropy and the field is not, so it is masked here: the number the panel
    // shows is always one `build_request` will accept.
    session.last_seed = session.seed_pin.has_value()
                            ? *session.seed_pin
                            : session.seed_draw() & ai::kMaxSeed;
    return session.last_seed;
}

ai::RegionPolicy AiSession::table_region_policy() const {
    ai::RegionPolicy policy{};
    policy.size_multiple = roles.size_policy.multiple;
    policy.max_area_px = roles.size_policy.max_area_px;
    policy.context_ratio = roles.context_policy.ratio;
    policy.min_context_px = roles.context_policy.min_px;
    return policy;
}

ai::RegionPolicy AiSession::region_policy(const ai::Recipe& recipe) const {
    ai::RegionPolicy policy = table_region_policy();
    if (recipe.context_ratio.has_value()) {
        policy.context_ratio = *recipe.context_ratio;
    }
    if (recipe.context_min_px.has_value()) {
        policy.min_context_px = *recipe.context_min_px;
    }
    // A model that states its own pixel ceiling overrides the table's, because the table's number is a
    // stand-in for a server that says nothing: compiling to something smaller than the model that will
    // answer can serve costs detail the answer never needed to lose.
    if (registry != nullptr) {
        const ai::ModelEntry* entry = registry->resolved_model(recipe.role);
        if (entry != nullptr && entry->image_options.soft_pixel_cap > 0) {
            policy.max_area_px = entry->image_options.soft_pixel_cap;
        }
    }
    return policy;
}

void open_ai_session(noise::AppState& state) {
    AiSession& ai = state.ai;

    if (ai.resource_dir.empty()) {
        std::vector<std::string> candidates;
        if (const auto override_dir = platform::env("NOISE_EDIT_ASSETS");
            override_dir && !override_dir->empty()) {
            candidates.push_back(*override_dir);
        }
        const std::string executable = platform::executable_path();
        if (!executable.empty()) {
            candidates.push_back(platform::path_to_utf8(
                platform::path_from_utf8(executable).parent_path() / NOISE_INSTALL_ASSET_RELATIVE));
        }
        candidates.push_back("assets");
        candidates.push_back(state.runtime_dir + "/assets");
        for (const std::string& candidate : candidates) {
            if (candidate.empty()) {
                continue;
            }
            if (std::filesystem::exists(platform::path_from_utf8(candidate) / "roles.json")) {
                ai.resource_dir = candidate;
                break;
            }
        }
        if (ai.resource_dir.empty()) {
            ai.resource_dir = state.runtime_dir + "/assets";
        }
    }

    std::string error;
    const std::optional<ai::RoleTable> roles =
        ai::load_roles(ai.resource_dir + "/roles.json", error);
    if (roles.has_value()) {
        ai.roles = *roles;
        ai.registry = std::make_unique<ai::Registry>(*roles);
    } else {
        ai.roles_error = error;
        log::error("ai: " + error);
    }

    const std::optional<ai::RecipeSet> recipes =
        ai::load_recipes(ai.resource_dir + "/recipes", error);
    if (recipes.has_value()) {
        ai.recipes = *recipes;
        // The verbs are only as true as the recipes they point at, and they are checked here rather
        // than at the press: a key whose recipe stopped being what the key says is caught once, at
        // startup, and every verb command then refuses with the same sentence.
        ai.verbs_error = verb_table_error(ai.recipes, kVerbs);
        if (!ai.verbs_error.empty()) {
            log::error("ai: " + ai.verbs_error);
        }
    } else {
        ai.recipes_error = error;
        log::error("ai: " + error);
    }

#ifdef __EMSCRIPTEN__
    // Browser builds have no blocking libcurl transport or worker: editing remains fully local.
    // A browser Fetch transport requires a separate asynchronous job path and server CORS support.
    ai.connect_error = "AI generation is unavailable in the browser build; use the native editor";
    return;
#else

    // Recordings first: a probe that means to replay must never reach the network by accident, and a
    // fixture directory that fails to load is reported rather than falling through to a live server.
    bool replaying = false;
    if (const std::optional<std::string> fixtures = platform::env(kFixturesVariable)) {
        std::string load_error;
        std::unique_ptr<ai::RecordedTransport> recorded =
            ai::RecordedTransport::load(*fixtures, load_error);
        if (recorded != nullptr) {
            ai.transport = std::move(recorded);
            ai.replaying = true;
            replaying = true;
            log::info("ai: replaying recordings from the directory in $" +
                      std::string(kFixturesVariable));
        } else {
            ai.connect_error = load_error;
            log::error("ai: " + load_error);
        }
    } else {
        ai.transport = ai::make_curl_transport();
    }

    if (ai.transport != nullptr) {
        ai::ServerProfile profile = session_profile(replaying);
        profile.name = replaying ? "recordings" : "server";
        ai.client = std::make_unique<ai::Client>(*ai.transport, std::move(profile),
                                                 [] { return platform::now_seconds(); });
        ai.jobs = std::make_unique<ai::JobQueue>(
            ai::JobQueue::Deps{ai.client.get(), state.runtime_dir + "/ai/jobs"},
            [] { return platform::now_seconds(); });
    }
#endif
}

}  // namespace noise::app
