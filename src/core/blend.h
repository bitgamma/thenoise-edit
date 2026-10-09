// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

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

// How a layer's pixels combine with what is under them. The row carries the name that mode has in
// a `.noise` manifest, in a command argument and in an error - so a mode is added here and
// nowhere else, and a file naming a mode this build does not have is refused by name instead of
// being opened as `normal`.
//
// The eight are the MVP set from `docs/MVP.md`: `darken`/`lighten` are the cheapest pair to
// understand and the only ones that never mix channels, `overlay` is what retouchers reach for,
// and `color-dodge`/`color-burn` are here because they are the pair that goes wrong at the ends of
// the range (division by a zero divisor) and are therefore the ones worth having in a table rather
// than discovered later.
enum class BlendMode {
    normal,
    multiply,
    screen,
    overlay,
    darken,
    lighten,
    color_dodge,
    color_burn,
};

struct BlendModeInfo {
    BlendMode mode;
    const char* name;
};

// Names are the W3C/PDF spellings, hyphen included, because they are what every other editor and
// spec calls these and a mode name outlives this codebase.
inline constexpr BlendModeInfo kBlendModes[]{
    {BlendMode::normal, "normal"},
    {BlendMode::multiply, "multiply"},
    {BlendMode::screen, "screen"},
    {BlendMode::overlay, "overlay"},
    {BlendMode::darken, "darken"},
    {BlendMode::lighten, "lighten"},
    {BlendMode::color_dodge, "color-dodge"},
    {BlendMode::color_burn, "color-burn"},
};

inline constexpr std::size_t kBlendModeCount = std::size(kBlendModes);

// "normal" - the mode names as an error message wants them, from the same table.
std::string blend_mode_names();

// Empty for a mode with no row, which no caller can build while the enum has one value.
inline std::string_view blend_mode_name(BlendMode mode) {
    for (const BlendModeInfo& info : kBlendModes) {
        if (info.mode == mode) {
            return info.name;
        }
    }
    return {};
}

// nullopt for a name the table has no row for, which is a file to refuse rather than a mode to
// guess at.
inline std::optional<BlendMode> blend_mode_from_name(const std::string& name) {
    for (const BlendModeInfo& info : kBlendModes) {
        if (name == info.name) {
            return info.mode;
        }
    }
    return std::nullopt;
}

// The mode's arithmetic on one channel, both operands and the result in 0..1, with alpha not
// involved at all: the separable functions of PDF 1.4 / CSS Compositing. This is the definition a
// GPU pair would have to reproduce channel for channel when the compositor moves to the GPU, and
// `test_blend` checks it against the pixel-level result rather than against itself.
//
// `normal` returns the source, which is what lets the compositing rule treat every mode with one
// formula and no special case.
double blend_separable(BlendMode mode, double backdrop, double source);

// One pixel of `dst` with a source of `src_rgb` at `src_alpha`, blended `normal` - what a brush dab
// does, since a blend mode belongs to a layer and not to a stroke. `src_alpha` is a double rather
// than a byte because a brush coverage value is not quantised until it lands.
void blend_over(std::uint8_t* dst, const std::uint8_t* src_rgb, double src_alpha);

// A whole layer over `dst` inside `region`, and the only image-level "over": a second entry point
// would be a second thing for a shader pair to be checked against. `mode` chooses the per-channel
// arithmetic and `opacity` scales the layer's own alpha before it, which is what a layer's 50 %
// means - a brush's opacity belongs to a stroke and is applied where the dabs are composited, not
// here. A repair costs what a region costs, so `region` is the whole canvas when nothing is known
// correct.
void blend_layer(Image& dst, const Image& src, Rect region, BlendMode mode, double opacity);

// The eraser: it only scales the destination alpha by `1 - amount`. A pixel that reaches zero
// alpha has its colour zeroed too, so "nothing here" has exactly one representation.
void blend_erase(std::uint8_t* dst, double amount);

}  // namespace noise
