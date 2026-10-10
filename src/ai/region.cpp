// SPDX-License-Identifier: Apache-2.0
#include "ai/region.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <string>
#include <vector>

namespace noise::ai {

namespace {

constexpr int kChannels = 4;
// A prep blur is sized off the region, not off the picture: identity has to go at every region size.
constexpr int kBlurDivisor = 12;
constexpr int kMinBlurRadiusPx = 1;
// A block of one radius across: coarse enough that a block is a colour, fine enough that the layout
// of light survives it.
constexpr int kPixelateDivisor = 1;
// Deviation, in 8-bit steps, that `noise` puts on the region's mean colour. Enough to destroy any
// shape, little enough that the mean and the palette the model has to continue are still the ones the
// picture had.
constexpr int kNoiseAmplitude = 12;
// Three box passes approximate the gaussian a recipe asks for. A real gaussian kernel is a refinement
// for the prep ablation to ask for, not a guess to make now.
constexpr int kBlurPasses = 3;

int round_up_to(int value, int multiple) {
    if (multiple <= 1 || value <= 0) {
        return value;
    }
    return ((value + multiple - 1) / multiple) * multiple;
}

bool is_multiple_of(int value, int multiple) {
    return multiple <= 1 || value % multiple == 0;
}

// Grown for context, kept inside the canvas, and snapped on both axes where the canvas is long
// enough to allow it. Snapping happens before the clamp is honoured rather than after, because a
// crop that hangs off the edge is a crop the caller cannot fill: the shift back inside the canvas is
// what keeps `crop` a rectangle of real pixels.
Rect grown_crop(Rect bounds, Extent canvas, const RegionPolicy& policy) {
    const int grow_x = std::max(static_cast<int>(std::lround(bounds.w * policy.context_ratio)),
                                policy.min_context_px);
    const int grow_y = std::max(static_cast<int>(std::lround(bounds.h * policy.context_ratio)),
                                policy.min_context_px);
    Rect grown{bounds.x - grow_x, bounds.y - grow_y, bounds.w + 2 * grow_x,
               bounds.h + 2 * grow_y};
    grown.w = round_up_to(grown.w, policy.size_multiple);
    grown.h = round_up_to(grown.h, policy.size_multiple);
    if (grown.w >= canvas.w) {
        grown.x = 0;
        grown.w = canvas.w;
    } else {
        grown.x = std::clamp(grown.x, 0, canvas.w - grown.w);
    }
    if (grown.h >= canvas.h) {
        grown.y = 0;
        grown.h = canvas.h;
    } else {
        grown.y = std::clamp(grown.y, 0, canvas.h - grown.h);
    }
    return grown;
}

std::array<double, kChannels> mean_of(const std::vector<int>& plane, int cells) {
    std::array<double, kChannels> mean{};
    if (cells <= 0) {
        return mean;
    }
    for (int c = 0; c < kChannels; ++c) {
        double sum = 0.0;
        for (int i = c; i < cells * kChannels; i += kChannels) {
            sum += static_cast<double>(plane[static_cast<std::size_t>(i)]);
        }
        mean[static_cast<std::size_t>(c)] = sum / static_cast<double>(cells);
    }
    return mean;
}

// One box pass along one axis, integer accumulation, edges clamped rather than wrapped: a wrap would
// pull the far side of the crop into the frame the model is being asked to continue.
void box_pass(const std::vector<int>& src, std::vector<int>& dst, int w, int h, int radius,
              bool horizontal) {
    const int span = 2 * radius + 1;
    for (int fixed = 0; fixed < (horizontal ? h : w); ++fixed) {
        for (int cell = 0; cell < (horizontal ? w : h); ++cell) {
            std::array<int, kChannels> sum{};
            for (int offset = -radius; offset <= radius; ++offset) {
                const int other = std::clamp(cell + offset, 0, (horizontal ? w : h) - 1);
                const int x = horizontal ? other : fixed;
                const int y = horizontal ? fixed : other;
                const std::size_t at =
                    (static_cast<std::size_t>(y) * static_cast<std::size_t>(w) +
                     static_cast<std::size_t>(x)) *
                    static_cast<std::size_t>(kChannels);
                for (int c = 0; c < kChannels; ++c) {
                    sum[static_cast<std::size_t>(c)] += src[at + static_cast<std::size_t>(c)];
                }
            }
            const std::size_t to =
                (static_cast<std::size_t>(horizontal ? fixed : cell) *
                     static_cast<std::size_t>(w) +
                 static_cast<std::size_t>(horizontal ? cell : fixed)) *
                static_cast<std::size_t>(kChannels);
            for (int c = 0; c < kChannels; ++c) {
                dst[to + static_cast<std::size_t>(c)] =
                    (sum[static_cast<std::size_t>(c)] + span / 2) / span;
            }
        }
    }
}

// Deterministic, seeded, and nothing to do with the clock: the same seed must give the same crop, or
// the request cache stops meaning anything.
std::uint32_t next_noise(std::uint32_t& state) {
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    return state;
}

}  // namespace

const char* prep_name(Prep prep) {
    switch (prep) {
        case Prep::none:
            return "none";
        case Prep::average:
            return "average";
        case Prep::blur:
            return "blur";
        case Prep::pixelate:
            return "pixelate";
        case Prep::noise:
            return "noise";
        case Prep::white:
            return "white";
    }
    return "?";
}

std::optional<Prep> prep_from_name(const std::string& name) {
    const std::array<Prep, 6> all{Prep::none, Prep::average, Prep::blur,
                                  Prep::pixelate, Prep::noise, Prep::white};
    for (const Prep prep : all) {
        if (name == prep_name(prep)) {
            return prep;
        }
    }
    return std::nullopt;
}

std::string prep_names() {
    std::string names;
    for (const Prep prep : {Prep::none, Prep::average, Prep::blur, Prep::pixelate, Prep::noise,
                            Prep::white}) {
        if (!names.empty()) {
            names += ", ";
        }
        names += prep_name(prep);
    }
    return names;
}

Vec2 RegionPlan::to_document(Vec2 request_point) const {
    const double inverse = scale > 0.0 ? 1.0 / scale : 1.0;
    return Vec2{static_cast<double>(crop.x) + request_point.x * inverse,
                static_cast<double>(crop.y) + request_point.y * inverse};
}

Vec2 RegionPlan::to_request(Vec2 document_point) const {
    return Vec2{(document_point.x - static_cast<double>(crop.x)) * scale,
                (document_point.y - static_cast<double>(crop.y)) * scale};
}

std::optional<RegionPlan> plan_region(Extent canvas, const Mask& mask, const RegionPolicy& policy) {
    if (canvas.w <= 0 || canvas.h <= 0 || mask.empty() || mask.bounds().empty()) {
        return std::nullopt;
    }
    RegionPlan plan;
    plan.mask_bounds = mask.bounds();
    plan.crop = grown_crop(plan.mask_bounds, canvas, policy);
    const double area = static_cast<double>(plan.crop.w) * static_cast<double>(plan.crop.h);
    if (area > static_cast<double>(policy.max_area_px)) {
        plan.scale = std::sqrt(static_cast<double>(policy.max_area_px) / area);
    }
    const int wanted_w = static_cast<int>(std::ceil(static_cast<double>(plan.crop.w) * plan.scale));
    const int wanted_h = static_cast<int>(std::ceil(static_cast<double>(plan.crop.h) * plan.scale));
    plan.requested = Extent{round_up_to(wanted_w, policy.size_multiple),
                            round_up_to(wanted_h, policy.size_multiple)};
    plan.snapped_width = is_multiple_of(plan.crop.w, policy.size_multiple);
    plan.snapped_height = is_multiple_of(plan.crop.h, policy.size_multiple);
    return plan;
}

int prep_blur_radius(Rect crop) {
    const int short_side = std::min(crop.w, crop.h);
    return std::max(kMinBlurRadiusPx, short_side / kBlurDivisor);
}

Image prep_region(const Image& base, const RegionPlan& plan, Prep prep, std::uint64_t seed) {
    const Rect crop = plan.crop;
    if (crop.empty() || crop.w > base.size().w || crop.h > base.size().h) {
        return Image{};
    }

    std::vector<int> plane(static_cast<std::size_t>(crop.w) * static_cast<std::size_t>(crop.h) *
                           static_cast<std::size_t>(kChannels));
    for (int y = 0; y < crop.h; ++y) {
        for (int x = 0; x < crop.w; ++x) {
            const std::uint8_t* from = base.at(crop.x + x, crop.y + y);
            const std::size_t to =
                (static_cast<std::size_t>(y) * static_cast<std::size_t>(crop.w) +
                 static_cast<std::size_t>(x)) *
                static_cast<std::size_t>(kChannels);
            for (int c = 0; c < kChannels; ++c) {
                plane[to + static_cast<std::size_t>(c)] =
                    static_cast<int>(from[c]);
            }
        }
    }

    if (prep == Prep::blur) {
        const int radius = prep_blur_radius(crop);
        std::vector<int> scratch(plane.size());
        for (int pass = 0; pass < kBlurPasses; ++pass) {
            box_pass(plane, scratch, crop.w, crop.h, radius, pass % 2 == 0);
            plane = scratch;
        }
    } else if (prep == Prep::average) {
        const auto mean = mean_of(plane, crop.w * crop.h);
        for (int i = 0; i < crop.w * crop.h; ++i) {
            for (int c = 0; c < kChannels; ++c) {
                plane[static_cast<std::size_t>(i * kChannels + c)] =
                    static_cast<int>(std::lround(mean[static_cast<std::size_t>(c)]));
            }
        }
    } else if (prep == Prep::pixelate) {
        const int block = std::max(1, prep_blur_radius(crop) / kPixelateDivisor);
        for (int by = 0; by < crop.h; by += block) {
            for (int bx = 0; bx < crop.w; bx += block) {
                const int bw = std::min(block, crop.w - bx);
                const int bh = std::min(block, crop.h - by);
                std::vector<int> cells;
                cells.reserve(static_cast<std::size_t>(bw) * static_cast<std::size_t>(bh) *
                              static_cast<std::size_t>(kChannels));
                for (int y = by; y < by + bh; ++y) {
                    for (int x = bx; x < bx + bw; ++x) {
                        const std::size_t at =
                            (static_cast<std::size_t>(y) * static_cast<std::size_t>(crop.w) +
                             static_cast<std::size_t>(x)) *
                            static_cast<std::size_t>(kChannels);
                        for (int c = 0; c < kChannels; ++c) {
                            cells.push_back(plane[at + static_cast<std::size_t>(c)]);
                        }
                    }
                }
                const auto block_mean = mean_of(cells, bw * bh);
                for (int y = by; y < by + bh; ++y) {
                    for (int x = bx; x < bx + bw; ++x) {
                        const std::size_t at =
                            (static_cast<std::size_t>(y) * static_cast<std::size_t>(crop.w) +
                             static_cast<std::size_t>(x)) *
                            static_cast<std::size_t>(kChannels);
                        for (int c = 0; c < kChannels; ++c) {
                            plane[at + static_cast<std::size_t>(c)] =
                                static_cast<int>(std::lround(block_mean[static_cast<std::size_t>(c)]));
                        }
                    }
                }
            }
        }
    } else if (prep == Prep::noise) {
        const auto mean = mean_of(plane, crop.w * crop.h);
        std::uint32_t state = seed == 0 ? 0x9E3779B9u : static_cast<std::uint32_t>(seed);
        for (int i = 0; i < crop.w * crop.h; ++i) {
            for (int c = 0; c < kChannels; ++c) {
                const std::uint32_t draw = next_noise(state);
                const int offset =
                    static_cast<int>(draw % static_cast<std::uint32_t>(2 * kNoiseAmplitude + 1)) -
                    kNoiseAmplitude;
                const int value = static_cast<int>(
                                      std::lround(mean[static_cast<std::size_t>(c)])) +
                                  offset;
                plane[static_cast<std::size_t>(i * kChannels + c)] = std::clamp(value, 0, 255);
            }
        }
    } else if (prep == Prep::white) {
        for (int i = 0; i < crop.w * crop.h; ++i) {
            for (int c = 0; c < kChannels; ++c) {
                plane[static_cast<std::size_t>(i * kChannels + c)] = 255;
            }
        }
    }

    Image out = Image::filled(Extent{crop.w, crop.h}, 0, 0, 0, 0);
    for (int y = 0; y < crop.h; ++y) {
        for (int x = 0; x < crop.w; ++x) {
            const std::size_t at =
                (static_cast<std::size_t>(y) * static_cast<std::size_t>(crop.w) +
                 static_cast<std::size_t>(x)) *
                static_cast<std::size_t>(kChannels);
            std::uint8_t* to = out.at(x, y);
            for (int c = 0; c < kChannels; ++c) {
                to[c] = static_cast<std::uint8_t>(
                    std::clamp(plane[at + static_cast<std::size_t>(c)], 0, 255));
            }
        }
    }
    return out;
}

}  // namespace noise::ai
