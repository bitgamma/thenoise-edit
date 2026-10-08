// SPDX-License-Identifier: Apache-2.0
#include "core/image.h"

#include "core/hash.h"

#include <png.h>

#include <algorithm>
#include <cassert>
#include <cstring>
#include <fstream>

namespace noise {

namespace {

constexpr int kChannels = 4;

std::size_t stride_of(Extent size) {
    return static_cast<std::size_t>(size.w) * kChannels;
}

}

Image Image::filled(Extent size, std::uint8_t r, std::uint8_t g, std::uint8_t b, std::uint8_t a) {
    Image img;
    img.size_ = size;
    img.px_.assign(stride_of(size) * static_cast<std::size_t>(size.h), 0u);
    img.fill_rect(Rect{0, 0, size.w, size.h}, std::array<std::uint8_t, 4>{r, g, b, a}.data());
    return img;
}

std::uint8_t* Image::at(int x, int y) {
    assert(in_bounds(x, y));
    return px_.data() + (static_cast<std::size_t>(y) * static_cast<std::size_t>(size_.w) +
                         static_cast<std::size_t>(x)) * kChannels;
}

const std::uint8_t* Image::at(int x, int y) const {
    assert(in_bounds(x, y));
    return px_.data() + (static_cast<std::size_t>(y) * static_cast<std::size_t>(size_.w) +
                         static_cast<std::size_t>(x)) * kChannels;
}

Image Image::region(Rect r) const {
    const Rect clipped = clamp_rect(r, size_);
    Image out = filled(Extent{clipped.w, clipped.h}, 0, 0, 0, 0);
    if (clipped.empty()) {
        return out;
    }
    for (int y = 0; y < clipped.h; ++y) {
        const std::uint8_t* src = at(clipped.x, clipped.y + y);
        std::uint8_t* dst = out.at(0, y);
        std::memcpy(dst, src, static_cast<std::size_t>(clipped.w) * kChannels);
    }
    return out;
}

void Image::blit(const Image& src, int dst_x, int dst_y) {
    if (src.empty() || empty()) {
        return;
    }
    const Rect target = clamp_rect(Rect{dst_x, dst_y, src.size().w, src.size().h}, size_);
    const int off_x = target.x - dst_x;
    const int off_y = target.y - dst_y;
    for (int y = 0; y < target.h; ++y) {
        const std::uint8_t* src_px = src.at(off_x, off_y + y);
        std::uint8_t* dst_px = at(target.x, target.y + y);
        std::memcpy(dst_px, src_px, static_cast<std::size_t>(target.w) * kChannels);
    }
}

void Image::fill_rect(Rect r, const std::uint8_t rgba[4]) {
    const Rect clipped = clamp_rect(r, size_);
    for (int y = 0; y < clipped.h; ++y) {
        std::uint8_t* row = at(clipped.x, clipped.y + y);
        for (int x = 0; x < clipped.w; ++x) {
            std::memcpy(row + static_cast<std::size_t>(x) * kChannels, rgba, kChannels);
        }
    }
}

std::string Image::checksum_hex() const {
    return sha256_hex(pixels());
}

Image Image::decode_png(std::span<const std::uint8_t> bytes, std::string& error) {
    png_image png{};
    png.version = PNG_IMAGE_VERSION;
    if (!png_image_begin_read_from_memory(&png, bytes.data(), bytes.size())) {
        error = std::string("png decode: ") + png.message;
        return {};
    }
    png.format = PNG_FORMAT_RGBA;
    Image img;
    img.size_ = Extent{static_cast<int>(png.width), static_cast<int>(png.height)};
    img.px_.resize(PNG_IMAGE_SIZE(png));
    if (!png_image_finish_read(&png, nullptr, img.px_.data(),
                               static_cast<png_int_32>(stride_of(img.size_)), nullptr)) {
        error = std::string("png decode: ") + png.message;
        return {};
    }
    return img;
}

Image Image::load_png(const std::string& path, std::string& error) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        error = "cannot open file: " + path;
        return {};
    }
    std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(file)),
                                    std::istreambuf_iterator<char>());
    return decode_png(std::span<const std::uint8_t>(bytes), error);
}

std::vector<std::uint8_t> Image::encode_png() const {
    png_image png{};
    png.version = PNG_IMAGE_VERSION;
    png.width = static_cast<png_uint_32>(size_.w);
    png.height = static_cast<png_uint_32>(size_.h);
    png.format = PNG_FORMAT_RGBA;

    png_alloc_size_t needed = 0;
    if (!png_image_write_to_memory(&png, nullptr, &needed, 0, px_.data(),
                                   static_cast<png_int_32>(stride_of(size_)), nullptr)) {
        return {};
    }
    std::vector<std::uint8_t> out(needed);
    if (!png_image_write_to_memory(&png, out.data(), &needed, 0, px_.data(),
                                   static_cast<png_int_32>(stride_of(size_)), nullptr)) {
        return {};
    }
    out.resize(needed);
    return out;
}

bool Image::save_png(const std::string& path, std::string& error) const {
    const std::vector<std::uint8_t> bytes = encode_png();
    if (bytes.empty()) {
        error = "png encode failed";
        return false;
    }
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) {
        error = "cannot write file: " + path;
        return false;
    }
    file.write(reinterpret_cast<const char*>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
    if (!file) {
        error = "short write: " + path;
        return false;
    }
    return true;
}

}
