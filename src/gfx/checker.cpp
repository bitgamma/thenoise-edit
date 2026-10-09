// SPDX-License-Identifier: Apache-2.0
#include "gfx/checker.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace noise::gfx {

namespace {

constexpr double kSquareWorldPx = 8.0;
constexpr double kSquareMinPx = 4.0;
constexpr double kSquareMaxPx = 64.0;
constexpr std::uint8_t kShadeLight = 204;
constexpr std::uint8_t kShadeDark = 153;

double wrapped_unit(double u) {
    return u - std::floor(u);
}

// Screen pixels per checker square.
double square_px(double scale) {
    return std::clamp(kSquareWorldPx * scale, kSquareMinPx, kSquareMaxPx);
}

}

CheckerUv checker_uv(Vec2 rect_min, Vec2 rect_size, Vec2 canvas_min, double scale) {
    // One UV repeat covers two squares: the tile is a 2x2 checker.
    const double period = 2.0 * square_px(scale);
    const double u0 = wrapped_unit((rect_min.x - canvas_min.x) / period);
    const double v0 = wrapped_unit((rect_min.y - canvas_min.y) / period);
    return CheckerUv{static_cast<float>(u0), static_cast<float>(v0),
                     static_cast<float>(u0 + rect_size.x / period),
                     static_cast<float>(v0 + rect_size.y / period)};
}

void Checker::init() {
    // Texel (0,0) is the light square at the canvas's top-left corner, so the pattern
    // reads the same way up as the projection ImGui draws with.
    const std::uint8_t pixels[16] = {
        kShadeLight, kShadeLight, kShadeLight, 255, kShadeDark,  kShadeDark,  kShadeDark,  255,
        kShadeDark,  kShadeDark,  kShadeDark,  255, kShadeLight, kShadeLight, kShadeLight, 255,
    };

    sg_image_desc image_desc{};
    image_desc.width = 2;
    image_desc.height = 2;
    image_desc.pixel_format = SG_PIXELFORMAT_RGBA8;
    image_desc.label = "noise-checker";
    image_desc.data.mip_levels[0].ptr = pixels;
    image_desc.data.mip_levels[0].size = sizeof(pixels);
    const sg_image image = sg_make_image(&image_desc);

    sg_view_desc view_desc{};
    view_desc.texture.image = image;
    tile = sg_make_view(&view_desc);

    sg_sampler_desc sampler_desc{};
    sampler_desc.min_filter = SG_FILTER_NEAREST;
    sampler_desc.mag_filter = SG_FILTER_NEAREST;
    sampler_desc.wrap_u = SG_WRAP_REPEAT;
    sampler_desc.wrap_v = SG_WRAP_REPEAT;
    sampler_desc.label = "noise-checker";
    sampler = sg_make_sampler(&sampler_desc);
}

void Checker::discard() {
    if (sampler.id != SG_INVALID_ID) {
        sg_destroy_sampler(sampler);
        sampler = sg_sampler{SG_INVALID_ID};
    }
    if (tile.id != SG_INVALID_ID) {
        sg_destroy_view(tile);
        tile = sg_view{SG_INVALID_ID};
    }
}

}
