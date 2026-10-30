// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <nlohmann/json.hpp>

#include <map>
#include <optional>
#include <string>
#include <vector>

#include "ai/compose.h"
#include "ai/recipes.h"
#include "ai/queue.h"
#include "ai/region.h"
#include "core/command.h"
#include "core/geom.h"
#include "core/image.h"
#include "core/mask.h"

namespace noise {
struct AppState;
}

namespace noise::app {

// "every pixel may receive an answer", stated as a mask because a mask is how this codebase says where a
// write goes.
Mask whole_canvas(Extent size);

// What a document-scope operation claims: the whole canvas, or the frame an aspect picks. The recipe's
// `context` policy is what stops the crop growing past that frame - a generation has no surrounding
// pixels to take context from - and the coverage that lands is the frame's.
//
// `source` takes the words a report shows, because the document cannot say afterwards which aspect its
// pixels were asked for. Whether the recipe offers the aspect is `aspect_refusal`'s question.
Mask document_coverage(Extent canvas, const std::optional<AspectRatio>& aspect, std::string& source);

// Empty when the recipe offers `aspect`, or when none was chosen. The sentence names the recipe and what
// it does offer: a preset that silently did nothing is worse than a refusal.
std::string aspect_refusal(const ai::Recipe& recipe, const std::optional<AspectRatio>& aspect);

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
    ai::CompositePolicy policy{};
    bool apply = false;
    // File-name stem for the preview PNG, so `ai.compose` and `job.apply` write distinguishable files.
    std::string preview_stem = "ai-preview";
    // Where `coverage` came from, in the words the report shows ("selection", "whole document",
    // "job j0003"). The document cannot say afterwards, and a report that guessed would be a lie about
    // what was asked.
    std::string coverage_source = "selection";
    // The answer becomes a **new layer directly above this one** - the layer the region was flattened
    // from - and this is its name. Nothing is merged into a layer that already exists: the layer is the
    // preview, which is why there is no commit step and why hide, blend, mask, delete and undo are
    // ordinary layer operations on a result. Above the layer the answer was compiled from, rather than
    // above whatever is active when a slow job returns, keeps the result under content that was
    // deliberately not in the upload.
    int above_layer = 0;
    std::string layer_name = "answer";
};

// The door an answer goes through to become pixels. It takes the document's own flattened composite over
// `plan.crop`, up to and including `above_layer`, as the colour reference — measured there rather than
// passed in, because there is exactly one right answer and a caller that forgot it would land an answer
// corrected against an empty layer. Previewing writes nothing and returns a PNG; applying
// adds a layer carrying the generated crop and a persistent, editable coverage mask (after all
// inputs are validated). The preview follows the ordinary, flattened compose path. A landing is one
// undo step; changing the mask afterwards can reveal or hide more of the generated crop.
//
// Both `ai.compose` and `job.apply` call this and nothing else puts an answer on the document, which is
// what makes the pixel-lock proof in the probe apply to every path that lands an answer.
CommandResult land_answer(const char* command, noise::AppState& state,
                           const AnswerLanding& landing);

// Put one finished job's answer on the document: read the bytes back, take the coverage the job was
// queued with (never whatever is selected now), composite through the same door as `ai.compose`, and -
// with `record_outcome` - write the outcome onto the job so it is never landed twice. `command` names the door
// that asked, because a refusal has to say who was refused.
CommandResult land_job(AppState& state, const ai::JobRecord& record, const char* command,
                       const std::optional<int>& feather_override,
                       const std::optional<bool>& colour_match_override,
                       bool record_outcome);

// The beat that lands whatever finished while the app was busy: succeeded jobs that belong to the
// document open now and have not landed. Called by every door that reads the queue - the status line
// reads it every frame - so no single door owns landing and none can double-apply.
void pump_ai_landings(AppState& state);

// Shared argument shapes. `feather` is a recipe's word or a number, and both callers take it the same way.
bool arg_feather(const nlohmann::json& args, const char* key, std::optional<int>& out);
bool arg_recipe_vars(const nlohmann::json& args, const char* key,
                     std::map<std::string, std::string>& out, std::string& error);

// What a recipe would ask for, as far as that is known **without touching pixels**: the endpoint, the
// size the region compiler settles on for `aspect`, the model that would answer and the steps it would
// take. The status bar shows this every frame, so it is geometry and table lookups only - the upload
// itself is prepped and encoded by `ai.preview` and by a run, and by nothing else.
struct RequestQuote {
    std::string endpoint;
    Extent size{0, 0};
    // True when the region will be shrunk to the model's pixel ceiling: the answer comes back resampled
    // over the region rather than as the region. The price line says so before the press, because the
    // alternative is discovering it as softness afterwards.
    bool resampled = false;
    std::string model;
    std::optional<int> steps;
};

// Empty when the region cannot be planned (no document, nothing marked for a selection recipe).
std::optional<RequestQuote> quote_request(AppState& state, const ai::Recipe& recipe,
                                         const std::optional<AspectRatio>& aspect);

nlohmann::json region_plan_json(const ai::RegionPlan& plan, const std::string& coverage_source);
nlohmann::json compose_report_json(const ai::ComposeReport& report);
nlohmann::json composite_policy_json(const ai::CompositePolicy& policy);

}  // namespace noise::app
