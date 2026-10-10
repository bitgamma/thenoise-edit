// SPDX-License-Identifier: Apache-2.0
#include "core/resample.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <vector>

namespace noise {
namespace {

constexpr double kLanczosRadius = 3.0;
constexpr double kPi = 3.14159265358979323846;

double sinc(double x) {
    if (x == 0.0) {
        return 1.0;
    }
    const double px = kPi * x;
    return std::sin(px) / px;
}

double lanczos3(double x) {
    if (x == 0.0) {
        return 1.0;
    }
    if (x <= -kLanczosRadius || x >= kLanczosRadius) {
        return 0.0;
    }
    return sinc(x) * sinc(x / kLanczosRadius);
}

// The taps one axis contributes to each output index. `taps` is the same for every index so the
// weights are a flat table, and the window slides instead of changing size.
struct Axis {
    int taps = 0;
    std::vector<int> from{};      // one per output index, clamped into the source
    std::vector<float> weights{};  // out_len * taps, each row summing to 1
};

Axis build_axis(int in_len, int out_len) {
    Axis axis;
    const double scale = static_cast<double>(in_len) / static_cast<double>(out_len);
    // Widening the support when reducing is the whole difference between a resample and an alias:
    // a source sampled more coarsely than its own detail has to be averaged over the whole span.
    const double stretch = std::max(1.0, scale);
    const double width = kLanczosRadius * stretch;
    axis.taps = std::min(in_len, static_cast<int>(std::ceil(2.0 * width)) + 1);
    axis.from.resize(static_cast<std::size_t>(out_len));
    axis.weights.resize(static_cast<std::size_t>(out_len) * static_cast<std::size_t>(axis.taps));
    for (int out = 0; out < out_len; ++out) {
        const double centre = (static_cast<double>(out) + 0.5) * scale - 0.5;
        const int start =
            std::clamp(static_cast<int>(std::ceil(centre - width)), 0, in_len - axis.taps);
        axis.from[static_cast<std::size_t>(out)] = start;
        double sum = 0.0;
        for (int t = 0; t < axis.taps; ++t) {
            const double w =
                lanczos3((static_cast<double>(start + t) - centre) / stretch);
            axis.weights[static_cast<std::size_t>(out) * static_cast<std::size_t>(axis.taps) +
                         static_cast<std::size_t>(t)] = static_cast<float>(w);
            sum += w;
        }
        // A window truncated by the border still has to add up to one, or a resample would darken
        // the edge of every picture it touches.
        if (sum <= 0.0) {
            const int nearest = std::clamp(static_cast<int>(std::lround(centre)), 0, in_len - 1);
            for (int t = 0; t < axis.taps; ++t) {
                axis.weights[static_cast<std::size_t>(out) * static_cast<std::size_t>(axis.taps) +
                             static_cast<std::size_t>(t)] =
                    start + t == nearest ? 1.0f : 0.0f;
            }
        } else if (sum != 1.0) {
            for (int t = 0; t < axis.taps; ++t) {
                axis.weights[static_cast<std::size_t>(out) * static_cast<std::size_t>(axis.taps) +
                             static_cast<std::size_t>(t)] /= static_cast<float>(sum);
            }
        }
    }
    return axis;
}

// Premultiplied samples, row-major, one float per channel: the filter is linear and has to be, so
// the values it mixes have to be the ones that add up. Float rather than double because the plane is
// crop-sized and 32 bytes a pixel is a different order of allocation than 16.
std::vector<float> premultiply(const Image& src) {
    std::vector<float> plane(static_cast<std::size_t>(src.size().w) *
                             static_cast<std::size_t>(src.size().h) * kChannels);
    for (int y = 0; y < src.size().h; ++y) {
        for (int x = 0; x < src.size().w; ++x) {
            const std::uint8_t* p = src.at(x, y);
            const double amount = static_cast<double>(p[3]) / 255.0;
            float* to = plane.data() + (static_cast<std::size_t>(y) *
                                            static_cast<std::size_t>(src.size().w) +
                                        static_cast<std::size_t>(x)) *
                                           kChannels;
            for (int c = 0; c < 3; ++c) {
                to[static_cast<std::size_t>(c)] = static_cast<float>(static_cast<double>(p[c]) * amount);
            }
            to[3] = static_cast<float>(p[3]);
        }
    }
    return plane;
}

std::uint8_t byte_from_unit(double value) {
    return static_cast<std::uint8_t>(std::clamp(std::lround(value), 0L, 255L));
}

}  // namespace

Image resample_lanczos3(const Image& src, Extent size) {
    if (src.empty() || size.w <= 0 || size.h <= 0) {
        return Image{};
    }
    const Axis horizontal = build_axis(src.size().w, size.w);
    const Axis vertical = build_axis(src.size().h, size.h);
    const std::size_t taps_x = static_cast<std::size_t>(horizontal.taps);

    // Across first: the intermediate is source-tall and destination-wide, and the pass down writes
    // straight into the result, so the plane of floats is the only large buffer beyond the images.
    std::vector<float> wide = premultiply(src);
    std::vector<float> across(static_cast<std::size_t>(src.size().h) *
                              static_cast<std::size_t>(size.w) * kChannels);
    for (int y = 0; y < src.size().h; ++y) {
        const std::size_t row = static_cast<std::size_t>(y) * static_cast<std::size_t>(src.size().w);
        for (int x = 0; x < size.w; ++x) {
            const std::size_t w = static_cast<std::size_t>(x) * taps_x;
            const float* weight = horizontal.weights.data() + w;
            const int from = horizontal.from[static_cast<std::size_t>(x)];
            float* to = across.data() +
                        (static_cast<std::size_t>(y) * static_cast<std::size_t>(size.w) +
                         static_cast<std::size_t>(x)) *
                            kChannels;
            for (int t = 0; t < horizontal.taps; ++t) {
                const float* sample = wide.data() +
                                      (row + static_cast<std::size_t>(from + t)) * kChannels;
                for (int c = 0; c < kChannels; ++c) {
                    to[static_cast<std::size_t>(c)] +=
                        weight[static_cast<std::size_t>(t)] * sample[static_cast<std::size_t>(c)];
                }
            }
        }
    }

    Image out = Image::filled(size, 0, 0, 0, 0);
    const std::size_t stride = static_cast<std::size_t>(size.w) * kChannels;
    for (int y = 0; y < size.h; ++y) {
        const int from = vertical.from[static_cast<std::size_t>(y)];
        const float* weight = vertical.weights.data() +
                              static_cast<std::size_t>(y) * static_cast<std::size_t>(vertical.taps);
        for (int x = 0; x < size.w; ++x) {
            float mixed[kChannels] = {0.0f, 0.0f, 0.0f, 0.0f};
            for (int t = 0; t < vertical.taps; ++t) {
                const float* sample =
                    across.data() +
                    (static_cast<std::size_t>(from + t)) * stride +
                    static_cast<std::size_t>(x) * kChannels;
                for (int c = 0; c < kChannels; ++c) {
                    mixed[c] += weight[static_cast<std::size_t>(t)] * sample[static_cast<std::size_t>(c)];
                }
            }
            std::uint8_t* to = out.at(x, y);
            to[3] = byte_from_unit(static_cast<double>(mixed[3]));
            if (to[3] == 0) {
                // No amount left means no colour to carry, and dividing by what the filter left
                // behind would invent one: weights that are arithmetic noise against a transparent
                // pixel still multiply the neighbours' colours. The divisor below is the alpha that
                // is actually stored, so a pixel is never self-inconsistent by a step.
                std::memcpy(to, kEmptyPixel, kChannels);
                continue;
            }
            const double amount = static_cast<double>(to[3]) / 255.0;
            for (int c = 0; c < 3; ++c) {
                to[c] = byte_from_unit(static_cast<double>(mixed[c]) / amount);
            }
        }
    }
    return out;
}

}  // namespace noise
