// SPDX-License-Identifier: Apache-2.0
#include "app/ai_landing.h"

#include "app/command_args.h"
#include "app/state.h"

#include "core/log.h"
#include "io/file.h"
#include "io/png.h"

#include <string>

#include "ai/registry.h"
#include "core/log.h"

namespace noise::app {

using nlohmann::json;

namespace {

// Nothing has ever been put on this layer: no colour anywhere, no coverage of its own. `New` hands a
// canvas one of these, and an answer arriving over it is the picture the person asked for rather than
// a second layer above an invisible one.
bool layer_is_void(const Layer& layer) {
    if (layer.mask.has_value()) {
        return false;
    }
    const std::uint8_t* bytes = layer.pixels.data();
    for (std::size_t i = 3; i < layer.pixels.bytes(); i += kChannels) {
        if (bytes[i] != 0) {
            return false;
        }
    }
    return true;
}

ai::RegionPlan plan_of_landing(const ai::JobRecord& record) {
    // The compositor maps by two rectangles and reads nothing else from a plan, so a reloaded job
    // carries exactly what it needs: the crop it covered and the size the answer was asked for.
    ai::RegionPlan plan;
    plan.crop = record.landing.crop;
    plan.requested = record.landing.requested;
    plan.mask_bounds = record.landing.mask_bounds;
    plan.resampled = record.landing.resampled;
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
                "; an answer lands under the mask it was asked for, so this document is not the one "
                "it came from";
        return Mask{};
    }
    return coverage_of_png(png);
}

}  // namespace

Mask whole_canvas(Extent size) {
    Mask all(size);
    all.fill(kMaskIn);
    return all;
}

Mask document_coverage(Extent canvas, const std::optional<AspectRatio>& aspect,
                      std::string& source) {
    if (!aspect.has_value()) {
        source = "whole document (recipe region)";
        return whole_canvas(canvas);
    }
    source = "document at " + aspect_name(*aspect) + " (recipe region)";
    Mask coverage(canvas);
    coverage.op(ai::frame_for_aspect(canvas, *aspect), MaskOp::replace);
    return coverage;
}

std::string aspect_refusal(const ai::Recipe& recipe, const std::optional<AspectRatio>& aspect) {
    if (!aspect.has_value()) {
        return {};
    }
    if (std::find(recipe.aspects.begin(), recipe.aspects.end(), *aspect) == recipe.aspects.end()) {
        return "recipe '" + recipe.id + "' " +
               (recipe.aspects.empty()
                    ? "offers no aspect: its region is what it is, and a choice would be ignored"
                    : "offers " + aspect_names(recipe.aspects)) +
               " - '" + aspect_name(*aspect) + "' is not one of them";
    }
    return {};
}

std::vector<std::uint8_t> coverage_png(const Mask& coverage) {
    if (!coverage.sized()) {
        return {};
    }
    std::vector<std::uint8_t> rgba(coverage.plane().size() * 4, 0);
    for (std::size_t i = 0; i < coverage.plane().size(); ++i) {
        rgba[i * 4 + 3] = coverage.plane()[i];
    }
    return io::encode_png(Image::from_rgba(coverage.size(), std::move(rgba)));
}

Mask coverage_of_png(const Image& png) {
    if (png.empty()) {
        return Mask{};
    }
    std::vector<std::uint8_t> plane(static_cast<std::size_t>(png.size().w) *
                                    static_cast<std::size_t>(png.size().h));
    for (int y = 0; y < png.size().h; ++y) {
        for (int x = 0; x < png.size().w; ++x) {
            plane[static_cast<std::size_t>(y) * static_cast<std::size_t>(png.size().w) +
                  static_cast<std::size_t>(x)] = png.at(x, y)[3];
        }
    }
    const std::optional<Mask> coverage = Mask::from_plane(png.size(), std::move(plane));
    return coverage.value_or(Mask{});
}

bool arg_feather(const json& args, const char* key, std::optional<int>& out) {
    if (!args.contains(key)) {
        return true;
    }
    const json& value = args.at(key);
    if (value.is_string()) {
        if (value.get<std::string>() == "auto") {
            out = std::nullopt;
            return true;
        }
        return false;
    }
    int px = 0;
    if (!want_int(args, key, px) || px < 0) {
        return false;
    }
    out = px;
    return true;
}

bool arg_recipe_vars(const json& args, const char* key, std::map<std::string, std::string>& out,
                     std::string& error) {
    if (!args.contains(key)) {
        return true;
    }
    if (!args.at(key).is_object()) {
        error = std::string(key) + " must be an object of recipe variables, not a " +
                std::string(args.at(key).type_name());
        return false;
    }
    for (auto it = args.at(key).begin(); it != args.at(key).end(); ++it) {
        if (!it.value().is_string()) {
            error = std::string(key) + "." + it.key() + " must be a string; a variable is substituted "
                                                      "into the prompt text";
            return false;
        }
        out[it.key()] = it.value().get<std::string>();
    }
    return true;
}

json region_plan_json(const ai::RegionPlan& plan, const std::string& coverage_source) {
    return json{{"coverage", coverage_source},
                {"mask_bounds", json::array({plan.mask_bounds.x, plan.mask_bounds.y,
                                             plan.mask_bounds.w, plan.mask_bounds.h})},
                {"crop", json::array({plan.crop.x, plan.crop.y, plan.crop.w, plan.crop.h})},
                {"requested", json::array({plan.requested.w, plan.requested.h})},
                {"scale", plan.scale},
                // Whether the answer will be a resample of the region rather than the region: true only
                // above the model's pixel ceiling, and the reason a Change can look like a no-op.
                {"resampled", plan.resampled},
                {"snapped_width", plan.snapped_width},
                {"snapped_height", plan.snapped_height}};
}

json compose_report_json(const ai::ComposeReport& report) {
    return json{{"composed", report.composed},
                {"refusal", report.refusal},
                {"feather_px", report.feather_px},
                {"colour_corrected", report.colour_corrected},
                {"area", json::array({report.area.x, report.area.y, report.area.w, report.area.h})},
                {"result_width", report.result_dims.w},
                {"result_height", report.result_dims.h},
                {"answered_as_requested", report.answered_as_requested},
                {"pixels_changed", report.pixels_changed},

                {"coverage_pixels", report.coverage.covered()},
                {"coverage_area", report.coverage.area()},
                {"coverage_bounds", json::array({report.coverage.bounds().x,
                                                 report.coverage.bounds().y,
                                                 report.coverage.bounds().w,
                                                 report.coverage.bounds().h})}};
}

json composite_policy_json(const ai::CompositePolicy& policy) {
    return json{{"feather",
                 policy.feather_px.has_value() ? json(*policy.feather_px) : json("auto")},
                {"colour_match", policy.colour_match},
                {"lock_band_px", policy.lock_band_px}};
}

CommandResult land_answer(const char* command, AppState& state, const AnswerLanding& landing) {
    CommandResult guard = require_document(state);
    if (!guard.ok) {
        return guard;
    }
    // The pixels the answer has to agree with colour-wise: the same flattened composite the request was
    // compiled from, taken now, so the drift corrected is the drift against what is on screen. Canvas
    // sized, because that is the space the compositor crops in; `flatten_over` hands back only the part
    // of the crop that met the canvas, region-relative, and the rest stays transparent - there is no
    // document colour out there, and the drift estimate skips those pixels rather than invent one.
    const Rect canvas_rect{0, 0, state.doc.size().w, state.doc.size().h};
    const Rect overlap = intersect(landing.plan.crop, canvas_rect);
    const Image band = state.doc.flatten_over(
        overlap, static_cast<std::size_t>(std::max(landing.above_layer, 0)));
    Image reference = Image::filled(state.doc.size(), 0, 0, 0, 0);
    for (int y = 0; y < overlap.h; ++y) {
        for (int x = 0; x < overlap.w; ++x) {
            std::memcpy(reference.at(overlap.x + x, overlap.y + y), band.at(x, y), kChannels);
        }
    }

    ai::ComposeReport report;
    std::string preview;
    int landed_layer = -1;
    if (landing.apply) {
        // One layer per answer, made here - after every argument has been believed - so a refusal
        // cannot leave an empty layer sitting in the stack, and one `paint` step means one `Ctrl+Z`.
        if (!state.doc.select_layer(landing.above_layer)) {
            return fail(command,
                        "the answer was compiled from layer " +
                            std::to_string(landing.above_layer) +
                            ", which is no longer in the document, so there is nothing to land it above");
        }
        // A blank canvas is the one document that holds nothing to land above: `New` made the empty
        // layer, nobody drew on it, and the answer is what the canvas was asked for. Composing into it
        // gives byte-identical pixels to a fresh layer above it (there was nothing underneath to agree
        // with, and coverage holds every byte outside the answer), and `Ctrl+Z` returns the blank canvas
        // instead of peeling a layer off an empty one.
        const bool into_blank_canvas = landing.above_layer == 0 &&
                                       state.doc.layers().size() == 1 &&
                                       layer_is_void(state.doc.layer(0));
        if (into_blank_canvas) {
            state.doc.paint(landing.plan.crop, PaintFence::own_coverage, [&](Image& layer) {
                report = compose_result(layer, reference, landing.answer, landing.plan,
                                        landing.coverage, landing.policy);
            });
            landed_layer = report.composed ? 0 : -1;
        } else {
            // Elsewhere the creation and the write are one undo step, so the answer comes back as the
            // one edit it was asked to be: layer and pixels together. A refusal adds nothing - there is
            // no empty layer left named after a job that produced no answer.
            landed_layer = state.doc.add_layer_written(
                landing.layer_name, landing.plan.crop, [&](Image& layer) {
                    report = compose_result(layer, reference, landing.answer, landing.plan,
                                            landing.coverage, landing.policy);
                    return report.composed;
                });
            if (landed_layer < 0 && report.composed) {
                report.refusal = "no layer could be added to receive the answer";
            }
        }
        if (landed_layer < 0) {
            return fail(command, report.refusal);
        }
    } else {
        // The preview is what the new layer would hold - an empty destination receiving the answer under
        // its coverage - not a merge into whatever is active now.
        Image candidate = Image::filled(state.doc.size(), 0, 0, 0, 0);
        report = compose_result(candidate, reference, landing.answer, landing.plan,
                                landing.coverage, landing.policy);
        if (!report.composed) {
            return fail(command, report.refusal);
        }
        preview = captures_dir(state) + "/" + landing.preview_stem + "-" +
                  candidate.checksum_hex().substr(0, 12) + ".png";
        std::string error;
        if (!io::save_png(preview, candidate, error)) {
            return fail(command, error);
        }
    }
    if (!report.composed) {
        return fail(command, report.refusal);
    }

    log::info(std::string(command) + ": " + std::to_string(report.area.w) + "x" +
              std::to_string(report.area.h) + " under a " + std::to_string(report.feather_px) +
              "px feather: " + std::to_string(report.pixels_changed) + " pixels" +
              (landing.apply ? "" : " (preview)") +
              // Two different reasons the picture is not the one that was asked for, said separately: one is
              // the engine snapping, the other is this app rescaling the region before asking - which costs
              // detail at both ends and is the one a person can avoid by marking a smaller area.
              (landing.plan.resampled ? ", rescaled to " + std::to_string(landing.plan.requested.w) + "x" +
                                            std::to_string(landing.plan.requested.h) +
                                            " for the model's pixel ceiling"
                                      : "") +
              (report.answered_as_requested ? "" : ", answered at a different size than requested"));

    json result{{"applied", landing.apply},
                {"region", region_plan_json(landing.plan, landing.coverage_source)},
                {"policy", composite_policy_json(landing.policy)},
                {"composite", compose_report_json(report)}};
    if (landed_layer >= 0) {
        result["layer"] = landed_layer;
        result["layer_name"] = landing.layer_name;
    }
    if (!preview.empty()) {
        result["preview"] = preview;
    }
    if (landing.apply) {
        result["document"] = document_json(state);
    }
    return command_ok(result);
}
CommandResult land_job(AppState& state, const ai::JobRecord& record, const char* command,
                       const std::optional<int>& feather_override,
                       const std::optional<bool>& colour_match_override,
                       bool record_outcome) {
    if (state.ai.jobs == nullptr) {
        return fail(command, "there is no queue to read job '" + record.id + "' from");
    }
    std::string error;
    std::vector<std::uint8_t> bytes;
    if (!state.ai.jobs->result_png(record.id, bytes, error)) {
        if (record_outcome) {
            state.ai.jobs->set_land_refusal(record.id, error);
        }
        return fail(command, error);
    }
    const Image answer = io::decode_png(bytes, error);
    if (answer.empty()) {
        const std::string why = error + " (" + record.result_png + ")";
        if (record_outcome) {
            state.ai.jobs->set_land_refusal(record.id, why);
        }
        return fail(command, why);
    }
    const Mask coverage = mask_for_landing(record, state.doc.size(), error);
    if (!coverage.sized()) {
        if (record_outcome) {
            state.ai.jobs->set_land_refusal(record.id, error);
        }
        return fail(command, error);
    }

    ai::CompositePolicy policy;
    policy.feather_px = record.landing.feather_px;
    policy.colour_match = record.landing.colour_match;
    policy.lock_band_px = record.landing.lock_band_px;
    if (feather_override.has_value()) {
        policy.feather_px = feather_override;
    }
    if (colour_match_override.has_value()) {
        policy.colour_match = *colour_match_override;
    }

    // Named for what the operation is called, not for the key that identifies it: this string is the row
    // a person reads in the layer stack afterwards. An id edited away since the job ran lands under the
    // id rather than under a guess.
    const ai::Recipe* recipe = state.ai.recipes.find(record.recipe_id);
    const std::string layer_name =
        std::string(recipe != nullptr ? recipe->label : record.recipe_id) + " " + record.id;
    const AnswerLanding landing{
        .answer = answer,
        .coverage = coverage,
        .plan = plan_of_landing(record),
        .policy = policy,
        .apply = true,
        .preview_stem = "ai-job-" + record.id,
        .coverage_source = "job " + record.id,
        .above_layer = record.landing.target_layer,
        .layer_name = layer_name};
    CommandResult result = land_answer(command, state, landing);
    if (!result.ok) {
        if (record_outcome) {
            state.ai.jobs->set_land_refusal(record.id, result.error);
        }
        return result;
    }
    if (record_outcome) {
        state.ai.jobs->set_landed(record.id, layer_name, result.value);
    }
    result.value["layer_name"] = layer_name;
    return result;
}

void pump_ai_landings(AppState& state) {
    if (state.ai.jobs == nullptr || state.doc.empty()) {
        return;
    }
    const std::vector<ai::JobRecord> pending = state.ai.jobs->pending_landings(state.document_epoch);
    for (const ai::JobRecord& record : pending) {
        const CommandResult landed = land_job(state, record, "answer", std::nullopt, std::nullopt,
                                             /*record_outcome=*/true);
        if (landed.ok) {
            log::info("answer from " + record.id + " landed on layer '" +
                      landed.value.value("layer_name", std::string()) + "'");
        } else {
            log::error("answer from " + record.id + " did not land: " + landed.error);
        }
    }
}
}  // namespace noise::app
