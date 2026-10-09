// SPDX-License-Identifier: Apache-2.0
#include "core/image.h"

#include "core/hash.h"


#include <algorithm>
#include <cassert>
#include <cstring>
#include <fstream>

namespace noise {

namespace {

constexpr int kChannels = 4;

}

std::size_t stride_of(Extent size) {
    return static_cast<std::size_t>(size.w) * kChannels;
}

Image Image::from_rgba(Extent size, std::vector<std::uint8_t> rgba) {
    assert(rgba.size() == stride_of(size) * static_cast<std::size_t>(size.h) &&
           "decoder gave the wrong number of pixels");
    Image img;
    img.size_ = size;
    img.px_ = std::move(rgba);
    return img;
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

}
