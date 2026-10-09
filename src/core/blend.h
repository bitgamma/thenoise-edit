// SPDX-License-Identifier: Apache-2.0
#pragma once

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

// How a layer's pixels combine with what is under them. One row today, and the row carries the
// name that mode has in a `.noise` manifest, in a command argument and in an error - so a mode
// is added here and nowhere else, and a file naming a mode this build does not have is refused
// by name instead of being opened as `normal`.
enum class BlendMode {
    normal,
};

struct BlendModeInfo {
    BlendMode mode;
    const char* name;
};

inline constexpr BlendModeInfo kBlendModes[]{
    {BlendMode::normal, "normal"},
};

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

// One pixel of `dst` with a source of `src_rgb` at `src_alpha`. `src_alpha` is a double rather
// than a byte because a brush coverage value is not quantised until it lands.
void blend_over(std::uint8_t* dst, const std::uint8_t* src_rgb, double src_alpha);

// A whole layer over `dst` inside `region`, and the only image-level "over": a second entry
// point would be a second thing for a shader pair to be checked against. `opacity` scales the
// layer's own alpha before the blend, which is what a layer's 50 % means - a brush's opacity
// belongs to a stroke and is applied where the dabs are composited, not here. A repair costs
// what a region costs, so `region` is the whole canvas when nothing is known correct.
void blend_layer(Image& dst, const Image& src, Rect region, BlendMode mode, double opacity);

// The eraser: it only scales the destination alpha by `1 - amount`. A pixel that reaches zero
// alpha has its colour zeroed too, so "nothing here" has exactly one representation.
void blend_erase(std::uint8_t* dst, double amount);

}  // namespace noise
