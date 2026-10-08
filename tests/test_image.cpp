// SPDX-License-Identifier: Apache-2.0
#include "test.h"

#include "core/image.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

using namespace noise;

namespace {

// Deterministic pattern that exercises all four channels, including
// alpha 0 (fully transparent) and values with set high bits.
Image make_pattern(Extent size) {
    Image img = Image::filled(size, 0, 0, 0, 0);
    for (int y = 0; y < size.h; ++y) {
        for (int x = 0; x < size.w; ++x) {
            std::uint8_t* p = img.at(x, y);
            p[0] = static_cast<std::uint8_t>((x * 7 + y * 3) & 0xff);
            p[1] = static_cast<std::uint8_t>((x * 131 + y * 17) & 0xff);
            p[2] = static_cast<std::uint8_t>((x + y) & 0xff);
            p[3] = static_cast<std::uint8_t>((x % 5 == 0) ? 0 : ((x * 251 + y * 5) & 0xff));
        }
    }
    return img;
}

std::string temp_path(const std::string& name) {
    return (std::filesystem::temp_directory_path() / ("noise-test-" + name)).string();
}

void test_png_round_trip() {
    const int widths[] = {1, 3, 17, 65, 256};
    const int heights[] = {1, 5, 33, 64};
    for (const int w : widths) {
        for (const int h : heights) {
            const Image source = make_pattern(Extent{w, h});
            const std::vector<std::uint8_t> encoded = source.encode_png();
            CHECK(!encoded.empty());
            std::string error;
            const Image decoded = Image::decode_png(std::span<const std::uint8_t>(encoded), error);
            CHECK_EQ(decoded.size().w, w);
            CHECK_EQ(decoded.size().h, h);
            CHECK_EQ(decoded.bytes(), source.bytes());
            CHECK_EQ(decoded.checksum_hex(), source.checksum_hex());
        }
    }
}

void test_png_file_round_trip_preserves_alpha() {
    const Image source = make_pattern(Extent{23, 11});
    const std::string path = temp_path("alpha.png");
    std::string error;
    CHECK(source.save_png(path, error));
    const Image loaded = Image::load_png(path, error);
    CHECK_EQ(loaded.checksum_hex(), source.checksum_hex());
    // Transparent pixels must stay transparent, not be flattened onto anything.
    for (int y = 0; y < source.size().h; ++y) {
        for (int x = 0; x < source.size().w; ++x) {
            if (source.at(x, y)[3] == 0) {
                CHECK_EQ(static_cast<int>(loaded.at(x, y)[3]), 0);
            }
        }
    }
    std::filesystem::remove(path);
}

void test_decode_rejects_bad_input() {
    std::string error;
    const std::vector<std::uint8_t> garbage{0x00, 0x01, 0x02, 0x03, 0x04, 0x05};
    CHECK(Image::decode_png(std::span<const std::uint8_t>(garbage), error).empty());
    CHECK(!error.empty());

    const Image source = make_pattern(Extent{40, 40});
    std::vector<std::uint8_t> truncated = source.encode_png();
    truncated.resize(truncated.size() / 2);
    error.clear();
    CHECK(Image::decode_png(std::span<const std::uint8_t>(truncated), error).empty());
    CHECK(!error.empty());

    error.clear();
    CHECK(Image::load_png(temp_path("does-not-exist.png"), error).empty());
    CHECK(error.find("does-not-exist.png") != std::string::npos);
}

void test_region_matches_source_pixels() {
    const Image source = make_pattern(Extent{64, 48});
    const Rect region{7, 5, 20, 11};
    const Image crop = source.region(region);
    CHECK_EQ(crop.size().w, 20);
    CHECK_EQ(crop.size().h, 11);
    for (int y = 0; y < 11; ++y) {
        for (int x = 0; x < 20; ++x) {
            const std::uint8_t* a = source.at(region.x + x, region.y + y);
            const std::uint8_t* b = crop.at(x, y);
            CHECK_EQ(static_cast<int>(a[0]), static_cast<int>(b[0]));
            CHECK_EQ(static_cast<int>(a[1]), static_cast<int>(b[1]));
            CHECK_EQ(static_cast<int>(a[2]), static_cast<int>(b[2]));
            CHECK_EQ(static_cast<int>(a[3]), static_cast<int>(b[3]));
        }
    }
}

void test_region_clips_and_rejects() {
    const Image source = make_pattern(Extent{16, 16});
    const Image clipped = source.region(Rect{-8, -8, 32, 32});
    CHECK_EQ(clipped.size().w, 16);
    CHECK_EQ(clipped.size().h, 16);
    CHECK_EQ(clipped.checksum_hex(), source.checksum_hex());

    CHECK(source.region(Rect{16, 0, 4, 4}).empty());
    CHECK(source.region(Rect{0, 16, 4, 4}).empty());
    CHECK(source.region(Rect{0, 0, 0, 5}).empty());
}

void test_blit_clips_at_edges() {
    Image target = Image::filled(Extent{16, 16}, 10, 20, 30, 255);
    const Image patch = make_pattern(Extent{8, 8});
    const std::string untouched_checksum = target.checksum_hex();

    target.blit(patch, -3, -3);
    CHECK(target.checksum_hex() != untouched_checksum);
    for (int y = 0; y < 5; ++y) {
        for (int x = 0; x < 5; ++x) {
            const std::uint8_t* a = patch.at(x + 3, y + 3);
            const std::uint8_t* b = target.at(x, y);
            CHECK_EQ(static_cast<int>(a[0]), static_cast<int>(b[0]));
            CHECK_EQ(static_cast<int>(a[3]), static_cast<int>(b[3]));
        }
    }
    // The far corner must still hold the original fill.
    const std::uint8_t* corner = target.at(15, 15);
    CHECK_EQ(static_cast<int>(corner[0]), 10);
    CHECK_EQ(static_cast<int>(corner[1]), 20);

    Image outside = Image::filled(Extent{16, 16}, 10, 20, 30, 255);
    outside.blit(patch, 16, 16);
    CHECK_EQ(outside.checksum_hex(), untouched_checksum);
    outside.blit(patch, -8, 0);
    CHECK_EQ(outside.checksum_hex(), untouched_checksum);
}

void test_fill_rect_clips() {
    Image img = Image::filled(Extent{10, 10}, 0, 0, 0, 255);
    const std::uint8_t red[4] = {255, 0, 0, 255};
    img.fill_rect(Rect{-5, -5, 8, 8}, red);
    CHECK_EQ(static_cast<int>(img.at(0, 0)[0]), 255);
    CHECK_EQ(static_cast<int>(img.at(2, 2)[0]), 255);
    CHECK_EQ(static_cast<int>(img.at(4, 4)[0]), 0);
    img.fill_rect(Rect{100, 100, 4, 4}, red);
    CHECK_EQ(static_cast<int>(img.at(9, 9)[0]), 0);
}

void test_empty_image_is_empty() {
    Image empty;
    CHECK(empty.empty());
    CHECK_EQ(empty.bytes(), std::size_t(0));
    CHECK(Image::filled(Extent{0, 5}, 0, 0, 0, 0).empty());
}

}

int main() {
    test_png_round_trip();
    test_png_file_round_trip_preserves_alpha();
    test_decode_rejects_bad_input();
    test_region_matches_source_pixels();
    test_region_clips_and_rejects();
    test_blit_clips_at_edges();
    test_fill_rect_clips();
    test_empty_image_is_empty();
    return test::summary("image");
}
