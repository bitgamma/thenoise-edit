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
                {"colour_matched", report.colour_matched},
                {"area", rect_json(report.area)},
                {"result_width", report.result_dims.w},
                {"result_height", report.result_dims.h},
                {"answered_as_requested", report.answered_as_requested},
                {"pixels_changed", report.pixels_changed},
                {"pixels_clipped_by_alpha", report.pixels_clipped_by_alpha},
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
              {}, {"image", "apply", "feather", "colour_match", "lock_band"},
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

        // No selection is the whole canvas, which is what a full-frame recipe means; a selection
        // that covers nothing is the other thing the document distinguishes, and it compiles to no
        // region at all.
        std::string coverage_source = "selection";
        Mask coverage = state.doc.selection().value_or(Mask{state.doc.size()});
        if (!state.doc.selection().has_value()) {
            coverage.fill(kMaskIn);
            coverage_source = "whole document (no selection)";
        }

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
            // Through the document's own door, so the selection fence, the undo step, the dirty
            // rects and the revision all behave as they do for a stroke.
            state.doc.paint(plan->crop, [&](Image& layer) {
                report = compose_result(layer, answer, *plan, coverage, policy);
            });
        } else {
            Image candidate = state.doc.layer(static_cast<std::size_t>(state.doc.active())).pixels;
            report = compose_result(candidate, answer, *plan, coverage, policy);
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
