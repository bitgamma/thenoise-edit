// SPDX-License-Identifier: Apache-2.0
#include "ai/colour_match.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>

namespace noise::ai {
namespace {

// The statistics are taken in CIE Lab, not in sRGB: a drift an edit puts on a frame is a change of
// tone and lightness, and in sRGB those are the same number. The transfer functions and the matrices
// are sRGB/ITU-R BT.709 with the D65 white point, which is what the bytes in an Image mean.
constexpr double kWhiteX = 0.95047;
constexpr double kWhiteZ = 1.08883;
constexpr double kLinearKnot = 0.008856451679;  // (6/29)^3
constexpr double kLinearScale = 7.787037037;    // 1 / (3 * (6/29)^2)
constexpr double kLinearOffset = 0.137931034;   // 4 / 29
constexpr std::size_t kLabChannels = 3;

double to_linear(std::uint8_t channel) {
    const double unit = static_cast<double>(channel) / 255.0;
    return unit <= 0.04045 ? unit / 12.92 : std::pow((unit + 0.055) / 1.055, 2.4);
}

std::uint8_t from_linear(double linear) {
    const double clamped = std::clamp(linear, 0.0, 1.0);
    const double unit = clamped <= 0.0031308 ? clamped * 12.92 : 1.055 * std::pow(clamped, 1.0 / 2.4) - 0.055;
    return static_cast<std::uint8_t>(std::lround(unit * 255.0));
}

double lab_curve(double t) {
    return t > kLinearKnot ? std::cbrt(t) : t * kLinearScale + kLinearOffset;
}

double lab_curve_inverse(double f) {
    return f * f * f > kLinearKnot ? f * f * f : (f - kLinearOffset) / kLinearScale;
}

std::array<double, kLabChannels> lab_of(const std::uint8_t* pixel) {
    const double r = to_linear(pixel[0]);
    const double g = to_linear(pixel[1]);
    const double b = to_linear(pixel[2]);
    const double x = (0.4124564 * r + 0.3575761 * g + 0.1804375 * b) / kWhiteX;
    const double y = 0.2126729 * r + 0.7151522 * g + 0.0721750 * b;
    const double z = (0.0193339 * r + 0.1191920 * g + 0.9503041 * b) / kWhiteZ;
    const double fx = lab_curve(x);
    const double fy = lab_curve(y);
    const double fz = lab_curve(z);
    return {116.0 * fy - 16.0, 500.0 * (fx - fy), 200.0 * (fy - fz)};
}

void rgb_of(const std::array<double, kLabChannels>& lab, std::uint8_t* pixel) {
    const double fy = (lab[0] + 16.0) / 116.0;
    const double fx = fy + lab[1] / 500.0;
    const double fz = fy - lab[2] / 200.0;
    const double x = lab_curve_inverse(fx) * kWhiteX;
    const double y = lab_curve_inverse(fy);
    const double z = lab_curve_inverse(fz) * kWhiteZ;
    pixel[0] = from_linear(3.2404542 * x - 1.5371385 * y - 0.4985314 * z);
    pixel[1] = from_linear(-0.9692660 * x + 1.8760108 * y + 0.0415560 * z);
    pixel[2] = from_linear(0.0556434 * x - 0.2040259 * y + 1.0572252 * z);
}

// Mean and standard deviation in one pass: the Lab values are bounded well enough that the sum of
// squares keeps more precision than a byte, which is what a two-pass mean would buy.
struct Spread {
    std::size_t pixels = 0;
    std::array<double, kLabChannels> sum{};
    std::array<double, kLabChannels> squares{};

    void add(const std::array<double, kLabChannels>& lab) {
        ++pixels;
        for (std::size_t c = 0; c < kLabChannels; ++c) {
            sum[c] += lab[c];
            squares[c] += lab[c] * lab[c];
        }
    }

    std::array<double, kLabChannels> mean() const {
        std::array<double, kLabChannels> out{};
        if (pixels == 0) {
            return out;
        }
        for (std::size_t c = 0; c < kLabChannels; ++c) {
            out[c] = sum[c] / static_cast<double>(pixels);
        }
        return out;
    }

    std::array<double, kLabChannels> deviation() const {
        std::array<double, kLabChannels> out{};
        if (pixels < 2) {
            return out;
        }
        const double n = static_cast<double>(pixels);
        for (std::size_t c = 0; c < kLabChannels; ++c) {
            const double m = sum[c] / n;
            const double variance = squares[c] / n - m * m;
            out[c] = variance > 0.0 ? std::sqrt(variance) : 0.0;
        }
        return out;
    }
};

}  // namespace

bool colour_match_ring(Image& patch, const Image& original, const Mask& reach) {
    if (patch.empty() || patch.size() != original.size() || !reach.sized()) {
        return false;
    }

    Spread ring;
    Spread painted;
    for (int y = 0; y < patch.size().h; ++y) {
        for (int x = 0; x < patch.size().w; ++x) {
            // A pixel with no alpha holds no colour. Counting one would pull the mean towards black
            // and darken the paste for no reason the picture can show.
            if (original.at(x, y)[3] == 0) {
                if (reach.coverage(x, y) > 0 && patch.at(x, y)[3] > 0) {
                    painted.add(lab_of(patch.at(x, y)));
                }
                continue;
            }
            if (reach.coverage(x, y) > 0) {
                painted.add(lab_of(patch.at(x, y)));
            } else {
                ring.add(lab_of(original.at(x, y)));
            }
        }
    }
    if (ring.pixels < 2 || painted.pixels < 2) {
        return false;
    }

    const std::array<double, kLabChannels> from = painted.mean();
    const std::array<double, kLabChannels> to = ring.mean();
    const std::array<double, kLabChannels> from_spread = painted.deviation();
    const std::array<double, kLabChannels> to_spread = ring.deviation();

    std::array<double, kLabChannels> ratio{1.0, 1.0, 1.0};
    bool corrected = false;
    for (std::size_t c = 0; c < kLabChannels; ++c) {
        if (std::abs(to[c] - from[c]) > 1e-9) {
            corrected = true;
        }
        if (from_spread[c] > kMinLabDeviation && to_spread[c] > kMinLabDeviation) {
            ratio[c] = std::clamp(to_spread[c] / from_spread[c], 1.0 / kMaxDeviationRatio,
                                  kMaxDeviationRatio);
            if (ratio[c] != 1.0) {
                corrected = true;
            }
        }
    }
    if (!corrected) {
        return false;
    }

    for (int y = 0; y < patch.size().h; ++y) {
        for (int x = 0; x < patch.size().w; ++x) {
            std::uint8_t* pixel = patch.at(x, y);
            if (pixel[3] == 0) {
                continue;  // no colour here to correct
            }
            const std::array<double, kLabChannels> lab = lab_of(pixel);
            std::array<double, kLabChannels> moved{};
            for (std::size_t c = 0; c < kLabChannels; ++c) {
                moved[c] = (lab[c] - from[c]) * ratio[c] + to[c];
            }
            rgb_of(moved, pixel);
        }
    }
    return true;
}

}  // namespace noise::ai
