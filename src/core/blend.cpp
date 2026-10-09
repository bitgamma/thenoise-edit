// SPDX-License-Identifier: Apache-2.0
#include "core/blend.h"

#include <algorithm>
#include <cmath>

namespace noise {

std::uint8_t channel_from_unit(double unit) {
    return static_cast<std::uint8_t>(std::lround(std::clamp(unit, 0.0, 1.0) * 255.0));
}

void blend_over(std::uint8_t* dst, const std::uint8_t* src_rgb, double src_alpha) {
    if (src_alpha <= 0.0) {
        return;
    }
    const double dest_alpha = dst[3] / 255.0;
    const double kept = 1.0 - src_alpha;
    const double out_alpha = src_alpha + dest_alpha * kept;
    for (int channel = 0; channel < 3; ++channel) {
        dst[channel] = channel_from_unit(((src_rgb[channel] / 255.0) * src_alpha +
                                          (dst[channel] / 255.0) * dest_alpha * kept) /
                                         out_alpha);
    }
    dst[3] = channel_from_unit(out_alpha);
}

void blend_over(Image& dst, const Image& src, Rect region) {
    for (int y = region.y; y < region.bottom(); ++y) {
        for (int x = region.x; x < region.right(); ++x) {
            const std::uint8_t* here = src.at(x, y);
            if (here[3] == 0) {
                continue;
            }
            blend_over(dst.at(x, y), here, here[3] / 255.0);
        }
    }
}

void blend_over(Image& dst, const Image& src) {
    blend_over(dst, src, Rect{0, 0, src.size().w, src.size().h});
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
