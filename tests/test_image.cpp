// SPDX-License-Identifier: Apache-2.0
#include "test.h"

#include "core/image.h"
#include "io/file.h"
#include "io/flat.h"
#include "io/jpeg.h"
#include "io/png.h"
#include "io/webp.h"
#include "platform/paths.h"

#include <span>

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

// A smooth gradient, which is what a lossy measurement means anything on. `make_pattern` is
// per-pixel noise, and noise is exactly what a DCT codec throws away: the same encode measures a
// mean channel error of 0.5 against this and 110 against that, so a bound that passed on the noise
// would only be a large number.
Image smooth_pattern(Extent size) {
    Image img = Image::filled(size, 0, 0, 0, 255);
    for (int y = 0; y < size.h; ++y) {
        for (int x = 0; x < size.w; ++x) {
            std::uint8_t* p = img.at(x, y);
            p[0] = static_cast<std::uint8_t>((x * 255) / size.w);
            p[1] = static_cast<std::uint8_t>((y * 255) / size.h);
            p[2] = static_cast<std::uint8_t>(128 + ((x + y) * 60) / (size.w + size.h));
            p[3] = 255;
        }
    }
    return img;
}

// Mean per-channel error: what a lossy codec is measured with. Asserting exact bytes would be a
// lie about the format, and asserting only "it decoded" would pass on noise.
double mean_channel_error(const Image& a, const Image& b) {
    if (a.size().w != b.size().w || a.size().h != b.size().h) {
        return 1e9;
    }
    double total = 0.0;
    for (int y = 0; y < a.size().h; ++y) {
        for (int x = 0; x < a.size().w; ++x) {
            const std::uint8_t* pa = a.at(x, y);
            const std::uint8_t* pb = b.at(x, y);
            for (int c = 0; c < 4; ++c) {
                total += static_cast<double>(pa[c] > pb[c] ? pa[c] - pb[c] : pb[c] - pa[c]);
            }
        }
    }
    return total / (static_cast<double>(a.size().w) * static_cast<double>(a.size().h) * 4.0);
}

// What a format with no alpha channel stores of `source`: the composite against the backdrop
// `encode_jpeg` uses, computed here independently of the codec.
Image flattened_reference(const Image& source) {
    Image out = Image::filled(source.size(), 255, 255, 255, 255);
    for (int y = 0; y < source.size().h; ++y) {
        for (int x = 0; x < source.size().w; ++x) {
            const std::uint8_t* p = source.at(x, y);
            const unsigned a = p[3];
            std::uint8_t* q = out.at(x, y);
            for (int c = 0; c < 3; ++c) {
                q[c] = static_cast<std::uint8_t>((p[c] * a + 255 * (255 - a) + 127) / 255);
            }
        }
    }
    return out;
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
            const std::vector<std::uint8_t> encoded = io::encode_png(source);
            CHECK(!encoded.empty());
            std::string error;
            const Image decoded = io::decode_png(std::span<const std::uint8_t>(encoded), error);
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
    CHECK(io::save_png(path, source, error));
    const Image loaded = io::load_png(path, error);
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

// Native file dialogs return UTF-8 even on Windows. Keep that spelling usable
// through disk IO, including when the filename cannot round-trip via the ANSI code page.
void test_unicode_image_path() {
    const std::filesystem::path native =
        std::filesystem::temp_directory_path() / std::filesystem::path(u8"noise-ü-日本.png");
    const std::string path = platform::path_to_utf8(native);
    const Image source = make_pattern(Extent{11, 7});
    std::string error;
    CHECK(io::save_png(path, source, error));
    CHECK_EQ(io::load_png(path, error).checksum_hex(), source.checksum_hex());
    std::vector<std::uint8_t> bytes;
    CHECK(io::read_file(path, bytes, error));
    CHECK_EQ(io::decode_png(bytes, error).checksum_hex(), source.checksum_hex());
    CHECK(io::write_file_atomic(path, bytes, error));
    CHECK_EQ(io::load_png(path, error).checksum_hex(), source.checksum_hex());
    std::filesystem::remove(native);
}

void test_decode_rejects_bad_input() {
    std::string error;
    const std::vector<std::uint8_t> garbage{0x00, 0x01, 0x02, 0x03, 0x04, 0x05};
    CHECK(io::decode_png(std::span<const std::uint8_t>(garbage), error).empty());
    CHECK(!error.empty());

    const Image source = make_pattern(Extent{40, 40});
    std::vector<std::uint8_t> truncated = io::encode_png(source);
    truncated.resize(truncated.size() / 2);
    error.clear();
    CHECK(io::decode_png(std::span<const std::uint8_t>(truncated), error).empty());
    CHECK(!error.empty());

    error.clear();
    CHECK(io::load_png(temp_path("does-not-exist.png"), error).empty());
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

// JPEG carries no alpha and is lossy: what comes back is the composite against white, close to what
// went in, in the shape that went in. Exact bytes would be the wrong promise; being opaque is right.
void test_jpeg_round_trip_is_opaque_and_close() {
    const Image source = smooth_pattern(Extent{64, 48});
    std::string error;

    const std::vector<std::uint8_t> bytes = io::encode_jpeg(source, 90);
    CHECK(bytes.size() > 100);
    CHECK(bytes[0] == 0xff && bytes[1] == 0xd8 && bytes[2] == 0xff);

    const Image back = io::decode_jpeg(std::span<const std::uint8_t>(bytes), error);
    CHECK(!back.empty());
    CHECK(back.size().w == 64 && back.size().h == 48);
    bool all_opaque = true;
    for (int y = 0; y < back.size().h && all_opaque; ++y) {
        for (int x = 0; x < back.size().w; ++x) {
            if (back.at(x, y)[3] != 255) {
                all_opaque = false;
                break;
            }
        }
    }
    CHECK(all_opaque);
    // Close to what the format could actually hold, not to the source's transparent pixels.
    CHECK(mean_channel_error(back, flattened_reference(source)) < 6.0);

    // Deterministic: saving the same image twice produces the same file, which is what makes an
    // export diffable. And `quality` is not decoration.
    CHECK(io::encode_jpeg(source, 90) == bytes);
    CHECK(io::encode_jpeg(source, 20).size() < io::encode_jpeg(source, 95).size());

    const std::string path = temp_path("codec.jpeg");
    CHECK(io::save_jpeg(path, source, 85, error));
    const Image from_disk = io::load_jpeg(path, error);
    CHECK(!from_disk.empty());
    CHECK_EQ(from_disk.size().w, 64);
}

// WebP holds alpha and libwebp's RGBA path is straight alpha, the same definition the app stores
// with - so transparency survives the trip, and a fully transparent column stays transparent.
void test_webp_round_trip_keeps_straight_alpha() {
    const Image source = smooth_pattern(Extent{64, 48});
    const Image noisy = make_pattern(Extent{64, 48});
    std::string error;

    const std::vector<std::uint8_t> bytes = io::encode_webp(source, 90);
    CHECK(bytes.size() > 100);
    const std::string head(bytes.begin(), bytes.begin() + 4);
    CHECK_EQ(head, std::string("RIFF"));
    CHECK_EQ(std::string(bytes.begin() + 8, bytes.begin() + 12), std::string("WEBP"));

    const Image back = io::decode_webp(std::span<const std::uint8_t>(bytes), error);
    CHECK(!back.empty());
    CHECK(back.size().w == 64 && back.size().h == 48);
    CHECK(mean_channel_error(back, source) < 2.0);

    // Straight alpha, kept: a column that went in fully transparent comes back essentially so, and
    // not as an opaque black column with the colour of "transparent" painted into it.
    auto noisy_bytes = io::encode_webp(noisy, 90);
    const Image noisy_back = io::decode_webp(std::span(noisy_bytes), error);
    CHECK(!noisy_back.empty());
    double transparent_column_alpha = 0.0;
    for (int y = 0; y < noisy_back.size().h; ++y) {
        transparent_column_alpha += noisy_back.at(0, y)[3];  // x % 5 == 0 went in fully transparent
    }
    CHECK(transparent_column_alpha / static_cast<double>(back.size().h) < 24.0);

    CHECK(io::encode_webp(source, 90) == bytes);
    CHECK(io::encode_webp(source, 20).size() <= io::encode_webp(source, 95).size());

    const std::string path = temp_path("codec.webp");
    CHECK(io::save_webp(path, source, 85, error));
    CHECK(!io::load_webp(path, error).empty());
}

// Reading decides by the bytes, so a file whose name says nothing still opens, and a file that is
// none of these formats is refused naming the formats and the first bytes of what was found instead.
void test_flat_reading_is_decided_by_the_bytes() {
    const Image source = make_pattern(Extent{32, 24});
    std::string error;
    struct Named {
        const char* file;
        std::vector<std::uint8_t> bytes;
    };
    const std::vector<std::uint8_t> png = io::encode_png(source);
    const std::vector<std::uint8_t> jpeg = io::encode_jpeg(source, 90);
    const std::vector<std::uint8_t> webp = io::encode_webp(source, 90);
    std::vector<Named> cases;
    cases.push_back(Named{"by-content-a.bin", png});
    cases.push_back(Named{"by-content-b.bin", jpeg});
    cases.push_back(Named{"by-content-c.bin", webp});
    cases.push_back(Named{"by-content-d.png", jpeg});  // a JPEG named .png: bytes decide
    for (const Named& item : cases) {
        const std::string path = temp_path(item.file);
        CHECK(io::write_file_atomic(path, item.bytes, error));
        const Image back = io::load_flat_image(path, error);
        CHECK(!back.empty());
        CHECK(back.size().w == 32 && back.size().h == 24);
    }

    const std::string not_an_image = temp_path("by-content-text.bin");
    const std::string text = "this is a text file, not an image";
    CHECK(io::write_file_atomic(
        not_an_image, std::vector<std::uint8_t>(text.begin(), text.end()), error));
    const Image refusal = io::load_flat_image(not_an_image, error);
    CHECK(refusal.empty());
    CHECK(error.find("PNG, JPEG, WebP") != std::string::npos);
    CHECK(error.find("74 68 69 73") != std::string::npos);  // "this", as bytes
}

// An export to a name this app does not write is refused with the list rather than writing
// something and hoping.
void test_flat_writing_refuses_an_extension_it_does_not_have() {
    const Image source = make_pattern(Extent{16, 16});
    std::string error;
    CHECK(!io::save_flat_image(temp_path("nope.tiff"), source, 90, error));
    CHECK(error.find("tiff") != std::string::npos);
    CHECK(error.find("png") != std::string::npos);
    CHECK(error.find("webp") != std::string::npos);
    CHECK(io::flat_format_exists("jpeg"));
    CHECK(!io::flat_format_exists("jxl"));
    CHECK(io::flat_format_has_alpha("webp"));
    CHECK(!io::flat_format_has_alpha("jpg"));
    // PNG is lossless: a quality is a field with nothing to do.
    CHECK(!io::flat_format_has_quality("png"));
    CHECK(io::flat_format_has_quality("jpeg"));
}

int main() {
    test_png_round_trip();
    test_png_file_round_trip_preserves_alpha();
    test_unicode_image_path();
    test_decode_rejects_bad_input();
    test_region_matches_source_pixels();
    test_region_clips_and_rejects();
    test_blit_clips_at_edges();
    test_fill_rect_clips();
    test_empty_image_is_empty();
    test_jpeg_round_trip_is_opaque_and_close();
    test_webp_round_trip_keeps_straight_alpha();
    test_flat_reading_is_decided_by_the_bytes();
    test_flat_writing_refuses_an_extension_it_does_not_have();
    return test::summary("image");
}
