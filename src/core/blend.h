// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>

#include "core/image.h"

namespace noise {

// The one reference for how pixels combine: straight (non-premultiplied) 8-bit RGBA, and the
// arithmetic a GPU pair has to be checked against when the GPU starts blending. Everything
// that changes a pixel from a coverage value lands here, so the brush and the compositor
// cannot drift apart.
//
//   a_out = a_s + a_d * (1 - a_s)
//   c_out = (c_s * a_s + c_d * a_d * (1 - a_s)) / a_out

// 0..1 to an 8-bit channel, rounded and clamped: the one place that quantises a blend result.
std::uint8_t channel_from_unit(double unit);

// One pixel of `dst` with a source of `src_rgb` at `src_alpha`. `src_alpha` is a double rather
// than a byte because a brush coverage value is not quantised until it lands.
void blend_over(std::uint8_t* dst, const std::uint8_t* src_rgb, double src_alpha);

// Every pixel of `src` over `dst`; the two images are the same size, which is what a document's
// layers always are.
void blend_over(Image& dst, const Image& src);

// The same, restricted to `region`: what a composite costs when only part of a stack changed.
void blend_over(Image& dst, const Image& src, Rect region);

// The eraser: it only scales the destination alpha by `1 - amount`. A pixel that reaches zero
// alpha has its colour zeroed too, so "nothing here" has exactly one representation.
void blend_erase(std::uint8_t* dst, double amount);

}  // namespace noise
