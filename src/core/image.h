// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "core/geom.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace noise {

// One pixel, as the app names its channels. The store itself is bytes; this is for code that
// reasons about a colour rather than a buffer.
struct Rgba8 {
    std::uint8_t r;
    std::uint8_t g;
    std::uint8_t b;
    std::uint8_t a;
};

// Bytes in one pixel, and the stride the whole store, every codec and every blend loop uses.
inline constexpr int kChannels = 4;

// Row length in bytes. The store is tightly packed, and codecs write with this stride.
std::size_t stride_of(Extent size);

// RGBA8, 8 bits per channel, stride == width * 4. This is the CPU backing store
// the whole app reads from: painting, compositing, saving and AI all go through it.
class Image {
public:
    Image() = default;

    static Image filled(Extent size, std::uint8_t r, std::uint8_t g, std::uint8_t b, std::uint8_t a);
    // Takes the pixels a decoder produced. `rgba` must hold exactly stride_of(size) * size.h
    // bytes; anything else is a decoder bug, so this asserts rather than resizes.
    static Image from_rgba(Extent size, std::vector<std::uint8_t> rgba);

    Extent size() const { return size_; }
    bool empty() const { return size_.w == 0 || size_.h == 0; }
    std::size_t bytes() const { return px_.size(); }
    std::uint8_t* data() { return px_.data(); }
    const std::uint8_t* data() const { return px_.data(); }
    std::span<std::uint8_t> pixels() { return px_; }
    std::span<const std::uint8_t> pixels() const { return px_; }

    std::uint8_t* at(int x, int y);
    const std::uint8_t* at(int x, int y) const;
    bool in_bounds(int x, int y) const { return x >= 0 && y >= 0 && x < size_.w && y < size_.h; }

    Image region(Rect r) const;
    void blit(const Image& src, int dst_x, int dst_y);
    void fill_rect(Rect r, const std::uint8_t rgba[4]);
    std::string checksum_hex() const;

private:
    Extent size_{};
    std::vector<std::uint8_t> px_;
};

}
