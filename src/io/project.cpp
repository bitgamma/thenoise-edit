// SPDX-License-Identifier: Apache-2.0
#include "io/project.h"

#include "core/layer.h"
#include "io/file.h"
#include "io/png.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace noise::io {
namespace {

using nlohmann::json;

constexpr const char* kManifestName = "manifest.json";
constexpr const char* kMergedName = "mergedimage.png";

std::span<const std::uint8_t> bytes_of(const std::string& text) {
    return std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(text.data()),
                                         text.size());
}

std::string text_of(const std::vector<std::uint8_t>& bytes) {
    return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

std::string layer_source(std::size_t index) {
    return "layers/" + std::to_string(index) + "/layer.png";
}

const ZipEntry* find(const std::vector<ZipEntry>& entries, const std::string& name) {
    for (const ZipEntry& entry : entries) {
        if (entry.name == name) {
            return &entry;
        }
    }
    return nullptr;
}

}  // namespace

bool load_project(const std::string& path, Project& out, std::string& error) {
    std::vector<std::uint8_t> raw;
    if (!read_file(path, raw, error)) {
        return false;
    }
    const auto entries = read_zip(std::span<const std::uint8_t>(raw), error);
    if (!entries) {
        error = error + " (" + path + ")";
        return false;
    }

    const ZipEntry* mimetype = find(*entries, "mimetype");
    if (mimetype == nullptr) {
        error = "no mimetype member, so this is not a .noise file: " + path;
        return false;
    }
    if (text_of(mimetype->data) != kMimetype) {
        error = "mimetype '" + text_of(mimetype->data) + "' is not '" + kMimetype + "': " + path;
        return false;
    }

    const ZipEntry* manifest_entry = find(*entries, kManifestName);
    if (manifest_entry == nullptr) {
        error = std::string("no ") + kManifestName + " in " + path;
        return false;
    }
    const json manifest = json::parse(text_of(manifest_entry->data), nullptr, false);
    if (manifest.is_discarded()) {
        error = std::string(kManifestName) + " is not valid JSON in " + path;
        return false;
    }
    if (!manifest.contains("format") || !manifest.at("format").is_number_integer()) {
        error = std::string(kManifestName) + " has no integer 'format' in " + path;
        return false;
    }
    const int format = manifest.at("format").get<int>();
    if (format != kFormatVersion) {
        // One version is read: this one. Files from earlier sessions are disposable, and the
        // alternative is guessing at fields a newer or older layout did not have to write.
        error = "project format " + std::to_string(format) + " is not the format this build reads (" +
                std::to_string(kFormatVersion) + "): " + path;
        return false;
    }

    if (!manifest.contains("layers") || !manifest.at("layers").is_array() ||
        manifest.at("layers").empty()) {
        error = std::string(kManifestName) + " lists no layers in " + path;
        return false;
    }

    Extent claimed{0, 0};
    if (manifest.contains("size")) {
        const json& size = manifest.at("size");
        if (!size.is_array() || size.size() != 2 || !size[0].is_number_integer() ||
            !size[1].is_number_integer()) {
            error = std::string(kManifestName) + " has a malformed 'size' in " + path;
            return false;
        }
        claimed = Extent{size[0].get<int>(), size[1].get<int>()};
    }

    std::vector<Layer> loaded;
    std::vector<std::string> sources;
    std::optional<Mask> selection;
    const json& declared = manifest.at("layers");
    for (std::size_t index = 0; index < declared.size(); ++index) {
        const json& layer = declared[index];
        if (!layer.contains("src") || !layer.at("src").is_string()) {
            error = "layer " + std::to_string(index) + " has no 'src' in " + path;
            return false;
        }
        const std::string source = layer.at("src").get<std::string>();
        const ZipEntry* layer_entry = find(*entries, source);
        if (layer_entry == nullptr) {
            error = "layer " + std::to_string(index) + " names '" + source +
                    "', which the archive does not hold: " + path;
            return false;
        }
        std::string decode_error;
        Image pixels = decode_png(std::span<const std::uint8_t>(layer_entry->data), decode_error);
        if (pixels.empty()) {
            error = "'" + source + "' in " + path + ": " + decode_error;
            return false;
        }
        if (!claimed.w && !claimed.h) {
            claimed = pixels.size();
        }
        if (pixels.size() != claimed) {
            error = "'" + source + "' is " + std::to_string(pixels.size().w) + "x" +
                    std::to_string(pixels.size().h) + " but the document is " +
                    std::to_string(claimed.w) + "x" + std::to_string(claimed.h) + " in " + path;
            return false;
        }
        // What a layer looks like is as much a part of the file as what it holds, so these are
        // required and a row that omits or misspells one is refused with the field named. A
        // `name` is the exception: a file from elsewhere may simply not have named its layers.
        if (!layer.contains("visible") || !layer.at("visible").is_boolean()) {
            error = "layer " + std::to_string(index) + " has no boolean 'visible' in " + path;
            return false;
        }
        const bool visible = layer.at("visible").get<bool>();

        if (!layer.contains("opacity") || !layer.at("opacity").is_number()) {
            error = "layer " + std::to_string(index) + " has no numeric 'opacity' in " + path;
            return false;
        }
        const double opacity = layer.at("opacity").get<double>();
        if (!(opacity >= 0.0 && opacity <= 1.0)) {
            error = "layer " + std::to_string(index) + " has opacity " + std::to_string(opacity) +
                    ", which is not in 0..1, in " + path;
            return false;
        }

        if (!layer.contains("mode") || !layer.at("mode").is_string()) {
            error = "layer " + std::to_string(index) + " has no string 'mode' in " + path;
            return false;
        }
        const std::string mode_name = layer.at("mode").get<std::string>();
        const std::optional<BlendMode> mode = blend_mode_from_name(mode_name);
        if (!mode) {
            error = "layer " + std::to_string(index) + " has mode '" + mode_name +
                    "', this build knows: " + blend_mode_names() + " in " + path;
            return false;
        }

        loaded.push_back(Layer{0,
                               layer.contains("name") && layer.at("name").is_string()
                                   ? layer.at("name").get<std::string>()
                                   : default_layer_name(index),
                               std::move(pixels),
                               visible,
                               opacity,
                               *mode});
        sources.push_back(source);
    }

    // Absent is a real answer: it means the document had no selection. Present-but-unreadable is a
    // refusal, because a selection the reader cannot honour would let writes through that the user
    // had fenced off.
    if (manifest.contains("selection")) {
        const json& row = manifest.at("selection");
        if (!row.is_object() || !row.contains("src") || !row.at("src").is_string()) {
            error = std::string(kManifestName) +
                    " has a 'selection' with no string 'src' in " + path;
            return false;
        }
        const std::string source = row.at("src").get<std::string>();
        const ZipEntry* plane = find(*entries, source);
        if (plane == nullptr) {
            error = "'selection' names '" + source + "', which the archive does not hold: " + path;
            return false;
        }
        selection = Mask::from_plane(claimed, plane->data);
        if (!selection) {
            error = "'" + source + "' holds " + std::to_string(plane->data.size()) +
                    " coverage bytes, not one per pixel of a " + std::to_string(claimed.w) + "x" +
                    std::to_string(claimed.h) + " canvas in " + path;
            return false;
        }
        sources.push_back(source);
    }

    out = Project{};
    out.format = format;
    out.layers = std::move(loaded);
    out.selection = std::move(selection);

    for (const ZipEntry& entry : *entries) {
        if (entry.name == "mimetype" || entry.name == kManifestName || entry.name == kMergedName ||
            std::find(sources.begin(), sources.end(), entry.name) != sources.end()) {
            continue;
        }
        out.carried.push_back(entry);
    }
    std::sort(out.carried.begin(), out.carried.end(),
              [](const ZipEntry& a, const ZipEntry& b) { return a.name < b.name; });
    return true;
}

bool save_project(const std::string& path, std::span<const Layer> layers, const Image& merged,
                  const std::optional<Mask>& selection, std::span<const ZipEntry> carried,
                  std::string& error) {
    if (layers.empty()) {
        error = "a project needs at least one layer to save: " + path;
        return false;
    }
    if (merged.empty()) {
        error = "the composite has no pixels to write as " + std::string(kMergedName) + ": " + path;
        return false;
    }

    json declared = json::array();
    for (std::size_t index = 0; index < layers.size(); ++index) {
        declared.push_back(json{{"mode", std::string(blend_mode_name(layers[index].mode))},
                                {"name", layers[index].name},
                                {"opacity", layers[index].opacity},
                                {"src", layer_source(index)},
                                {"visible", layers[index].visible}});
    }
    json manifest{
        {"format", kFormatVersion},
        {"size", json::array({merged.size().w, merged.size().h})},
        {"layers", declared},
    };
    if (selection) {
        // Its size is the canvas's, so the row names the member and nothing else: a width repeated
        // here is a fact that can disagree with `size`.
        manifest["selection"] = json{{"src", kSelectionName}};
    }

    ZipWriter zip;
    // The order is the format: sniffable mimetype first, then the app's own manifest, then the
    // layers bottom-first, then the composite, then anything this build carried through.
    zip.add("mimetype", bytes_of(kMimetype));
    zip.add(kManifestName, bytes_of(manifest.dump(2) + "\n"));
    for (std::size_t index = 0; index < layers.size(); ++index) {
        zip.add(layer_source(index), std::span<const std::uint8_t>(encode_png(layers[index].pixels)));
    }
    if (selection) {
        zip.add(kSelectionName, std::span<const std::uint8_t>(selection->plane()));
    }
    zip.add(kMergedName, std::span<const std::uint8_t>(encode_png(merged)));
    for (const ZipEntry& entry : carried) {
        zip.add(entry.name, std::span<const std::uint8_t>(entry.data));
    }
    return write_file_atomic(path, zip.bytes(), error);
}

}  // namespace noise::io
