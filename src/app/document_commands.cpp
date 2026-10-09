// SPDX-License-Identifier: Apache-2.0
#include "app/command_groups.h"

#include "app/command_args.h"

#include "app/commands.h"
#include "core/base64.h"
#include "core/log.h"

#include <nlohmann/json.hpp>


namespace noise::app {

void register_document_commands(AppState& state, CommandTable& table) {
    table.add("doc.get", "Report the document (path, size, checksum, revision, history)", {},
              [&state](const json&) -> CommandResult {
        CommandResult guard = require_document(state);
        if (!guard.ok) {
            return guard;
        }
        return command_ok(document_json(state));
    });

    table.add("doc.new", "Create an empty document", {}, [&state](const json& args) -> CommandResult {
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
        state.doc.reset(Image::filled(Extent{width, height}, fill[0], fill[1], fill[2], fill[3]),
                          std::string());
        request_fit(state);
        log::info("created image " + std::to_string(width) + "x" + std::to_string(height));
        return command_ok(document_json(state));
    });

    table.add("doc.open", "Load a PNG into the document", {}, [&state](const json& args) -> CommandResult {
        if (!args.contains("path") || !args.at("path").is_string()) {
            return fail("doc.open", "expected string 'path'");
        }
        const std::string path = args.at("path").get<std::string>();
        std::string error;
        Image loaded = Image::load_png(path, error);
        if (loaded.empty()) {
            return fail("doc.open", error + " (" + path + ")");
        }
        state.doc.reset(std::move(loaded), path);
        request_fit(state);
        log::info("loaded " + path);
        return command_ok(document_json(state));
    });

    table.add("doc.save", "Save the document (defaults to its current path)", {"Ctrl+S"},
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
        std::string error;
        if (!state.doc.pixels().save_png(path, error)) {
            return fail("doc.save", error);
        }
        state.doc.set_path(path);
        return command_ok(document_json(state));
    });

    table.add("image.get", "Return document pixels (whole image or a region)", {},
              [&state](const json& args) -> CommandResult {
        CommandResult guard = require_document(state);
        if (!guard.ok) {
            return guard;
        }
        Rect region{0, 0, state.doc.width(), state.doc.height()};
        if (!arg_rect(args, "region", region)) {
            return fail("image.get", "'region' must be [x, y, width, height] of integers");
        }
        Image crop = state.doc.pixels().region(region);
        if (crop.empty()) {
            return fail("image.get", "region is empty after clipping to the image bounds");
        }
        const std::vector<std::uint8_t> png = crop.encode_png();
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
            if (!crop.save_png(path, error)) {
                return fail("image.get", error);
            }
            result["png_path"] = path;
        }
        return command_ok(result);
    });
}

}  // namespace noise::app
