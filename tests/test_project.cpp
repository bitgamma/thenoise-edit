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

std::vector<Layer> one(const std::string& name, Image pixels) {
    std::vector<Layer> layers;
    Layer layer;
    layer.name = name;
    layer.pixels = std::move(pixels);
    layers.push_back(std::move(layer));
    return layers;
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

// A format-2 layer row with every field this reader requires. A hand-built file that leaves one
// out is a hostile file, not a convenient one, so the tests that mean to write a valid project
// start here and the ones that mean to write a broken one take a field away.
json layer_rows(std::size_t count) {
    json rows = json::array();
    for (std::size_t index = 0; index < count; ++index) {
        rows.push_back(json{{"mode", "normal"},
                            {"name", "Layer " + std::to_string(index + 1)},
                            {"opacity", 1.0},
                            {"src", "layers/" + std::to_string(index) + "/layer.png"},
                            {"visible", true}});
    }
    return rows;
}

std::vector<io::ZipEntry> members_with_rows(const Image& image, const json& rows, int format,
                                            const std::string& mimetype = io::kMimetype,
                                            const json& claimed_size = json()) {
    json manifest{{"format", format}, {"layers", rows}};
    manifest["size"] =
        claimed_size.is_null() ? json::array({image.size().w, image.size().h}) : claimed_size;

    std::vector<io::ZipEntry> members{
        io::ZipEntry{"mimetype", bytes_of(mimetype)},
        io::ZipEntry{"manifest.json", bytes_of(manifest.dump())},
        io::ZipEntry{"layers/0/layer.png", io::encode_png(image)},
    };
    if (rows.size() > 1) {
        members.push_back(io::ZipEntry{"layers/1/layer.png", io::encode_png(image)});
    }
    return members;
}

std::vector<io::ZipEntry> minimal_members(const Image& image, int format,
                                          std::size_t layers_declared,
                                          const std::string& mimetype = io::kMimetype,
                                          const json& claimed_size = json()) {
    return members_with_rows(image, layer_rows(layers_declared), format, mimetype, claimed_size);
}

void test_pixels_and_layer_name_come_back_unchanged() {
    const Image original = checker(Extent{41, 23});
    const std::string path = temp("roundtrip.noise");
    std::string error;
    CHECK(io::save_project(path, one("Sketch", original), original, {}, error));

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

// What a layer looks like is file content: a save that dropped `visible`, or an open that
// defaulted a missing field instead of refusing, loses the difference between a document the
// user arranged and one the reader invented.
void test_layer_state_round_trips() {
    const Image image = checker(Extent{7, 5});
    std::vector<Layer> stack;
    stack.push_back(Layer{0, "Paper", image, true, 1.0, BlendMode::normal});
    stack.push_back(Layer{0, "Wash", image, false, 0.25, BlendMode::normal});

    const std::string path = temp("state.noise");
    std::string error;
    CHECK(io::save_project(path, stack, image, {}, error));

    io::Project loaded;
    CHECK(io::load_project(path, loaded, error));
    CHECK_EQ(loaded.layers.size(), 2u);
    if (loaded.layers.size() != 2) {
        return;
    }
    CHECK_EQ(loaded.layers[0].visible, true);
    CHECK_EQ(loaded.layers[0].opacity, 1.0);
    CHECK_EQ(loaded.layers[1].visible, false);
    CHECK_EQ(loaded.layers[1].opacity, 0.25);
    CHECK(loaded.layers[1].mode == BlendMode::normal);

    // And the same through the bytes, not just the structs: a field the writer never emitted
    // cannot be read back, however the in-memory test looks.
    std::vector<std::uint8_t> raw;
    CHECK(io::read_file(path, raw, error));
    const auto entries = io::read_zip(std::span<const std::uint8_t>(raw), error);
    CHECK(entries.has_value());
    if (!entries) {
        return;
    }
    const json manifest = json::parse(text_of((*entries)[1].data));
    const json row = manifest["layers"][1];
    CHECK_EQ(row["visible"].get<bool>(), false);
    CHECK_EQ(row["opacity"].get<double>(), 0.25);
    CHECK_EQ(row["mode"].get<std::string>(), std::string("normal"));
    std::filesystem::remove(path);
}

// One format is read, and it is named: opening a file by guessing at a layout this build does
// not have is how a document comes back subtly wrong.
void test_another_format_is_refused_by_name() {
    const Image image = checker(Extent{6, 6});
    for (const int format : {1, io::kFormatVersion + 6}) {
        const std::string path = temp("format-" + std::to_string(format) + ".noise");
        write_archive(path, minimal_members(image, format, 1));
        io::Project loaded;
        std::string error;
        CHECK(!io::load_project(path, loaded, error));
        CHECK(error.find(std::to_string(format)) != std::string::npos);
        CHECK(error.find(std::to_string(io::kFormatVersion)) != std::string::npos);
        CHECK(error.find("format-" + std::to_string(format) + ".noise") != std::string::npos);
        std::filesystem::remove(path);
    }
}

void test_a_layer_that_omits_its_state_is_refused() {
    const Image image = checker(Extent{6, 6});
    for (const char* field : {"visible", "opacity", "mode"}) {
        json rows = layer_rows(1);
        rows[0].erase(field);
        const std::string path = temp(std::string("missing-") + field + ".noise");
        write_archive(path, members_with_rows(image, rows, io::kFormatVersion));
        io::Project loaded;
        std::string error;
        CHECK(!io::load_project(path, loaded, error));
        CHECK(error.find(field) != std::string::npos);
        // Which layer, not just which field: a two-layer file that says "no 'mode'" is a puzzle.
        CHECK(error.find("layer 0") != std::string::npos);
        std::filesystem::remove(path);
    }
}

// Values out of range or out of the mode table are refused with the offending text, because the
// alternative is a layer that opens as something the file never asked for.
void test_an_impossible_layer_state_is_refused() {
    const Image image = checker(Extent{6, 6});

    json high = layer_rows(1);
    high[0]["opacity"] = 1.5;
    const std::string high_path = temp("opaque.noise");
    write_archive(high_path, members_with_rows(image, high, io::kFormatVersion));
    io::Project loaded;
    std::string error;
    CHECK(!io::load_project(high_path, loaded, error));
    CHECK(error.find("1.5") != std::string::npos);
    CHECK(error.find("0..1") != std::string::npos);
    std::filesystem::remove(high_path);

    json odd_mode = layer_rows(1);
    odd_mode[0]["mode"] = "not-a-mode";
    const std::string mode_path = temp("mode.noise");
    write_archive(mode_path, members_with_rows(image, odd_mode, io::kFormatVersion));
    CHECK(!io::load_project(mode_path, loaded, error));
    CHECK(error.find("not-a-mode") != std::string::npos);
    // The names that would have been accepted, so the reader can be fixed without a guess.
    CHECK(error.find("normal") != std::string::npos);
    std::filesystem::remove(mode_path);
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
    CHECK(io::save_project(path, one("Layer 1", image), image, carried, error));

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
    CHECK(io::save_project(path, one("Layer 1", image), image, loaded.carried, error));
    io::Project again;
    CHECK(io::load_project(path, again, error));
    CHECK_EQ(again.carried.size(), 2u);
    std::filesystem::remove(path);
}

// Layers are bottom-first in the file and in memory, and a name that came from elsewhere
// survives a save: rewriting a project must not rename what the user called it.
void test_layers_keep_their_order_and_names_through_a_save() {
    const Image base = checker(Extent{13, 11});
    Image top = checker(Extent{13, 11});
    top.fill_rect(Rect{0, 0, 4, 4}, std::array<std::uint8_t, 4>{9, 9, 9, 255}.data());
    const std::string path = temp("stack.noise");
    std::vector<Layer> stack;
    stack.push_back(Layer{0, "Paper", base});
    stack.push_back(Layer{0, "Ink", top});
    std::string error;
    CHECK(io::save_project(path, stack, top, {}, error));

    io::Project loaded;
    CHECK(io::load_project(path, loaded, error));
    CHECK_EQ(loaded.layers.size(), 2u);
    if (loaded.layers.size() != 2) {
        return;
    }
    CHECK_EQ(loaded.layers[0].name, std::string("Paper"));
    CHECK_EQ(loaded.layers[1].name, std::string("Ink"));
    CHECK_EQ(loaded.layers[0].pixels.checksum_hex(), base.checksum_hex());
    CHECK_EQ(loaded.layers[1].pixels.checksum_hex(), top.checksum_hex());
    // Ids are the open document's business, so a file cannot name a layer it does not have.
    CHECK_EQ(loaded.layers[0].id, 0u);
    std::filesystem::remove(path);
}

// Every layer covers the whole canvas. One that does not would have to be cropped or padded to
// open, and doing either silently is data loss.
void test_a_layer_of_another_size_is_refused() {
    const Image base = checker(Extent{12, 9});
    Image odd = checker(Extent{12, 8});
    const std::string path = temp("odd.noise");
    std::vector<Layer> stack;
    stack.push_back(Layer{0, "Paper", base});
    stack.push_back(Layer{0, "Oversized", odd});
    std::string error;
    CHECK(io::save_project(path, stack, base, {}, error));

    io::Project loaded;
    CHECK(!io::load_project(path, loaded, error));
    CHECK(error.find("12x8") != std::string::npos);
    CHECK(error.find("12x9") != std::string::npos);
    CHECK(error.find("Oversized") != std::string::npos ||
          error.find("layers/1/layer.png") != std::string::npos);
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
    CHECK(!io::save_project(nowhere, one("Layer 1", image), image, {}, error));
    CHECK(!std::filesystem::exists(nowhere));

    const std::string path = temp("replaced.noise");
    CHECK(io::save_project(path, one("First", image), image, {}, error));
    // A directory in the place of the target makes the rename fail, and the original file has
    // to still be there and still be readable.
    const std::string occupied = temp("occupied.noise");
    CHECK(io::save_project(occupied, one("Keep", image), image, {}, error));
    std::filesystem::create_directory(occupied + ".noise-tmp");
    CHECK(!io::save_project(occupied, one("Replace", image), image, {}, error));
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
    test_layer_state_round_trips();
    test_another_format_is_refused_by_name();
    test_a_layer_that_omits_its_state_is_refused();
    test_an_impossible_layer_state_is_refused();
    test_unknown_members_are_carried_through_a_save();
    test_layers_keep_their_order_and_names_through_a_save();
    test_a_layer_of_another_size_is_refused();
    test_a_manifest_that_lies_about_the_size_is_refused();
    test_a_file_that_is_not_a_project_says_so();
    test_a_failed_save_leaves_no_file_and_keeps_the_old_one();
    return test::summary("project");
}
