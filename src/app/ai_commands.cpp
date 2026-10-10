// SPDX-License-Identifier: Apache-2.0
#include "app/command_groups.h"

#include "app/command_args.h"

#include "ai/compose.h"
#include "ai/region.h"
#include "core/log.h"
#include "io/flat.h"
#include "io/png.h"

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

namespace noise::app {

namespace {

// The recipe's word for "the compositor picks it from the mask", or an explicit radius.
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

// "every pixel may receive an answer", stated as a mask because a mask is how this codebase says where
// a write goes.
Mask whole_canvas(Extent size) {
    Mask all(size);
    all.fill(kMaskIn);
    return all;
}

json rect_json(Rect rect) {
    return json::array({rect.x, rect.y, rect.w, rect.h});
}

json region_json(const ai::RegionPlan& plan, const std::string& coverage_source) {
    return json{{"coverage", coverage_source},
                {"mask_bounds", rect_json(plan.mask_bounds)},
                {"crop", rect_json(plan.crop)},
                {"requested", json::array({plan.requested.w, plan.requested.h})},
                {"scale", plan.scale},
                {"snapped_width", plan.snapped_width},
                {"snapped_height", plan.snapped_height}};
}

json report_json(const ai::ComposeReport& report) {
    return json{{"composed", report.composed},
                {"refusal", report.refusal},
                {"feather_px", report.feather_px},
                {"colour_corrected", report.colour_corrected},
                {"area", rect_json(report.area)},
                {"result_width", report.result_dims.w},
                {"result_height", report.result_dims.h},
                {"answered_as_requested", report.answered_as_requested},
                {"pixels_changed", report.pixels_changed},
                {"pixels_clipped_by_extent", report.pixels_clipped_by_extent},
                {"coverage_pixels", report.coverage.covered()},
                {"coverage_area", report.coverage.area()},
                {"coverage_bounds", rect_json(report.coverage.bounds())}};
}

}  // namespace

void register_ai_commands(AppState& state, CommandTable& table) {
    // The result compositor, reached through the same table the UI will use: an answer image from
    // disk, the document's own selection, and the report of what landed where. Before the transport
    // exists this is the only way to see a composite on a real document - and the way to see it on
    // the wire as pixels, not as a claim.
    table.add("ai.compose", "Land an answer image on the document under the feathered mask",
              {}, {"image", "apply", "feather", "colour_match", "lock_band", "extent"},
              [&state](const json& args) -> CommandResult {
        CommandResult guard = require_document(state);
        if (!guard.ok) {
            return guard;
        }
        if (!args.contains("image") || !args.at("image").is_string()) {
            return fail("ai.compose", "expected string 'image' (the answer, in any flat format)");
        }
        const std::string path = args.at("image").get<std::string>();
        bool apply = false;
        if (!arg_bool(args, "apply", apply)) {
            return fail("ai.compose", "'apply' must be a boolean");
        }
        ai::CompositePolicy policy = ai::kCompositePolicy;
        if (!arg_feather(args, "feather", policy.feather_px)) {
            return fail("ai.compose",
                        "'feather' must be a non-negative integer or \"auto\" (the recipe's word)");
        }
        if (!arg_bool(args, "colour_match", policy.colour_match)) {
            return fail("ai.compose", "'colour_match' must be a boolean");
        }
        if (args.contains("lock_band") && !want_int(args, "lock_band", policy.lock_band_px)) {
            return fail("ai.compose", "'lock_band' must be an integer");
        }
        if (policy.lock_band_px < 0) {
            return fail("ai.compose",
                        "'lock_band' must not be negative (got " +
                            std::to_string(policy.lock_band_px) + ")");
        }

        // How far a write may exist. An edit is received into the layer's own alpha; a generation may
        // fill pixels the layer never had. That is a claim about the operation, so the caller states it
        // and the compositor only multiplies masks.
        std::string extent_name = "layer";
        if (args.contains("extent")) {
            if (!args.at("extent").is_string()) {
                return fail("ai.compose",
                            "'extent' must be 'layer' or 'canvas', not a " +
                                std::string(args.at("extent").type_name()));
            }
            extent_name = args.at("extent").get<std::string>();
        }
        if (extent_name != "layer" && extent_name != "canvas") {
            return fail("ai.compose",
                        "'extent' is 'layer' (an edit, received into the layer's own alpha) or "
                        "'canvas' (a generation, which may fill empty pixels); got '" +
                            extent_name + "'");
        }

        // No selection is the whole canvas, which is what a full-frame recipe means; a selection
        // that covers nothing is the other thing the document distinguishes, and it compiles to no
        // region at all.
        std::string coverage_source = "selection";
        Mask coverage = state.doc.selection().value_or(whole_canvas(state.doc.size()));
        if (!state.doc.selection().has_value()) {
            coverage_source = "whole document (no selection)";
        }
        const Mask extent = extent_name == "canvas"
                                ? whole_canvas(state.doc.size())
                                : alpha_coverage(state.active_layer().pixels);

        const std::optional<ai::RegionPlan> plan =
            ai::plan_region(state.doc.size(), coverage, ai::kRegionPolicy);
        if (!plan.has_value()) {
            return fail("ai.compose",
                        "the selection covers nothing, so there is no region to compose into");
        }

        std::string error;
        const Image answer = io::load_flat_image(path, error);
        if (answer.empty()) {
            return fail("ai.compose", error + " (" + path + ")");
        }

        ai::ComposeReport report;
        std::string preview;
        if (apply) {
            // Through the document's own door, so the undo step, the dirty rects and the revision
            // behave as they do for a stroke - but unfenced by the selection: the coverage this
            // composite writes is the feathered mask, wider than the selection by design, and fencing
            // it would delete the feather and leave the report describing an edit the document did not
            // perform.
            state.doc.paint(plan->crop, PaintFence::own_coverage, [&](Image& layer) {
                report = compose_result(layer, answer, *plan, coverage, extent, policy);
            });
        } else {
            Image candidate = state.active_layer().pixels;
            report = compose_result(candidate, answer, *plan, coverage, extent, policy);
            if (!report.composed) {
                return fail("ai.compose", report.refusal);
            }
            preview = captures_dir(state) + "/ai-compose-" + candidate.checksum_hex().substr(0, 12) +
                      ".png";
            if (!io::save_png(preview, candidate, error)) {
                return fail("ai.compose", error);
            }
        }
        if (!report.composed) {
            return fail("ai.compose", report.refusal);
        }

        log::info("composited " + path + " into " + std::to_string(report.area.w) + "x" +
                  std::to_string(report.area.h) + " under a " +
                  std::to_string(report.feather_px) + "px feather: " +
                  std::to_string(report.pixels_changed) + " pixels" +
                  (apply ? "" : " (preview)"));
        json result = json{{"applied", apply},
                           {"answer", path},
                           {"region", region_json(*plan, coverage_source)},
                           {"extent", extent_name},
                           {"policy", json{{"feather", policy.feather_px.has_value()
                                                             ? json(*policy.feather_px)
                                                             : json("auto")},
                                           {"colour_match", policy.colour_match},
                                           {"lock_band_px", policy.lock_band_px}}},
                           {"composite", report_json(report)}};
        if (!preview.empty()) {
            result["preview"] = preview;
        }
        if (apply) {
            result["document"] = document_json(state);
        }
        return command_ok(result);
    });
}

}  // namespace noise::app
