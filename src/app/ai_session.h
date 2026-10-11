// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <memory>
#include <string>

#include "ai/http.h"
#include "ai/queue.h"
#include "ai/recipes.h"
#include "ai/registry.h"
#include "ai/server.h"
#include "ai/transport.h"

namespace noise {
struct AppState;
}

namespace noise::app {

// Everything the AI path holds between commands: the two tables, the server, the queue. It is a member
// of `AppState` rather than a set of globals so a test harness can build one, point it at a fixture
// directory, and see exactly what the app would do.
// The seed a request takes when its caller names none. Zero is a seed like any other and it keeps a
// recipe reproducible across restarts; the panel shows it, `ai.intent {seed}` changes it, and variants
// ask for `seed + i` explicitly rather than inventing one per attempt.
inline constexpr std::uint64_t kSessionSeed = 0;

struct AiSession {
    // Where `roles.json` and `recipes/*.json` live. Set by the host, because nothing in the command
    // layer reaches for an ambient directory.
    std::string resource_dir;
    ai::RoleTable roles;
    // Non-empty when the table could not be read. Every AI command then refuses with this instead of
    // resolving roles against an empty table, which would look like "no model is downloaded".
    std::string roles_error;
    ai::RecipeSet recipes;
    std::string recipes_error;
    // Non-empty when a verb row no longer matches the recipe it drives (ai_verbs.h). Every verb
    // command then refuses with this: half a verb surface working is half the labels lying.
    std::string verbs_error;

    std::unique_ptr<ai::Transport> transport;
    // True when that transport answers from recordings rather than a socket. Reported by `server.state`
    // and used by the handshake's log line: without it a replay says "connected to <the URL in
    // $THENOISE_URL>", which is a sentence about a machine that was never contacted.
    bool replaying = false;
    std::unique_ptr<ai::Client> client;
    std::unique_ptr<ai::JobQueue> jobs;
    std::unique_ptr<ai::Registry> registry;
    // The seed the AI commands use when the call names none. One field, one home: nothing else may
    // decide what "unseeded" means.
    std::uint64_t seed = kSessionSeed;

    // What the last `server.connect` decided. Not a latch: a failed connect leaves the commands refusing
    // with the reason, and the next attempt starts from nothing.
    bool connected = false;
    // Set by whoever asks for the handshake - the host once at startup, the panel's button thereafter.
    // The automatic attempt is a single one because a server that is switched off costs a connect
    // timeout on the frame it happens, and a retry every frame would make the window unusable.
    bool connect_attempted = false;
    std::string connect_error;
    ai::ServerHealth health;

    // The region policy a recipe compiles under: the table's size policy, the recipe's context where it
    // names one. Assembled here rather than stored, so there is one place that knows how the two halves
    // combine.
    ai::RegionPolicy region_policy(const ai::Recipe& recipe) const;
    // The same, with no recipe involved: what a caller that named no recipe compiles under.
    ai::RegionPolicy table_region_policy() const;

    bool tables_loaded() const { return registry != nullptr && roles_error.empty(); }
    bool serving() const { return client != nullptr && jobs != nullptr; }
};

// Loads the tables and chooses a transport: recordings if `$NOISE_AI_FIXTURES` names a directory, the
// live server if `$THENOISE_URL` is set, nothing otherwise. Called once by the host after it has set
// `runtime_dir`; a failure to load is recorded in the session and reported by the commands that need it,
// because a bad `roles.json` must not stop the editor.
void open_ai_session(noise::AppState& state);

}  // namespace noise::app
