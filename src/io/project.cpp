// SPDX-License-Identifier: Apache-2.0
#include "io/project.h"

#include "core/document.h"
#include "io/file.h"
#include "io/png.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdint>
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
    if (format > kFormatVersion) {
        error = "project format " + std::to_string(format) + " is newer than this build reads (" +
                std::to_string(kFormatVersion) + "): " + path;
        return false;
    }
    if (format < 1) {
        error = "project format " + std::to_string(format) + " is not understood: " + path;
        return false;
    }

    if (!manifest.contains("layers") || !manifest.at("layers").is_array() ||
        manifest.at("layers").empty()) {
        error = std::string(kManifestName) + " lists no layers in " + path;
        return false;
    }
    const json& layers = manifest.at("layers");
    if (layers.size() != 1) {
        error = "this build opens single-layer projects; " + path + " has " +
                std::to_string(layers.size()) + " layers";
        return false;
    }
    const json& layer = layers.front();
    if (!layer.contains("src") || !layer.at("src").is_string()) {
        error = "layer 0 has no 'src' in " + path;
        return false;
    }
    const std::string source = layer.at("src").get<std::string>();
    const ZipEntry* layer_entry = find(*entries, source);
    if (layer_entry == nullptr) {
        error = "layer 0 names '" + source + "', which the archive does not hold: " + path;
        return false;
    }

    std::string decode_error;
    Image pixels = decode_png(std::span<const std::uint8_t>(layer_entry->data), decode_error);
    if (pixels.empty()) {
        error = "'" + source + "' in " + path + ": " + decode_error;
        return false;
    }
    if (manifest.contains("size")) {
        const json& size = manifest.at("size");
        if (!size.is_array() || size.size() != 2 || !size[0].is_number_integer() ||
            !size[1].is_number_integer()) {
            error = std::string(kManifestName) + " has a malformed 'size' in " + path;
            return false;
        }
        const Extent claimed{size[0].get<int>(), size[1].get<int>()};
        if (claimed != pixels.size()) {
            error = "'" + source + "' is " + std::to_string(pixels.size().w) + "x" +
                    std::to_string(pixels.size().h) + " but the manifest says " +
                    std::to_string(claimed.w) + "x" + std::to_string(claimed.h) + " in " + path;
            return false;
        }
    }

    out = Project{};
    out.format = format;
    out.layers.push_back(ProjectLayer{
        layer.contains("name") && layer.at("name").is_string()
            ? layer.at("name").get<std::string>()
            : std::string(kDefaultLayerName),
        std::move(pixels)});

    for (const ZipEntry& entry : *entries) {
        if (entry.name == "mimetype" || entry.name == kManifestName ||
            entry.name == kMergedName || entry.name == source) {
            continue;
        }
        out.carried.push_back(entry);
    }
    std::sort(out.carried.begin(), out.carried.end(),
              [](const ZipEntry& a, const ZipEntry& b) { return a.name < b.name; });
    return true;
}

bool save_project(const std::string& path, const std::string& layer_name, const Image& pixels,
                  std::span<const ZipEntry> carried, std::string& error) {
    if (pixels.empty()) {
        error = "layer 0 has no pixels to save";
        return false;
    }

    const json manifest{
        {"format", kFormatVersion},
        {"size", json::array({pixels.size().w, pixels.size().h})},
        {"layers", json::array({json{
                       {"name", layer_name},
                       {"src", layer_source(0)},
                   }})},
    };

    ZipWriter zip;
    // The order is the format: sniffable mimetype first, then the app's own manifest, then the
    // payload, then anything this build carried through untouched.
    zip.add("mimetype", bytes_of(kMimetype));
    zip.add(kManifestName, bytes_of(manifest.dump(2) + "\n"));
    zip.add(layer_source(0), std::span<const std::uint8_t>(encode_png(pixels)));
    zip.add(kMergedName, std::span<const std::uint8_t>(encode_png(pixels)));
    for (const ZipEntry& entry : carried) {
        zip.add(entry.name, std::span<const std::uint8_t>(entry.data));
    }
    return write_file_atomic(path, zip.bytes(), error);
}

}  // namespace noise::io
