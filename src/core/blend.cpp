// SPDX-License-Identifier: Apache-2.0
#include "core/blend.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace noise {

std::uint8_t channel_from_unit(double unit) {
    return static_cast<std::uint8_t>(std::lround(std::clamp(unit, 0.0, 1.0) * 255.0));
}

std::string blend_mode_names() {
    std::string names;
    for (const BlendModeInfo& info : kBlendModes) {
        if (!names.empty()) {
            names += ", ";
        }
        names += info.name;
    }
    return names;
}

double blend_separable(BlendMode mode, double backdrop, double source) {
    switch (mode) {
        case BlendMode::normal:
            return source;
        case BlendMode::multiply:
            return backdrop * source;
        case BlendMode::screen:
            return backdrop + source - backdrop * source;
        case BlendMode::overlay:
            // Hard light with the operands the other way round, which is what distinguishes
            // overlay from hard-light: the backdrop decides which half of the range is in play.
            return backdrop <= 0.5 ? 2.0 * backdrop * source
                                   : 1.0 - 2.0 * (1.0 - backdrop) * (1.0 - source);
        case BlendMode::darken:
            return backdrop < source ? backdrop : source;
        case BlendMode::lighten:
            return backdrop > source ? backdrop : source;
        // The end-of-range cases come first and in the specification's order, because they
        // disagree with each other at the corners: dodge(B, 1) is white but dodge(0, 1) is black,
        // and burn(1, C) is white but burn(B, 0) is black. Swapping the two tests changes those two
        // corners and nothing else, which is exactly the kind of thing `test_blend` pins.
        case BlendMode::color_dodge:
            if (backdrop <= 0.0) {
                return 0.0;
            }
            if (source >= 1.0) {
                return 1.0;
            }
            return std::min(1.0, backdrop / (1.0 - source));
        case BlendMode::color_burn:
            if (backdrop >= 1.0) {
                return 1.0;
            }
            if (source <= 0.0) {
                return 0.0;
            }
            return 1.0 - std::min(1.0, (1.0 - backdrop) / source);
    }
    // No `default`, so adding a row to `BlendMode` breaks the build here rather than quietly
    // blending one mode as another. Reaching this line means a value outside the enum, which a
    // caller cannot construct: the answer to that is a dead process, not a plausible colour.
    std::abort();
}

namespace {

// Source-over compositing with a separable blend inside it - PDF 1.4's and CSS Compositing's
// formula, over straight alpha:
//
//   Co = (1-ab)·as·Cs + as·ab·B + (1-as)·ab·Cb,   ao = as + ab·(1-as),   Cout = Co / ao
//
// `blended` is B(Cb, Cs) and `source` is Cs, each channel in 0..1. The two source terms are not
// merged into one `as·Cs` the way the normal-only version used to, because for a mode that is not
// `normal` they are genuinely different quantities: the part of the source that lands on empty
// backdrop cannot be blended with a backdrop that is not there.
//
// Keeping them separate is also what makes a lone layer over an empty canvas come back as itself
// for every mode - ab = 0 collapses Co to as·Cs and the divide undoes it - which is the shortcut
// `Document::composite` takes.
void composite_pixel(std::uint8_t* dst, const double blended[3], const double source[3],
                     double src_alpha) {
    const double dest_alpha = dst[3] / 255.0;
    const double out_alpha = src_alpha + dest_alpha * (1.0 - src_alpha);
    for (int channel = 0; channel < 3; ++channel) {
        const double backdrop = dst[channel] / 255.0;
        const double over = (1.0 - dest_alpha) * src_alpha * source[channel] +
                            src_alpha * dest_alpha * blended[channel] +
                            (1.0 - src_alpha) * dest_alpha * backdrop;
        dst[channel] = channel_from_unit(out_alpha > 0.0 ? over / out_alpha : 0.0);
    }
    dst[3] = channel_from_unit(out_alpha);
}

}  // namespace

void blend_over(std::uint8_t* dst, const std::uint8_t* src_rgb, double src_alpha) {
    if (src_alpha <= 0.0) {
        return;
    }
    // `normal` blends to the source itself, so both operands of the composite are Cs.
    const double source[3] = {src_rgb[0] / 255.0, src_rgb[1] / 255.0, src_rgb[2] / 255.0};
    composite_pixel(dst, source, source, src_alpha);
}

void blend_layer(Image& dst, const Image& src, Rect region, BlendMode mode, double opacity) {
    for (int y = region.y; y < region.bottom(); ++y) {
        for (int x = region.x; x < region.right(); ++x) {
            const std::uint8_t* here = src.at(x, y);
            const double alpha = (here[3] / 255.0) * opacity;
            if (alpha <= 0.0) {
                continue;
            }
            std::uint8_t* onto = dst.at(x, y);
            double blended[3];
            double source[3];
            for (int channel = 0; channel < 3; ++channel) {
                source[channel] = here[channel] / 255.0;
                // The blend reads the backdrop as it stands before this pixel is written.
                blended[channel] = blend_separable(mode, onto[channel] / 255.0, source[channel]);
            }
            composite_pixel(onto, blended, source, alpha);
        }
    }
}

void blend_erase(std::uint8_t* dst, double amount) {
    dst[3] = channel_from_unit((dst[3] / 255.0) * (1.0 - amount));
    if (dst[3] == 0) {
        dst[0] = 0;
        dst[1] = 0;
        dst[2] = 0;
    }
}

}  // namespace noise
