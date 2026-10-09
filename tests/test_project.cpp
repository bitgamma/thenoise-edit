// SPDX-License-Identifier: Apache-2.0
#include "io/project.h"

#include "io/file.h"
#include "io/png.h"
#include "platform/platform.h"
#include "test.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

using namespace noise;
using nlohmann::json;

namespace {

std::string temp_dir() {
    static const std::string dir = [] {
        const std::filesystem::path path = std::filesystem::temp_directory_path() /
                                           ("noise-project-test-" +
                                            std::to_string(platform::pid()));
        std::filesystem::create_directories(path);
        return path.string();
    }();
    return dir;
}

std::string temp(const std::string& name) {
    return (std::filesystem::path(temp_dir()) / name).string();
}

Image checker(Extent size) {
    Image img = Image::filled(size, 0, 0, 0, 0);
    for (int y = 0; y < size.h; ++y) {
        for (int x = 0; x < size.w; ++x) {
            const std::uint8_t value = ((x / 4) + (y / 4)) % 2 ? 210 : 30;
            const std::uint8_t alpha = (x + y) % 5 == 0 ? 0 : 255;
            img.at(x, y)[0] = value;
            img.at(x, y)[1] = static_cast<std::uint8_t>(x * 3);
            img.at(x, y)[2] = static_cast<std::uint8_t>(y * 7);
            img.at(x, y)[3] = alpha;
        }
    }
    return img;
}

// A vector, not a span: `bytes_of(json.dump())` twice would otherwise hand begin() and end()
// from two different temporaries, which is a garbage range rather than a compile error.
std::vector<std::uint8_t> bytes_of(const std::string& text) {
    return std::vector<std::uint8_t>(reinterpret_cast<const std::uint8_t*>(text.data()),
                                     reinterpret_cast<const std::uint8_t*>(text.data()) +
                                         text.size());
}

std::string text_of(const std::vector<std::uint8_t>& bytes) {
    return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

// A container written by hand, so the reader can be shown files this app would never write.
void write_archive(const std::string& path, const std::vector<io::ZipEntry>& entries) {
    io::ZipWriter zip;
    for (const io::ZipEntry& entry : entries) {
        zip.add(entry.name, std::span<const std::uint8_t>(entry.data));
    }
    std::string error;
    CHECK(io::write_file_atomic(path, zip.bytes(), error));
}

std::vector<io::ZipEntry> minimal_members(const Image& image, int format,
                                          std::size_t layers_declared,
                                          const std::string& mimetype = io::kMimetype,
                                          const json& claimed_size = json()) {
    json layer_array = json::array();
    for (std::size_t index = 0; index < layers_declared; ++index) {
        layer_array.push_back(json{{"name", "Layer " + std::to_string(index + 1)},
                                   {"src", "layers/" + std::to_string(index) + "/layer.png"}});
    }
    json manifest{{"format", format}, {"layers", layer_array}};
    if (!claimed_size.is_null()) {
        manifest["size"] = claimed_size;
    } else {
        manifest["size"] = json::array({image.size().w, image.size().h});
    }

    std::vector<io::ZipEntry> members{
        io::ZipEntry{"mimetype", bytes_of(mimetype)},
        io::ZipEntry{"manifest.json", bytes_of(manifest.dump())},
        io::ZipEntry{"layers/0/layer.png", io::encode_png(image)},
    };
    if (layers_declared > 1) {
        members.push_back(io::ZipEntry{"layers/1/layer.png", io::encode_png(image)});
    }
    return members;
}

void test_pixels_and_layer_name_come_back_unchanged() {
    const Image original = checker(Extent{41, 23});
    const std::string path = temp("roundtrip.noise");
    std::string error;
    CHECK(io::save_project(path, "Sketch", original, {}, error));

    io::Project loaded;
    CHECK(io::load_project(path, loaded, error));
    CHECK_EQ(loaded.format, io::kFormatVersion);
    CHECK_EQ(loaded.layers.size(), 1u);
    if (loaded.layers.empty()) {
        return;
    }
    CHECK_EQ(loaded.layers[0].name, std::string("Sketch"));
    CHECK_EQ(loaded.layers[0].pixels.checksum_hex(), original.checksum_hex());

    // The container also has to be readable as a plain ZIP with the members the format names,
    // sniffable mimetype first: that is what makes the file useful outside this app.
    std::vector<std::uint8_t> raw;
    CHECK(io::read_file(path, raw, error));
    const auto entries = io::read_zip(std::span<const std::uint8_t>(raw), error);
    CHECK(entries.has_value());
    if (!entries) {
        return;
    }
    const std::vector<std::string> names = [&] {
        std::vector<std::string> out;
        for (const io::ZipEntry& entry : *entries) {
            out.push_back(entry.name);
        }
        return out;
    }();
    CHECK_EQ(names.size(), 4u);
    CHECK_EQ(names[0], std::string("mimetype"));
    CHECK(std::find(names.begin(), names.end(), "manifest.json") != names.end());
    CHECK(std::find(names.begin(), names.end(), "layers/0/layer.png") != names.end());
    CHECK(std::find(names.begin(), names.end(), "mergedimage.png") != names.end());
    const json manifest = json::parse(text_of((*entries)[1].data));
    CHECK_EQ(manifest["format"].get<int>(), io::kFormatVersion);
    CHECK_EQ(manifest["size"][0].get<int>(), 41);
    CHECK_EQ(manifest["size"][1].get<int>(), 23);
    std::filesystem::remove(path);
}

// Members this build does not define are kept and written back: opening a file from a newer
// build must not delete what the app cannot show.
void test_unknown_members_are_carried_through_a_save() {
    const Image image = checker(Extent{8, 6});
    const std::string path = temp("carried.noise");
    const std::string provenance = "{\"prompt\":\"a red square\",\"model\":\"unknown\"}";
    const std::vector<io::ZipEntry> carried{
        io::ZipEntry{"provenance/op.json", bytes_of(provenance)},
        io::ZipEntry{"Thumbnails/thumbnail.png", io::encode_png(image)},
    };
    std::string error;
    CHECK(io::save_project(path, "Layer 1", image, carried, error));

    io::Project loaded;
    CHECK(io::load_project(path, loaded, error));
    CHECK_EQ(loaded.carried.size(), 2u);
    if (loaded.carried.size() != 2) {
        return;
    }
    CHECK_EQ(loaded.carried[0].name, std::string("Thumbnails/thumbnail.png"));
    CHECK_EQ(loaded.carried[1].name, std::string("provenance/op.json"));
    CHECK_EQ(text_of(loaded.carried[1].data), provenance);

    // And a save after that open keeps them, so an edit-and-save cannot lose them either.
    CHECK(io::save_project(path, "Layer 1", image, loaded.carried, error));
    io::Project again;
    CHECK(io::load_project(path, again, error));
    CHECK_EQ(again.carried.size(), 2u);
    std::filesystem::remove(path);
}

void test_a_newer_format_is_refused_by_name() {
    const Image image = checker(Extent{6, 6});
    const std::string path = temp("future.noise");
    write_archive(path, minimal_members(image, io::kFormatVersion + 6, 1));
    io::Project loaded;
    std::string error;
    CHECK(!io::load_project(path, loaded, error));
    CHECK(error.find(std::to_string(io::kFormatVersion + 6)) != std::string::npos);
    CHECK(error.find(std::to_string(io::kFormatVersion)) != std::string::npos);
    CHECK(error.find("future.noise") != std::string::npos);
    std::filesystem::remove(path);
}

// Loading a two-layer file as one would silently throw a layer away, which is data loss.
void test_a_multi_layer_project_is_refused_rather_than_opened_half() {
    const Image image = checker(Extent{6, 6});
    const std::string path = temp("layers.noise");
    write_archive(path, minimal_members(image, io::kFormatVersion, 2));
    io::Project loaded;
    std::string error;
    CHECK(!io::load_project(path, loaded, error));
    CHECK(error.find("2 layers") != std::string::npos);
    std::filesystem::remove(path);
}

void test_a_manifest_that_lies_about_the_size_is_refused() {
    const Image image = checker(Extent{9, 7});
    const std::string path = temp("lying.noise");
    write_archive(path, minimal_members(image, io::kFormatVersion, 1, io::kMimetype,
                                        json::array({9, 8})));
    io::Project loaded;
    std::string error;
    CHECK(!io::load_project(path, loaded, error));
    CHECK(error.find("9x7") != std::string::npos);
    CHECK(error.find("9x8") != std::string::npos);
    std::filesystem::remove(path);
}

void test_a_file_that_is_not_a_project_says_so() {
    const Image image = checker(Extent{4, 4});
    std::string error;

    const std::string wrong_type = temp("openraster.noise");
    write_archive(wrong_type,
                  minimal_members(image, io::kFormatVersion, 1, "application/openraster"));
    io::Project loaded;
    CHECK(!io::load_project(wrong_type, loaded, error));
    CHECK(error.find("openraster") != std::string::npos);

    const std::string not_a_zip = temp("notes.noise");
    const std::string text = "just a note, not a container";
    std::vector<std::uint8_t> raw(text.begin(), text.end());
    CHECK(io::write_file_atomic(not_a_zip, raw, error));
    CHECK(!io::load_project(not_a_zip, loaded, error));
    CHECK(error.find("notes.noise") != std::string::npos);
    std::filesystem::remove(wrong_type);
    std::filesystem::remove(not_a_zip);
}

// The save must not destroy the file it is replacing when it cannot write: a half-written
// container is worse than the older one it replaced.
void test_a_failed_save_leaves_no_file_and_keeps_the_old_one() {
    const Image image = checker(Extent{5, 5});
    std::string error;

    const std::string nowhere = temp("no-such-directory/project.noise");
    CHECK(!io::save_project(nowhere, "Layer 1", image, {}, error));
    CHECK(!std::filesystem::exists(nowhere));

    const std::string path = temp("replaced.noise");
    CHECK(io::save_project(path, "First", image, {}, error));
    // A directory in the place of the target makes the rename fail, and the original file has
    // to still be there and still be readable.
    const std::string occupied = temp("occupied.noise");
    CHECK(io::save_project(occupied, "Keep", image, {}, error));
    std::filesystem::create_directory(occupied + ".noise-tmp");
    CHECK(!io::save_project(occupied, "Replace", image, {}, error));
    io::Project loaded;
    CHECK(io::load_project(occupied, loaded, error));
    CHECK_EQ(loaded.layers.size(), 1u);
    if (!loaded.layers.empty()) {
        CHECK_EQ(loaded.layers[0].name, std::string("Keep"));
    }
    std::filesystem::remove(path);
    std::filesystem::remove(occupied);
}

}  // namespace

int main() {
    test_pixels_and_layer_name_come_back_unchanged();
    test_unknown_members_are_carried_through_a_save();
    test_a_newer_format_is_refused_by_name();
    test_a_multi_layer_project_is_refused_rather_than_opened_half();
    test_a_manifest_that_lies_about_the_size_is_refused();
    test_a_file_that_is_not_a_project_says_so();
    test_a_failed_save_leaves_no_file_and_keeps_the_old_one();
    return test::summary("project");
}
