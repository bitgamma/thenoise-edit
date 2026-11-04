// SPDX-License-Identifier: Apache-2.0
#include "app/command_groups.h"

#include "app/command_args.h"
#include "app/new_document.h"
#include "app/selection_edit.h"

#include "app/commands.h"
#include "core/base64.h"
#include "ai/provider.h"
#include "ai/region.h"
#include "ai/registry.h"
#include "core/log.h"
#include "io/file.h"
#include "io/config.h"
#include "io/flat.h"
#include "io/png.h"
#include "io/project.h"
#ifdef __EMSCRIPTEN__
#include "platform/web.h"
#endif

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstddef>
#include <cstdint>
#include <span>


namespace noise::app {

namespace {

// One document ends and another begins. A job may land only in the document that asked for it, so every
// door that replaces the document says so here - a door that forgot would let an answer queued against
// the last file land in this one.
void note_new_document(AppState& state) {
    state.edit_plane = LayerPlane::pixels;
    state.deferred_upscale.reset();
    ++state.document_epoch;
    // The door is the way in, and a document in hand closes it - whoever made the document: the
    // picker, the recent list, a launch argument, a driver.
    state.front_door = FrontDoor::closed;
}

}  // namespace

void register_document_commands(AppState& state, CommandTable& table) {
    table.add("doc.get", "Report the document (path, size, checksum, revision, history)", {}, {},
              [&state](const json&) -> CommandResult {
        CommandResult guard = require_document(state);
        if (!guard.ok) {
            return guard;
        }
        return command_ok(document_json(state));
    });

    table.add("doc.new", "Create an empty document", {}, {"width", "height", "fill"}, [&state](const json& args) -> CommandResult {
        int width = 0;
        int height = 0;
        if (!want_int(args, "width", width) || !want_int(args, "height", height)) {
            return fail("doc.new", "expected integer 'width' and 'height'");
        }
        if (width <= 0 || height <= 0) {
            return fail("doc.new", "width and height must be positive");
        }
        std::uint8_t fill[4] = {0, 0, 0, 0};
        if (!arg_rgba(args, "fill", fill)) {
            return fail("doc.new", "'fill' must be [r,g,b,a] with each channel 0..255");
        }
        Layer base;
        base.name = default_layer_name(0);
        base.pixels = Image::filled(Extent{width, height}, fill[0], fill[1], fill[2], fill[3]);
        std::vector<Layer> layers;
        layers.push_back(std::move(base));
        state.doc.reset(std::move(layers), std::nullopt, std::string());
        note_new_document(state);
        state.carried.clear();  // a new document is not tied to the file that was open
        request_fit(state);
        log::info("created image " + std::to_string(width) + "x" + std::to_string(height));
        return command_ok(document_json(state));
    });

    table.add("doc.open", "Open a .noise project, or a flat PNG, JPEG or WebP", {}, {"path"},
              [&state](const json& args) -> CommandResult {
        if (!args.contains("path") || !args.at("path").is_string()) {
            return fail("doc.open", "expected string 'path'");
        }
        const std::string path = args.at("path").get<std::string>();
        std::string error;
        std::vector<Layer> loaded;
        std::optional<Mask> opened_selection;
        // The bytes decide, not the name. `.noise`, `.ora` and a dozen other formats are all ZIP
        // containers, and routing an OpenRaster file by its extension sent it to the PNG decoder,
        // which reported "Not a PNG file" about a file that is a perfectly good archive of
        // something we do not read. A container that is not ours is refused with its own mimetype.
        std::string sniff_error;
        if (io::zip_container_at(path, sniff_error)) {
            io::Project project;
            if (!io::load_project(path, project, error)) {
                return fail("doc.open", error);
            }
            std::vector<DocumentVersion> versions = std::move(project.archived_versions);
            versions.push_back(DocumentVersion{project.active_version_id,
                                                std::move(project.active_version_name),
                                                std::move(project.layers),
                                                std::move(project.selection),
                                                project.active_layer,
                                                project.active_version_parent_id});
            if (!state.doc.load_versions(std::move(versions), project.active_version_id, path)) {
                return fail("doc.open", "invalid document versions in " + path);
            }
            state.carried = std::move(project.carried);
        } else if (!sniff_error.empty()) {
            return fail("doc.open", sniff_error);
        } else {
            Image flat = io::load_flat_image(path, error);
            if (flat.empty()) {
                return fail("doc.open", error + " (" + path + ")");
            }
            Layer base;
            base.name = default_layer_name(0);
            base.pixels = std::move(flat);
            loaded.push_back(std::move(base));
            state.carried.clear();
        }
        if (!loaded.empty()) {
            state.doc.reset(std::move(loaded), std::move(opened_selection), path);
        }
        note_new_document(state);
        io::remember_recent_file(path);
        request_fit(state);
        log::info("loaded " + path + " (" + std::to_string(state.doc.layers().size()) +
                  " layers)");
        return command_ok(document_json(state));
    });

    table.add("doc.save", "Save the project (defaults to its current path)", {"Ctrl+S"}, {"path"},
#ifdef __EMSCRIPTEN__
              [&state, &table](const json& args) -> CommandResult {
#else
              [&state](const json& args) -> CommandResult {
#endif
        CommandResult guard = require_document(state);
        if (!guard.ok) {
            return guard;
        }
        std::string path = state.doc.path();
        if (args.contains("path")) {
            if (!args.at("path").is_string()) {
                return fail("doc.save", "'path' must be a string");
            }
            path = args.at("path").get<std::string>();
        }
#ifdef __EMSCRIPTEN__
        if (!args.contains("path") &&
            (path.empty() || io::extension_of(path) != io::kProjectExtension)) {
            return table.invoke("file.pick", json{{"for", "project"}});
        }
#endif
        if (path.empty()) {
            return fail("doc.save", "document has no path and none was given");
        }
        // A save dialog whose only filter is the project format hands back a name, and whether the
        // extension is on it is the dialog's mood rather than the person's choice. A *different*
        // extension is still refused: that is another format, and another command.
        if (io::extension_of(path).empty()) {
            path += ".";
            path += io::kProjectExtension;
        }
        if (io::extension_of(path) != io::kProjectExtension) {
            return fail("doc.save",
                        "doc.save writes ." + std::string(io::kProjectExtension) +
                            "; use doc.export for a flat image (got '" + path + "')");
        }
        std::string error;
        const std::vector<DocumentVersionView> versions = state.doc.versions();
        if (!io::save_project(path, state.doc.layers(), state.doc.composite(),
                              state.doc.selection(), versions, state.carried, error)) {
            return fail("doc.save", error);
        }
#ifdef __EMSCRIPTEN__
        if (!platform::download_file(path, error)) {
            return fail("doc.save", error);
        }
#endif
        state.doc.set_path(path);
        state.doc.mark_saved();
        io::remember_recent_file(path);
        log::info("saved " + path);
        return command_ok(document_json(state));
    });

    // A flat image is an export, not a save: it does not take the document's path or clear its
    // dirty flag, because the project on disk is still the thing the document is.
    table.add("doc.export", "Write a flat PNG, JPEG or WebP of the document", {},
              {"path", "quality"},
              [&state](const json& args) -> CommandResult {
        CommandResult guard = require_document(state);
        if (!guard.ok) {
            return guard;
        }
        if (!args.contains("path") || !args.at("path").is_string()) {
            return fail("doc.export", "expected string 'path'");
        }
        const std::string path = args.at("path").get<std::string>();
        const std::string extension = io::extension_of(path);
        if (!io::flat_format_exists(extension)) {
            return fail("doc.export", "'" + extension + "' is not a format this app writes; it writes " +
                                          io::flat_export_extensions());
        }
        int quality = io::kDefaultExportQuality;
        if (args.contains("quality")) {
            if (!want_int(args, "quality", quality)) {
                return fail("doc.export", "'quality' must be an integer");
            }
            if (quality < io::kMinQuality || quality > io::kMaxQuality) {
                return fail("doc.export",
                            "'quality' must be between " + std::to_string(io::kMinQuality) +
                                " and " + std::to_string(io::kMaxQuality) +
                                " (got " + std::to_string(quality) + ")");
            }
            if (!io::flat_format_has_quality(extension)) {
                return fail("doc.export",
                            "'quality' means nothing for a '" + extension + "' export: it is lossless");
            }
        }
        const Image& flat = state.doc.composite();
        std::string error;
        if (!io::save_flat_image(path, flat, quality, error)) {
            return fail("doc.export", error);
        }
#ifdef __EMSCRIPTEN__
        if (!platform::download_file(path, error)) {
            return fail("doc.export", error);
        }
#endif
        // A format with no alpha channel did not fail to write the transparency - it discarded it -
        // and the caller is told rather than left to notice later. `screenshot` and `image.get` keep
        // writing PNG, which loses nothing.
        bool transparency = false;
        const std::span<const std::uint8_t> pixels = flat.pixels();
        for (std::size_t i = 3; i < pixels.size(); i += 4) {
            if (pixels[i] != 255) {
                transparency = true;
                break;
            }
        }
        const bool discarded_alpha = transparency && !io::flat_format_has_alpha(extension);
        log::info("exported " + path + (discarded_alpha ? " (alpha flattened over white)" : ""));
        json result = document_json(state);
        result["exported"] = path;
        result["format"] = extension;
        result["discarded_alpha"] = discarded_alpha;
        if (io::flat_format_has_quality(extension)) {
            result["quality"] = io::clamp_quality(quality);
        }
        return command_ok(result);
    });

    table.add("doc.crop", "Crop canvas to a rectangle or the marked area's bounds", {},
              {"rect"}, [&state](const json& args) -> CommandResult {
        CommandResult guard = require_document(state);
        if (!guard.ok) {
            return guard;
        }
        Rect region{};
        if (args.contains("rect")) {
            if (!arg_rect(args, "rect", region)) {
                return fail("doc.crop", "'rect' must be [x, y, width, height] of integers");
            }
        } else if (state.doc.selection()) {
            region = state.doc.selection()->bounds();
        }
        if (region.empty() || !region.inside(state.doc.size())) {
            return fail("doc.crop", "crop must be a non-empty rectangle inside the canvas");
        }
        const std::uint64_t geometry_before = state.doc.geometry_revision();
        if (!state.doc.crop_canvas(region)) {
            return fail("doc.crop", "the crop could not be applied");
        }
        if (geometry_before != state.doc.geometry_revision()) {
            ++state.document_epoch;
            request_fit(state);
        }
        return command_ok(document_json(state));
    });

    table.add("doc.resize", "Resize the canvas without resampling pixels", {},
              {"width", "height", "offset_x", "offset_y", "mark_added"},
              [&state](const json& args) -> CommandResult {
        CommandResult guard = require_document(state);
        if (!guard.ok) {
            return guard;
        }
        int width = 0;
        int height = 0;
        int dx = 0;
        int dy = 0;
        bool mark = true;
        if (!want_int(args, "width", width) || !want_int(args, "height", height) ||
            !arg_int(args, "offset_x", dx) || !arg_int(args, "offset_y", dy) ||
            !arg_bool(args, "mark_added", mark)) {
            return fail("doc.resize", "expected width, height, offset_x, offset_y integers "
                                      "and optional boolean mark_added");
        }
        if (width <= 0 || height <= 0 ||
            static_cast<std::int64_t>(width) * height > kMaxCanvasPixels) {
            return fail("doc.resize", "canvas dimensions must be positive and at most " +
                                      std::to_string(kMaxCanvasPixels) + " pixels");
        }
        const Extent before = state.doc.size();
        if (!state.doc.resize_canvas({width, height}, dx, dy, mark)) {
            return fail("doc.resize", "offset must place the old canvas within reach of the new one");
        }
        if (before != state.doc.size() || dx != 0 || dy != 0) {
            ++state.document_epoch;
            request_fit(state);
        }
        json result = document_json(state);
        result["offset"] = json::array({dx, dy});
        result["marked_added"] = mark;
        return command_ok(result);
    });

    table.add("doc.rotate", "Rotate the entire canvas 90 degrees", {}, {"direction"},
              [&state](const json& args) -> CommandResult {
        CommandResult guard = require_document(state);
        if (!guard.ok) {
            return guard;
        }
        if (!args.contains("direction") || !args.at("direction").is_string()) {
            return fail("doc.rotate", "'direction' must be 'cw' or 'ccw'");
        }
        const std::string direction = args.at("direction").get<std::string>();
        if (direction != "cw" && direction != "ccw") {
            return fail("doc.rotate", "'direction' must be 'cw' or 'ccw'");
        }
        if (!state.doc.rotate_canvas(direction == "cw")) {
            return fail("doc.rotate", "could not rotate the canvas");
        }
        ++state.document_epoch;
        request_fit(state);
        return command_ok(document_json(state));
    });

    table.add("doc.resample", "Resize the composite or the whole stack using Lanczos3", {},
              {"width", "height", "whole_stack"}, [&state](const json& args) -> CommandResult {
        CommandResult guard = require_document(state);
        if (!guard.ok) {
            return guard;
        }
        int width = 0;
        int height = 0;
        if (!want_int(args, "width", width) || !want_int(args, "height", height)) {
            return fail("doc.resample", "expected integer width and height");
        }
        if (width <= 0 || height <= 0 ||
            static_cast<std::int64_t>(width) * height > kMaxCanvasPixels) {
            return fail("doc.resample", "image dimensions must be positive and at most " +
                                         std::to_string(kMaxCanvasPixels) + " pixels");
        }
        const Extent target{width, height};
        const Extent previous = state.doc.size();
        bool whole_stack = false;
        if (!arg_bool(args, "whole_stack", whole_stack)) {
            return fail("doc.resample", "'whole_stack' must be a boolean");
        }
        if (previous == target) {
            return command_ok(document_json(state));
        }
        if (!(whole_stack ? state.doc.resample_image(target)
                          : state.doc.resample_composite(target))) {
            return fail("doc.resample", "could not resize the image");
        }
        if (previous != target) {
            ++state.document_epoch;
            request_fit(state);
        }
        return command_ok(document_json(state));
    });

    table.add("doc.version.list", "List document versions and their native sizes", {}, {},
              [&state](const json&) -> CommandResult {
        CommandResult guard = require_document(state);
        if (!guard.ok) {
            return guard;
        }
        json versions = json::array();
        for (const DocumentVersionView& version : state.doc.versions()) {
            const Extent size = version.layers.front().pixels.size();
            versions.push_back(json{{"id", version.id}, {"name", version.name},
                                    {"width", size.w}, {"height", size.h},
                                    {"layers", version.layers.size()},
                                    {"active", version.current},
                                    {"parent", version.parent_id}});
        }
        return command_ok(json{{"versions", versions},
                               {"active", state.doc.current_version()}});
    });

    table.add("doc.version.select", "Switch to another resolution version", {}, {"id"},
              [&state](const json& args) -> CommandResult {
        if (!args.contains("id") || !args.at("id").is_number_unsigned()) {
            return fail("doc.version.select", "expected unsigned version 'id'");
        }
        const std::uint64_t id = args.at("id").get<std::uint64_t>();
        if (!state.doc.switch_version(id)) {
            return fail("doc.version.select", "no version with id " + std::to_string(id));
        }
        state.deferred_upscale.reset();
        state.edit_plane = LayerPlane::pixels;
        ++state.document_epoch;
        request_fit(state);
        return command_ok(document_json(state));
    });

    table.add("image.upscale.defer", "Use a larger resolution on the next AI image edit", {},
              {"enabled", "width", "height"},
              [&state](const json& args) -> CommandResult {
        CommandResult guard = require_document(state);
        if (!guard.ok) {
            return guard;
        }
        bool enabled = true;
        if (!arg_bool(args, "enabled", enabled)) {
            return fail("image.upscale.defer", "'enabled' must be a boolean");
        }
        if (!enabled) {
            state.deferred_upscale.reset();
            return command_ok(json{{"armed", false}});
        }
        if (state.image_size_form.whole_stack) {
            return fail("image.upscale.defer", "Entire layer stack is only supported by Lanczos3; "
                       "disable it before requesting an AI upscale");
        }
        int width = 0;
        int height = 0;
        if (!want_int(args, "width", width) || !want_int(args, "height", height)) {
            return fail("image.upscale.defer", "expected integer width and height");
        }
        const Extent source = state.doc.size();
        const std::int64_t target_area = static_cast<std::int64_t>(width) * height;
        const std::int64_t source_area = static_cast<std::int64_t>(source.w) * source.h;
        const int multiple = state.ai.roles.size_policy.multiple;
        if (width <= 0 || height <= 0 || target_area <= source_area ||
            target_area > kMaxCanvasPixels) {
            return fail("image.upscale.defer", "target must be larger than the current image "
                         "and fit within the document pixel limit");
        }
        if (width > ai::kMaxGeneratedDimensionPx ||
            height > ai::kMaxGeneratedDimensionPx || multiple <= 0) {
            return fail("image.upscale.defer", "target exceeds the edit model's maximum "
                         "output dimensions");
        }
        const Extent request_size = ai::edit_output_size(Extent{width, height}, multiple);
        if (request_size.w > ai::kMaxGeneratedDimensionPx ||
            request_size.h > ai::kMaxGeneratedDimensionPx) {
            return fail("image.upscale.defer", "the output grid would exceed the edit model's "
                         "maximum dimensions");
        }
        if (const ai::Recipe* edit = state.ai.recipes.find("inpaint-replace")) {
            const int budget = state.ai.region_policy(*edit).max_area_px;
            if (budget > 0 && static_cast<std::int64_t>(request_size.w) * request_size.h > budget) {
                return fail("image.upscale.defer", "edit model allows " +
                    std::to_string(budget) + " output pixels, but the " +
                    std::to_string(request_size.w) + "x" + std::to_string(request_size.h) +
                    " model request after grid alignment needs more");
            }
        }
        state.deferred_upscale = DeferredUpscale{source, Extent{width, height},
                                                  state.document_epoch};
        return command_ok(json{{"armed", true},
                               {"source", json::array({source.w, source.h})},
                               {"target", json::array({width, height})},
                               {"model_request", json::array({request_size.w, request_size.h})}});
    });

    table.add("image.upscale.status", "Report the next-AI-edit resolution", {}, {},
              [&state](const json&) -> CommandResult {
        const std::optional<DeferredUpscale>& pending = state.deferred_upscale;
        const bool valid = pending && pending->source == state.doc.size() &&
                           pending->document_epoch == state.document_epoch;
        const Extent request_size = valid
            ? ai::edit_output_size(pending->target, state.ai.roles.size_policy.multiple)
            : Extent{};
        return command_ok(json{{"armed", valid},
                               {"target", valid ? json::array({pending->target.w, pending->target.h})
                                                : json(nullptr)},
                               {"model_request", valid
                                   ? json::array({request_size.w, request_size.h})
                                   : json(nullptr)}});
    });

    table.add("image.upscale", "Upscale the visible image to the requested size", {},
              {"model", "width", "height"},
              [&state, &table](const json& args) -> CommandResult {
        CommandResult guard = require_document(state);
        if (!guard.ok) {
            return guard;
        }
        if (state.image_size_form.whole_stack) {
            return fail("image.upscale", "Entire layer stack is only supported by Lanczos3; "
                        "disable it before requesting an AI upscale");
        }
        const ai::Recipe* recipe = state.ai.recipes.find("upscale");
        Readiness ready = assess(state, recipe);
        std::string model = ready.model;
        if (!arg_text(args, "model", model)) {
            return fail("image.upscale", "'model' must be a string");
        }
        if (state.ai.registry == nullptr || !state.ai.connected || recipe == nullptr) {
            return fail("image.upscale", ready.reason);
        }
        const std::vector<std::string> offered =
            state.ai.registry->candidates("upscale", /*include_not_downloaded=*/true);
        if (std::find(offered.begin(), offered.end(), model) == offered.end()) {
            return fail("image.upscale", "model '" + model + "' is not an available upscaler");
        }
        ready.ready = true;
        const int native = ai::native_upscale_factor(model);
        if (native == 0) {
            return fail("image.upscale", "model '" + model + "' does not expose a native scale");
        }
        const Extent source = state.doc.size();
        int width = source.w * native;
        int height = source.h * native;
        if (args.contains("width") && !want_int(args, "width", width)) {
            return fail("image.upscale", "'width' must be an integer");
        }
        if (args.contains("height") && !want_int(args, "height", height)) {
            return fail("image.upscale", "'height' must be an integer");
        }
        const std::int64_t target_area = static_cast<std::int64_t>(width) * height;
        const std::int64_t cross = std::abs(static_cast<std::int64_t>(width) * source.h -
                                            static_cast<std::int64_t>(height) * source.w);
        if (width <= source.w || height <= source.h ||
            width > static_cast<std::int64_t>(source.w) * native ||
            height > static_cast<std::int64_t>(source.h) * native ||
            target_area > kMaxCanvasPixels || cross > std::max(source.w, source.h)) {
            return fail("image.upscale", "requested size must preserve aspect ratio, exceed the "
                                         "source size, fit the model's native " +
                                         std::to_string(native) + "x output, and stay within "
                                         "the 64-Mi pixel document limit");
        }
        return table.invoke("ai.run", json{{"recipe", "upscale"},
                                           {"include_references", false},
                                           {"model", model},
                                           {"output_width", width},
                                           {"output_height", height}});
    });

    table.add("doc.outpaint.prepare", "Enlarge the canvas and open Fill for its added area", {},
              {"width", "height", "offset_x", "offset_y", "prompt"},
              [&state, &table](const json& args) -> CommandResult {
        CommandResult guard = require_document(state);
        if (!guard.ok) {
            return guard;
        }
        int width = 0;
        int height = 0;
        int dx = 0;
        int dy = 0;
        std::string prompt = "extended the background";
        if (!want_int(args, "width", width) || !want_int(args, "height", height) ||
            !arg_int(args, "offset_x", dx) || !arg_int(args, "offset_y", dy) ||
            !arg_text(args, "prompt", prompt)) {
            return fail("doc.outpaint.prepare", "expected canvas dimensions, integer offsets and an optional prompt");
        }
        if (width <= 0 || height <= 0 ||
            static_cast<std::int64_t>(width) * height > kMaxCanvasPixels) {
            return fail("doc.outpaint.prepare", "canvas dimensions are invalid");
        }
        if (static_cast<std::int64_t>(dx) <= -static_cast<std::int64_t>(state.doc.width()) ||
            static_cast<std::int64_t>(dy) <= -static_cast<std::int64_t>(state.doc.height()) ||
            dx >= width || dy >= height) {
            return fail("doc.outpaint.prepare", "the resized canvas must overlap the original image");
        }
        const Rect new_canvas{0, 0, width, height};
        const Rect old_area{dx, dy, state.doc.width(), state.doc.height()};
        const Rect overlap = intersect(new_canvas, old_area);
        if (static_cast<std::int64_t>(overlap.w) * overlap.h ==
            static_cast<std::int64_t>(width) * height) {
            return fail("doc.outpaint.prepare", "enlarge the canvas to expose an area to fill");
        }
        if (selection_edit_phase(state) == SelectionEditPhase::generating) {
            return fail("doc.outpaint.prepare", "finish or cancel the current AI edit first");
        }
        if (state.ai.recipes.find("region-fill") == nullptr) {
            return fail("doc.outpaint.prepare", "the region-fill recipe is not loaded");
        }
        const CommandResult resized = table.invoke(
            "doc.resize", json{{"width", width}, {"height", height},
                               {"offset_x", dx}, {"offset_y", dy},
                               {"mark_added", true}});
        if (!resized.ok) {
            return resized;
        }
        selection_edit_prepare_outpaint(state);
        const CommandResult asked = table.invoke("verb.ask", json{{"show", "fill"}});
        if (!asked.ok) {
            return asked;
        }
        const CommandResult intent = table.invoke(
            "ai.intent", json{{"recipe", "region-fill"},
                              {"vars", json{{"prompt", prompt}}}});
        if (!intent.ok) {
            return intent;
        }
        return command_ok(json{{"document", resized.value},
                               {"ask", asked.value},
                               {"prompt", prompt}});
    });

    table.add("image.get", "Return document pixels (whole image or a region)", {}, {"region", "base64"},
              [&state](const json& args) -> CommandResult {
        CommandResult guard = require_document(state);
        if (!guard.ok) {
            return guard;
        }
        Rect region{0, 0, state.doc.width(), state.doc.height()};
        if (!arg_rect(args, "region", region)) {
            return fail("image.get", "'region' must be [x, y, width, height] of integers");
        }
        Image crop = state.doc.composite().region(region);
        if (crop.empty()) {
            return fail("image.get", "region is empty after clipping to the image bounds");
        }
        const std::vector<std::uint8_t> png = io::encode_png(crop);
        json result = image_json(crop, "region");
        result["region"] = json::array({region.x, region.y, crop.size().w, crop.size().h});
        bool as_base64 = false;
        if (!arg_bool(args, "base64", as_base64)) {
            return fail("image.get", "'base64' must be a boolean");
        }
        if (as_base64) {
            result["png_base64"] = base64_encode(std::span<const std::uint8_t>(png));
        } else {
            const std::string path = captures_dir(state) + "/image-" + crop.checksum_hex().substr(0, 12) + ".png";
            std::string error;
            if (!io::save_png(path, crop, error)) {
                return fail("image.get", error);
            }
            result["png_path"] = path;
        }
        return command_ok(result);
    });
}

}  // namespace noise::app
