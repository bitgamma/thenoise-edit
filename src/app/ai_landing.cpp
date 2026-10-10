// SPDX-License-Identifier: Apache-2.0
#include "app/ai_landing.h"

#include "app/command_args.h"
#include "app/state.h"

#include "core/log.h"
#include "io/png.h"

#include <string>

namespace noise::app {

using nlohmann::json;

Mask whole_canvas(Extent size) {
    Mask all(size);
    all.fill(kMaskIn);
    return all;
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

std::optional<std::string> arg_extent_name(const json& args, const char* key,
                                           const std::string& fallback, std::string& error) {
    if (!args.contains(key)) {
        return fallback;
    }
    if (!args.at(key).is_string()) {
        error = std::string(key) + " must be 'layer' or 'canvas', not a " +
                std::string(args.at(key).type_name());
        return std::nullopt;
    }
    const std::string name = args.at(key).get<std::string>();
    if (name != "layer" && name != "canvas") {
        error = std::string(key) +
                " is 'layer' (an edit, received into the layer's own alpha) or 'canvas' (a "
                "generation, which may fill pixels the layer never had); got '" +
                name + "'";
        return std::nullopt;
    }
    return name;
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
                {"pixels_clipped_by_extent", report.pixels_clipped_by_extent},
                {"coverage_pixels", report.coverage.covered()},
                {"coverage_area", report.coverage.area()},
                {"coverage_bounds", json::array({report.coverage.bounds().x,
                                                 report.coverage.bounds().y,
                                                 report.coverage.bounds().w,
                                                 report.coverage.bounds().h})}};
}

json composite_policy_json(const ai::CompositePolicy& policy, ai::ExtentSource extent) {
    return json{{"feather",
                 policy.feather_px.has_value() ? json(*policy.feather_px) : json("auto")},
                {"colour_match", policy.colour_match},
                {"lock_band_px", policy.lock_band_px},
                {"extent", ai::extent_source_name(extent)}};
}

CommandResult land_answer(const char* command, AppState& state, const AnswerLanding& landing) {
    CommandResult guard = require_document(state);
    if (!guard.ok) {
        return guard;
    }
    // The extent is the caller's claim about the destination, not the compositor's guess: an edit is
    // received into the layer's own alpha, a generation may fill pixels the layer never had.
    const Mask extent = landing.extent == ai::ExtentSource::canvas
                            ? whole_canvas(state.doc.size())
                            : alpha_coverage(state.active_layer().pixels);

    ai::ComposeReport report;
    std::string preview;
    if (landing.apply) {
        // `own_coverage`, not `selection`: the feather is meant to reach past the selection it was
        // compiled from, and fencing it there deletes the feather and leaves the report describing an
        // edit the document did not perform.
        state.doc.paint(landing.plan.crop, PaintFence::own_coverage, [&](Image& layer) {
            report = compose_result(layer, landing.answer, landing.plan, landing.coverage, extent,
                                    landing.policy);
        });
    } else {
        Image candidate = state.active_layer().pixels;
        report = compose_result(candidate, landing.answer, landing.plan, landing.coverage, extent,
                                landing.policy);
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
              (report.answered_as_requested ? "" : ", answered at a different size than requested"));

    json result{{"applied", landing.apply},
                {"region", region_plan_json(landing.plan, landing.coverage_source)},
                {"policy", composite_policy_json(landing.policy, landing.extent)},
                {"composite", compose_report_json(report)}};
    if (!preview.empty()) {
        result["preview"] = preview;
    }
    if (landing.apply) {
        result["document"] = document_json(state);
    }
    return command_ok(result);
}

}  // namespace noise::app
