// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <nlohmann/json.hpp>

#include <map>
#include <optional>
#include <string>
#include <vector>

#include "ai/compose.h"
#include "ai/recipes.h"
#include "ai/region.h"
#include "core/command.h"
#include "core/image.h"
#include "core/mask.h"

namespace noise {
struct AppState;
}

namespace noise::app {

// "every pixel may receive an answer", stated as a mask because a mask is how this codebase says where a
// write goes.
Mask whole_canvas(Extent size);

// A canvas-sized PNG carrying `coverage` in its alpha channel: the form a mask takes when it has to
// outlive the session that made it, since a job's answer must composite under the coverage it was asked
// for and not under whatever is selected when it lands.
std::vector<std::uint8_t> coverage_png(const Mask& coverage);
// The alpha plane back, as coverage. An image with no alpha channel is all coverage: an answer that
// arrived opaque is meant to land whole.
Mask coverage_of_png(const Image& png);

// What an answer lands under. The plan and the coverage come with the answer, not from the document at
// landing time: a job queued against one selection and applied after three more edits still covers the
// pixels it was asked to.
struct AnswerLanding {
    Image answer;
    Mask coverage;
    ai::RegionPlan plan;
    ai::ExtentSource extent = ai::ExtentSource::layer_alpha;
    ai::CompositePolicy policy{};
    bool apply = false;
    // File-name stem for the preview PNG, so `ai.compose` and `job.apply` write distinguishable files.
    std::string preview_stem = "ai-preview";
    // Where `coverage` came from, in the words the report shows ("selection", "whole document",
    // "job j0003"). The document cannot say afterwards, and a report that guessed would be a lie about
    // what was asked.
    std::string coverage_source = "selection";
};

// The door an answer goes through to become pixels. Previewing writes nothing and returns a PNG; applying
// takes it through `Document::paint` with `own_coverage`, so it is one undo step that returns every byte
// and is not cut off at a selection the feather is meant to cross.
//
// Both `ai.compose` and `job.apply` call this and nothing else puts an answer on the document, which is
// what makes the pixel-lock proof in the probe apply to every path that lands an answer.
CommandResult land_answer(const char* command, noise::AppState& state,
                           const AnswerLanding& landing);

// Shared argument shapes. `feather` is a recipe's word or a number, and both callers take it the same way.
bool arg_feather(const nlohmann::json& args, const char* key, std::optional<int>& out);
// `key` absent yields `fallback`. A wrong word is an error that names both words and what each means.
std::optional<std::string> arg_extent_name(const nlohmann::json& args, const char* key,
                                           const std::string& fallback, std::string& error);
bool arg_recipe_vars(const nlohmann::json& args, const char* key,
                     std::map<std::string, std::string>& out, std::string& error);

nlohmann::json region_plan_json(const ai::RegionPlan& plan, const std::string& coverage_source);
nlohmann::json compose_report_json(const ai::ComposeReport& report);
nlohmann::json composite_policy_json(const ai::CompositePolicy& policy,
                                     ai::ExtentSource extent);

}  // namespace noise::app
