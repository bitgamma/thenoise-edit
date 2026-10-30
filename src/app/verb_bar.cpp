// SPDX-License-Identifier: Apache-2.0
#include "app/verb_bar.h"

#include "app/ai_verbs.h"
#include "app/command_args.h"
#include "app/command_groups.h"
#include "app/selection_edit.h"

#include <nlohmann/json.hpp>

#include <optional>
#include <string>

#include "app/state.h"
#include "core/geom.h"

namespace noise::app {
namespace {

using nlohmann::json;

}  // namespace

// What the bar's phrase box holds, if one stands open: the verb, the field its words belong to, and
// the words themselves read back from the intent rather than from a buffer the view owns.
json verb_ask_state(AppState& state, CommandTable& table) {
    if (!state.ask_verb || selection_edit_phase(state) != SelectionEditPhase::actions) {
        return nullptr;
    }
    const VerbInfo* info = find_verb(*state.ask_verb);
    const ai::Recipe* active = info == nullptr ? nullptr : pressed_recipe(state, *info);
    if (active == nullptr || active->ask.empty()) {
        return nullptr;
    }
    const std::string field = active->ask[0];
    const json intent = table.invoke("ai.intent", json::object()).value;
    return json{{"verb", verb_name(info->id)},
                {"label", active->label},
                {"recipe", active->id},
                {"field", field},
                {"command", std::string("ai.") + verb_name(info->id)},
                {"value", intent.value("vars", json::object()).value(field, std::string())},
                // Where the frame last drew it, so a driver reads the box rather than the bar it hangs
                // from, and can tell a box that clipped its own buttons from one that did not.
                {"screen", json::array({state.verb_ask.x, state.verb_ask.y, state.verb_ask.w,
                                        state.verb_ask.h})}};
}

// The rows the mark is an instruction for: `scope` is the recipe's own region, read off `ai.verbs`
// rather than decided here, so a recipe that stopped reading the marked area drops out of the bar and
// out of the panel's claim at the same moment.
json verb_bar_rows(CommandTable& table, std::string* why) {
    json rows = json::array();
    const CommandResult view = table.invoke("ai.verbs", json::object());
    if (!view.ok) {
        if (why != nullptr) {
            *why = view.error;
        }
        return rows;
    }
    for (const json& verb : view.value["verbs"]) {
        if (verb["scope"].get<std::string>() == "selection") {
            rows.push_back(json{{"verb", verb["id"]},
                                {"label", verb["label"].is_null() ? verb["id"] : verb["label"]},
                                {"key", verb["key"]},
                                {"command", verb["command"]},
                                {"recipe", verb["recipe"]},
                                {"ready", verb["ready"]},
                                {"reason", verb["reason"]},
                                // The field a press would want, when it wants one: the bar offers a
                                // phrase box instead of running, so the words are typed where the eyes
                                // already are rather than in the panel across the window.
                                {"field", verb["ask"].empty() ? json(nullptr) : verb["ask"][0]}});
        }
    }
    return rows;
}

// The mark, in document pixels, when there is one. `selection_json` already answers with it, so the
// bar has no second notion of what "marked" means.
std::optional<Rect> verb_bar_mark(const AppState& state) {
    if (state.doc.empty()) {
        return std::nullopt;
    }
    const json selection = selection_json(state);
    if (!selection["active"].get<bool>()) {
        return std::nullopt;
    }
    const json& bounds = selection["bounds"];
    return Rect{bounds[0].get<int>(), bounds[1].get<int>(), bounds[2].get<int>(), bounds[3].get<int>()};
}

// A completed edit keeps its small return button near the result even if the user has cleared the
// marked area to inspect the edge. Re-arming a new edit without a mark hides the action bar again.
std::optional<Rect> verb_bar_anchor_mark(const AppState& state) {
    if (const std::optional<Rect> marked = verb_bar_mark(state); marked.has_value()) {
        return marked;
    }
    if (selection_edit_phase(state) == SelectionEditPhase::editing) {
        if (const Layer* layer = selection_edit_result_layer(state); layer != nullptr) {
            const Rect bounds = layer->mask->bounds();
            if (bounds.w > 0 && bounds.h > 0) {
                return bounds;
            }
        }
        // A fully hidden result has an empty mask. Its original request bounds still locate the
        // return button, so Un-cover never makes it impossible to start the next edit.
        if (state.ai.jobs != nullptr) {
            const std::optional<ai::JobRecord> job =
                state.ai.jobs->find(state.selection_edit.job);
            if (job.has_value() && job->landing.mask_bounds.w > 0 &&
                job->landing.mask_bounds.h > 0) {
                return job->landing.mask_bounds;
            }
        }
    }
    return std::nullopt;
}

void register_verb_bar_commands(AppState& state, CommandTable& table) {
    // The phrase box a marked-area verb needs, opened over the mark. Words themselves are the intent's
    // (`ai.intent`), the press is the verb's (`ai.<verb>`): this command only decides that a box is
    // standing open, and selects the recipe the box belongs to so the two doors cannot disagree.
    table.add("verb.ask", "Open ('<verb>') or shut ('close') the bar's phrase box", {}, {"show"},
              [&state, &table](const json& args) -> CommandResult {
        if (!args.contains("show") || !args.at("show").is_string()) {
            return fail("verb.ask",
                        "'show' must be a verb (" + verb_names() + ") or 'close'");
        }
        const std::string shown = args.at("show").get<std::string>();
        if (shown == "close") {
            state.ask_verb = std::nullopt;
            return command_ok(verb_ask_state(state, table));
        }
        if (selection_edit_phase(state) != SelectionEditPhase::actions) {
            return fail("verb.ask", "finish the current edit or choose New AI edit first");
        }
        const std::optional<VerbId> id = verb_from_name(shown);
        if (!id) {
            return fail("verb.ask", "'show' names no verb: '" + shown + "' (" + verb_names() +
                                        ", close)");
        }
        const VerbInfo* info = find_verb(*id);
        const ai::Recipe* active = pressed_recipe(state, *info);
        if (active == nullptr) {
            return fail("verb.ask", "the verb '" + shown + "' drives no recipe the table has");
        }
        if (active->ask.empty()) {
            return fail("verb.ask", "the verb '" + shown + "' asks for nothing; its button runs, "
                                    "so there is nothing to type");
        }
        const CommandResult intent = table.invoke("ai.intent", json{{"recipe", active->id}});
        if (!intent.ok) {
            return intent;
        }
        state.ask_verb = *id;
        return command_ok(verb_ask_state(state, table));
    });


    table.add("verb.new_edit", "Finish editing the result mask and mark the next AI edit", {}, {},
              [&state](const json&) -> CommandResult {
        const SelectionEditPhase phase = selection_edit_phase(state);
        if (phase != SelectionEditPhase::editing) {
            return fail("verb.new_edit", phase == SelectionEditPhase::generating
                                        ? "the AI edit is still generating"
                                        : "there is no result mask being edited");
        }
        selection_edit_new(state);
        return command_ok(json{{"mode", "actions"},
                               {"selection", selection_json(state)}});
    });

    table.add("verb.bar",
              "The verbs the marked area is an instruction for, and where the bar was last drawn",
              {}, {},
              [&state, &table](const json&) -> CommandResult {
        selection_edit_reconcile(state);
        const SelectionEditPhase mode = selection_edit_phase(state);
        const std::optional<Rect> mark = verb_bar_anchor_mark(state);
        std::string why;
        const json rows = mark && mode != SelectionEditPhase::editing
                              ? verb_bar_rows(table, &why) : json::array();
        if (!why.empty()) {
            return command_error("verb.bar: " + why);
        }
        // `shown` is the decision to draw; `screen` is where a frame actually did, and stays zero
        // outside one - a headless caller can read the first and must not trust the second.
        return command_ok(json{{"shown", mark.has_value() &&
                                             (mode == SelectionEditPhase::editing || !rows.empty())},
                               {"mode", selection_edit_phase_name(mode)},
                               {"job", state.selection_edit.job.empty() ? json(nullptr)
                                         : json(state.selection_edit.job)},
                               {"mark", mark == std::nullopt
                                            ? json(nullptr)
                                            : json::array({mark->x, mark->y, mark->w, mark->h})},
                               {"screen", json::array({state.verb_bar.x, state.verb_bar.y,
                                                       state.verb_bar.w, state.verb_bar.h})},
                               {"new_edit_screen", json::array({state.verb_new_edit.x,
                                                                  state.verb_new_edit.y,
                                                                  state.verb_new_edit.w,
                                                                  state.verb_new_edit.h})},
                               {"buttons", rows},
                               {"ask", verb_ask_state(state, table)}});
    });
}

}  // namespace noise::app
