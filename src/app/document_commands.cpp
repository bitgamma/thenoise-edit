// SPDX-License-Identifier: Apache-2.0
#include "app/command_groups.h"

#include "app/command_args.h"

#include "app/commands.h"
#include "core/base64.h"
#include "core/log.h"
#include "io/file.h"
#include "io/config.h"
#include "io/flat.h"
#include "io/png.h"
#include "io/project.h"

#include <nlohmann/json.hpp>

#include <cstddef>
#include <span>


namespace noise::app {

namespace {

// One document ends and another begins. A job may land only in the document that asked for it, so every
// door that replaces the document says so here - a door that forgot would let an answer queued against
// the last file land in this one.
void note_new_document(AppState& state) {
    ++state.document_epoch;
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
            loaded = std::move(project.layers);
            opened_selection = std::move(project.selection);
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
        state.doc.reset(std::move(loaded), std::move(opened_selection), path);
        note_new_document(state);
        io::remember_recent_file(path);
        request_fit(state);
        log::info("loaded " + path + " (" + std::to_string(state.doc.layers().size()) +
                  " layers)");
        return command_ok(document_json(state));
    });

    table.add("doc.save", "Save the project (defaults to its current path)", {"Ctrl+S"}, {"path"},
              [&state](const json& args) -> CommandResult {
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
        if (path.empty()) {
            return fail("doc.save", "document has no path and none was given");
        }
        if (io::extension_of(path) != io::kProjectExtension) {
            return fail("doc.save",
                        "doc.save writes ." + std::string(io::kProjectExtension) +
                            "; use doc.export for a flat image (got '" + path + "')");
        }
        std::string error;
        if (!io::save_project(path, state.doc.layers(), state.doc.composite(),
                              state.doc.selection(), state.carried,
                              error)) {
            return fail("doc.save", error);
        }
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
