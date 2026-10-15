// SPDX-License-Identifier: Apache-2.0
#include "app/ai_verbs.h"

#include "app/ai_intent.h"

#include "app/command_args.h"
#include "app/state.h"
#include "core/log.h"

#include <string>

namespace noise::app {

using nlohmann::json;

const std::vector<VerbInfo> kVerbs{
    {VerbId::remove, "inpaint-remove", "Ctrl+Backspace"},
    {VerbId::fill, "region-fill", "Ctrl+Shift+F"},
    {VerbId::change, "inpaint-replace", "Ctrl+Shift+C"},
    {VerbId::generate, "new-image", "Ctrl+G"},
};

namespace {

// What a verb requires of the recipe it drives, in the words the refusal uses. `verb_matches` is the
// same statement as a predicate: one list, two shapes, so the sentence and the rule cannot drift.
std::string expectation(VerbId id) {
    switch (id) {
        case VerbId::remove:
            return "a 'reconstruct' that asks for nothing - the marked area is the instruction";
        case VerbId::fill:
            return "a 'reconstruct' that asks for one field (what should appear)";
        case VerbId::change:
            return "an 'in_place' that asks for one field (what should change)";
        case VerbId::generate:
            return "a 'from_words' that asks for one field (the picture)";
    }
    return "an unknown verb";
}

}  // namespace

const char* verb_name(VerbId id) {
    switch (id) {
        case VerbId::remove:
            return "remove";
        case VerbId::fill:
            return "fill";
        case VerbId::change:
            return "change";
        case VerbId::generate:
            return "generate";
    }
    return "?";
}

std::optional<VerbId> verb_from_name(std::string_view name) {
    for (const VerbInfo& verb : kVerbs) {
        if (name == verb_name(verb.id)) {
            return verb.id;
        }
    }
    return std::nullopt;
}

std::string verb_names() {
    std::string out;
    for (const VerbInfo& verb : kVerbs) {
        if (!out.empty()) {
            out += ", ";
        }
        out += verb_name(verb.id);
    }
    return out;
}

const VerbInfo* find_verb(VerbId id) {
    for (const VerbInfo& verb : kVerbs) {
        if (verb.id == id) {
            return &verb;
        }
    }
    return nullptr;
}

const ai::Recipe* verb_recipe(noise::AppState& state, const VerbInfo& verb) {
    return state.ai.recipes.find(verb.recipe);
}

const ai::Recipe* pressed_recipe(noise::AppState& state, const VerbInfo& verb) {
    const ai::Recipe* head = state.ai.recipes.find(verb.recipe);
    if (head == nullptr) {
        return nullptr;
    }
    for (const ai::Recipe* member : state.ai.recipes.family(head->id)) {
        if (member->id == state.ai_intent.recipe) {
            return member;
        }
    }
    return head;
}

bool verb_matches(const VerbInfo& verb, const ai::Recipe& recipe) {
    switch (verb.id) {
        case VerbId::remove:
            return recipe.operation == ai::Operation::reconstruct && recipe.ask.empty();
        case VerbId::fill:
            return recipe.operation == ai::Operation::reconstruct && recipe.ask.size() == 1;
        case VerbId::change:
            return recipe.operation == ai::Operation::in_place && recipe.ask.size() == 1;
        case VerbId::generate:
            return recipe.operation == ai::Operation::from_words && recipe.ask.size() == 1;
    }
    return false;
}

std::string verb_table_error(const ai::RecipeSet& recipes, const std::vector<VerbInfo>& verbs) {
    for (const VerbInfo& verb : verbs) {
        const std::string id = verb_name(verb.id);
        const ai::Recipe* found = recipes.find(verb.recipe);
        if (found == nullptr) {
            return "the verb '" + id + "' drives recipe '" + verb.recipe +
                   "', which the recipe table does not define";
        }
        for (const VerbInfo& other : verbs) {
            if (other.id != verb.id && verb.recipe == other.recipe) {
                return "the verbs '" + std::string(verb_name(other.id)) + "' and '" + id +
                       "' both drive recipe '" + verb.recipe +
                       "'; two keys for one policy is two names for one thing";
            }
        }
        if (!verb_matches(verb, *found)) {
            return "the verb '" + id + "' needs " + expectation(verb.id) + ", and recipe '" +
                   verb.recipe + "' is '" + ai::operation_name(found->operation) + "' asking for " +
                   std::to_string(found->ask.size()) + " field(s)";
        }
        // A stand-in answers to this key, so it has to mean this verb: `load_recipes` made the family
        // interchangeable with itself, which is not the same as interchangeable with the key.
        for (const ai::Recipe* member : recipes.family(found->id)) {
            if (member->id == found->id) {
                continue;
            }
            if (!verb_matches(verb, *member)) {
                return "the verb '" + id + "' offers variant '" + member->id + "', which is '" +
                       ai::operation_name(member->operation) + "' asking for " +
                       std::to_string(member->ask.size()) + " field(s) - " + expectation(verb.id);
            }
        }
    }
    return {};
}

void register_ai_verb_commands(AppState& state, CommandTable& table) {
    // The surface, readable: which verb is offered, what it drives, the key it answers to, and whether
    // it could run now. This is what a window draws, so the window owns no verb list of its own.
    table.add("ai.verbs", "The verbs on offer, the recipe each drives, and whether it can run", {}, {},
              [&state](const json&) -> CommandResult {
        json rows = json::array();
        for (const VerbInfo& verb : kVerbs) {
            const ai::Recipe* recipe = state.ai.recipes.find(verb.recipe);
            const Readiness readiness = assess(state, recipe);
            const std::string table_error = state.ai.verbs_error;
            rows.push_back(json{{"id", verb_name(verb.id)},
                                {"command", std::string("ai.") + verb_name(verb.id)},
                                {"key", verb.key},
                                {"recipe", verb.recipe},
                                {"label", recipe != nullptr ? json(recipe->label) : json(nullptr)},
                                {"operation", recipe != nullptr
                                                  ? json(ai::operation_name(recipe->operation))
                                                  : json(nullptr)},
                                {"ask", recipe != nullptr ? json(recipe->ask) : json::array()},
                                {"scope", recipe == nullptr
                                              ? "unknown"
                                              : (recipe->whole_document() ? "document" : "selection")},
                                // The frames this key can be pressed at, and the recipes it can be
                                // pressed with: the panel draws both from here and owns neither.
                                {"aspects", recipe != nullptr
                                                ? json([&] {
                                                      json names = json::array();
                                                      for (const AspectRatio& offered : recipe->aspects) {
                                                          names.push_back(aspect_name(offered));
                                                      }
                                                      return names;
                                                  }())
                                                : json::array()},
                                {"variants", recipe != nullptr
                                                 ? json([&] {
                                                       json members = json::array();
                                                       for (const ai::Recipe* member
                                                            : state.ai.recipes.family(recipe->id)) {
                                                           if (member->id != recipe->id) {
                                                               members.push_back(json{{"id", member->id},
                                                                                      {"label",
                                                                                       member->label}});
                                                           }
                                                       }
                                                       return members;
                                                   }())
                                                 : json::array()},
                                // Which of them a press would run right now, from the one function the
                                // press itself uses.
                                {"active", pressed_recipe(state, verb) != nullptr
                                               ? json(pressed_recipe(state, verb)->id)
                                               : json(nullptr)},
                                {"ready", table_error.empty() && readiness.ready},
                                {"reason", table_error.empty() ? readiness.reason : table_error},
                                {"fix", readiness.fix},
                                {"model", readiness.model},
                                {"resident", state.ai.health.model_loaded},
                                // Priced before the press: a verb that would swap the model on a shared
                                // box says so, and the press that goes ahead carries `confirm`.
                                {"swap", needs_swap(state, readiness)}});
        }
        return command_ok(json{{"verbs", rows}, {"error", state.ai.verbs_error}});
    });

    for (const VerbInfo& verb : kVerbs) {
        const std::string id = std::string("ai.") + verb_name(verb.id);
        // One argument list for every verb, because what a verb *is* lives in the row and its recipe:
        // the fields, the region and the price all come from there.
        table.add(id, "Run the '" + std::string(verb_name(verb.id)) + "' verb",
                  {verb.key}, {"vars", "seed", "confirm"},
                  [&state, &table, id, verb_id = verb.id](const json& args) -> CommandResult {
            if (!state.ai.verbs_error.empty()) {
                return fail(id, state.ai.verbs_error);
            }
            const VerbInfo* row = find_verb(verb_id);
            const ai::Recipe* recipe =
                row != nullptr ? pressed_recipe(state, *row) : nullptr;
            if (recipe == nullptr) {
                return fail(id, "the verb '" + std::string(verb_name(verb_id)) +
                                    "' has no recipe to drive this run");
            }
            // The same door the window and a driver share, with this verb's recipe named: a press and a
            // `ai.intent {recipe, run}` cannot mean different things.
            json intent{{"recipe", recipe->id}, {"run", true}};
            for (const char* name : {"vars", "seed", "confirm"}) {
                if (args.contains(name)) {
                    intent[name] = args.at(name);
                }
            }
            const CommandResult ran = table.invoke("ai.intent", intent);
            if (!ran.ok) {
                return fail(id, ran.error);
            }
            json out = ran.value;
            out["verb"] = verb_name(verb_id);
            return command_ok(out);
        });
    }
}

}  // namespace noise::app
