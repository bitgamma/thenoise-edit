// SPDX-License-Identifier: Apache-2.0
#include "ai/region.h"

#include "core/blur.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <string>
#include <vector>

namespace noise::ai {

namespace {

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

int round_up_to(int value, int multiple) {
    if (multiple <= 1 || value <= 0) {
        return value;
    }
    return ((value + multiple - 1) / multiple) * multiple;
}

bool is_multiple_of(int value, int multiple) {
    return multiple <= 1 || value % multiple == 0;
}

// Request pixels per document pixel, and the other way round, on one axis. A crop or a request of
// zero on an axis is a plan nobody can compose against, so the map degenerates rather than divides.
double document_per_request_pixel(int request_side, int crop_side) {
    return request_side > 0 ? static_cast<double>(crop_side) / static_cast<double>(request_side) : 0.0;
}

double request_per_document_pixel(int request_side, int crop_side) {
    return crop_side > 0 ? static_cast<double>(request_side) / static_cast<double>(crop_side) : 0.0;
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
    return Vec2{static_cast<double>(crop.x) +
                    request_point.x * document_per_request_pixel(requested.w, crop.w),
                static_cast<double>(crop.y) +
                    request_point.y * document_per_request_pixel(requested.h, crop.h)};
}

Vec2 RegionPlan::to_request(Vec2 document_point) const {
    return Vec2{(document_point.x - static_cast<double>(crop.x)) *
                    request_per_document_pixel(requested.w, crop.w),
                (document_point.y - static_cast<double>(crop.y)) *
                    request_per_document_pixel(requested.h, crop.h)};
}

Rect frame_for_aspect(Extent canvas, AspectRatio ratio) {
    if (canvas.w <= 0 || canvas.h <= 0 || ratio.w <= 0 || ratio.h <= 0) {
        return Rect{};
    }
    // Which axis runs the full canvas, decided without dividing.
    if (static_cast<double>(canvas.w) * ratio.h > static_cast<double>(canvas.h) * ratio.w) {
        const int w = std::max(
            1, static_cast<int>(std::lround(static_cast<double>(canvas.h) * ratio.w / ratio.h)));
        return Rect{(canvas.w - w) / 2, 0, w, canvas.h};
    }
    const int h = std::max(
        1, static_cast<int>(std::lround(static_cast<double>(canvas.w) * ratio.h / ratio.w)));
    return Rect{0, (canvas.h - h) / 2, canvas.w, h};
}

std::optional<RegionPlan> plan_region(Extent canvas, const Mask& mask, const RegionPolicy& policy) {
    if (canvas.w <= 0 || canvas.h <= 0 || mask.empty() || mask.bounds().empty()) {
        return std::nullopt;
    }
    // A policy with no multiple cannot snap anything, and dividing by it would crash rather than refuse.
    // This is what a table that failed to load looks like from here, and a compile is refused instead.
    if (policy.size_multiple <= 0 || policy.max_area_px <= 0) {
        return std::nullopt;
    }
    RegionPlan plan;
    plan.mask_bounds = mask.bounds();
    plan.crop = grown_crop(plan.mask_bounds, canvas, policy);
    // The size asked for is the crop's, kept on the policy grid. The grid is not decoration: an off-grid
    // ask is snapped by the engine anyway (measured: asking 500x300 answers 512x304), so sending the
    // crop's exact size would trade a snap this side planned for one the engine invented, and the answer
    // would stop matching the mapping the crop was compiled against. Grown crops land on the grid already,
    // so normally this asks the crop's own size and nothing is resampled at either end.
    //
    // Shrinking is the other thing, and it happens only above the ceiling - which is the model's own
    // `soft_pixel_cap` when the box publishes one, never a number this app prefers. A scale below 1 means
    // the answer is built from fewer pixels than the region holds and resampled back over it: the same
    // picture, softer. That is what `resampled` reports, because a Change that visibly did nothing is
    // otherwise indistinguishable from a Change that failed.
    const double area = static_cast<double>(plan.crop.w) * static_cast<double>(plan.crop.h);
    if (area > static_cast<double>(policy.max_area_px)) {
        plan.scale = std::sqrt(static_cast<double>(policy.max_area_px) / area);
    }
    const int wanted_w = static_cast<int>(std::ceil(static_cast<double>(plan.crop.w) * plan.scale));
    const int wanted_h = static_cast<int>(std::ceil(static_cast<double>(plan.crop.h) * plan.scale));
    plan.requested = Extent{round_up_to(wanted_w, policy.size_multiple),
                            round_up_to(wanted_h, policy.size_multiple)};
    plan.resampled = plan.scale < 1.0;
    plan.snapped_width = is_multiple_of(plan.crop.w, policy.size_multiple);
    plan.snapped_height = is_multiple_of(plan.crop.h, policy.size_multiple);
    return plan;
}

int prep_blur_radius(Extent crop) {
    return std::max(kMinBlurRadiusPx, std::min(crop.w, crop.h) / kBlurDivisor);
}

Image prep_region(const Image& crop, const Mask& coverage_in_crop, Prep prep,
                  std::uint64_t seed) {
    const Extent size = crop.size();
    if (size.w <= 0 || size.h <= 0) {
        return Image{};
    }
    // No coverage at all means the caller means the whole crop; a sized coverage that does not match the
    // crop is a caller bug, and treating it as "everything" would quietly destroy the wrong pixels.
    const bool whole = !coverage_in_crop.sized();
    if (!whole && !(coverage_in_crop.size() == size)) {
        return Image{};
    }
    const auto in_hole = [&](int x, int y) {
        return whole || coverage_in_crop.coverage(x, y) > 0;
    };

    const std::size_t pixels = static_cast<std::size_t>(size.w) * static_cast<std::size_t>(size.h);
    std::vector<int> plane(pixels * static_cast<std::size_t>(kChannels));
    for (int y = 0; y < size.h; ++y) {
        for (int x = 0; x < size.w; ++x) {
            const std::uint8_t* from = crop.at(x, y);
            const std::size_t to =
                (static_cast<std::size_t>(y) * static_cast<std::size_t>(size.w) +
                 static_cast<std::size_t>(x)) *
                static_cast<std::size_t>(kChannels);
            for (int c = 0; c < kChannels; ++c) {
                plane[to + static_cast<std::size_t>(c)] = static_cast<int>(from[c]);
            }
        }
    }

    // Statistics of the *context*: the hole's own mean is the colour of the thing being got rid of, so
    // filling with it paints the answer with the question.
    std::array<double, kChannels> context_mean{};
    {
        std::array<double, kChannels> sum{};
        std::size_t counted = 0;
        for (int y = 0; y < size.h; ++y) {
            for (int x = 0; x < size.w; ++x) {
                if (in_hole(x, y)) {
                    continue;
                }
                const std::size_t at =
                    (static_cast<std::size_t>(y) * static_cast<std::size_t>(size.w) +
                     static_cast<std::size_t>(x)) *
                    static_cast<std::size_t>(kChannels);
                for (int c = 0; c < kChannels; ++c) {
                    sum[static_cast<std::size_t>(c)] +=
                        static_cast<double>(plane[at + static_cast<std::size_t>(c)]);
                }
                ++counted;
            }
        }
        if (counted == 0) {
            const auto all = mean_of(plane, static_cast<int>(pixels));
            for (int c = 0; c < kChannels; ++c) {
                context_mean[static_cast<std::size_t>(c)] = all[static_cast<std::size_t>(c)];
            }
        } else {
            for (int c = 0; c < kChannels; ++c) {
                context_mean[static_cast<std::size_t>(c)] =
                    sum[static_cast<std::size_t>(c)] / static_cast<double>(counted);
            }
        }
    }

    // The blur and the pixelation are computed over the whole plane so the hole's edge is filled with
    // what actually surrounds it; only the hole is then written back.
    std::vector<int> blurred;
    if (prep == Prep::blur) {
        blurred = plane;
        box_blur(blurred, size, kChannels, prep_blur_radius(size), kGaussianBoxPasses);
    }
    const int block = prep == Prep::pixelate ? std::max(1, prep_blur_radius(size) / kPixelateDivisor)
                                             : 1;
    std::vector<int> blockified;
    if (prep == Prep::pixelate) {
        blockified = plane;
        for (int by = 0; by < size.h; by += block) {
            for (int bx = 0; bx < size.w; bx += block) {
                const int bw = std::min(block, size.w - bx);
                const int bh = std::min(block, size.h - by);
                std::vector<int> cells;
                cells.reserve(static_cast<std::size_t>(bw) * static_cast<std::size_t>(bh) *
                              static_cast<std::size_t>(kChannels));
                for (int y = by; y < by + bh; ++y) {
                    for (int x = bx; x < bx + bw; ++x) {
                        const std::size_t at =
                            (static_cast<std::size_t>(y) * static_cast<std::size_t>(size.w) +
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
                            (static_cast<std::size_t>(y) * static_cast<std::size_t>(size.w) +
                             static_cast<std::size_t>(x)) *
                            static_cast<std::size_t>(kChannels);
                        for (int c = 0; c < kChannels; ++c) {
                            blockified[at + static_cast<std::size_t>(c)] =
                                static_cast<int>(std::lround(block_mean[static_cast<std::size_t>(c)]));
                        }
                    }
                }
            }
        }
    }

    // The draw sequence is tied to the geometry, not to the mask, so the same crop with a different
    // hole does not renumber the noise that lands in it.
    std::uint32_t state = seed == 0 ? 0x9E3779B9u : static_cast<std::uint32_t>(seed);

    Image out = Image::filled(size, 0, 0, 0, 0);
    for (int y = 0; y < size.h; ++y) {
        for (int x = 0; x < size.w; ++x) {
            const std::size_t at =
                (static_cast<std::size_t>(y) * static_cast<std::size_t>(size.w) +
                 static_cast<std::size_t>(x)) *
                static_cast<std::size_t>(kChannels);
            std::uint8_t* to = out.at(x, y);
            // The draw is taken for every pixel, hole or not, so the noise a hole gets depends on the
            // crop's geometry and not on how many pixels the mask happened to cover. Only the hole is
            // written.
            const std::uint32_t draw = next_noise(state);
            if (prep == Prep::noise && in_hole(x, y)) {
                const int offset = static_cast<int>(
                    draw % static_cast<std::uint32_t>(2 * kNoiseAmplitude + 1)) -
                                   kNoiseAmplitude;
                for (int c = 0; c < kChannels; ++c) {
                    const int jittered =
                        static_cast<int>(std::lround(context_mean[static_cast<std::size_t>(c)])) +
                        offset;
                    to[c] = static_cast<std::uint8_t>(std::clamp(jittered, 0, 255));
                }
                continue;
            }
            if (!in_hole(x, y)) {
                for (int c = 0; c < kChannels; ++c) {
                    to[c] = static_cast<std::uint8_t>(
                        std::clamp(plane[at + static_cast<std::size_t>(c)], 0, 255));
                }
                continue;
            }
            for (int c = 0; c < kChannels; ++c) {
                int value = plane[at + static_cast<std::size_t>(c)];
                switch (prep) {
                    case Prep::none:
                        break;
                    case Prep::blur:
                        value = blurred[at + static_cast<std::size_t>(c)];
                        break;
                    case Prep::pixelate:
                        value = blockified[at + static_cast<std::size_t>(c)];
                        break;
                    case Prep::average:
                        value = static_cast<int>(
                            std::lround(context_mean[static_cast<std::size_t>(c)]));
                        break;
                    case Prep::noise:
                        break;  // written above, and still confined to the hole by the check
                    case Prep::white:
                        value = 255;
                        break;
                }
                to[c] = static_cast<std::uint8_t>(std::clamp(value, 0, 255));
            }
        }
    }
    return out;
}


Image flatten_over_ring(const Image& crop, const Mask& coverage_in_crop) {
    if (crop.empty()) {
        return Image{};
    }
    // One pass collects both means: the ring first, everything with alpha as the fallback. Summing in
    // int keeps this exact for 8-bit channels at any crop size.
    long ring_r = 0, ring_g = 0, ring_b = 0, ring_n = 0;
    long all_r = 0, all_g = 0, all_b = 0, all_n = 0;
    for (int y = 0; y < crop.size().h; ++y) {
        for (int x = 0; x < crop.size().w; ++x) {
            const std::uint8_t* pixel = &crop.at(x, y)[0];
            if (pixel[3] == 0) {
                continue;  // no colour to take, whatever the bytes say
            }
            all_r += pixel[0];
            all_g += pixel[1];
            all_b += pixel[2];
            ++all_n;
            if (coverage_in_crop.coverage(x, y) == 0) {
                ring_r += pixel[0];
                ring_g += pixel[1];
                ring_b += pixel[2];
                ++ring_n;
            }
        }
    }
    const bool have_ring = ring_n > 0;
    const long n = have_ring ? ring_n : all_n;
    // Mid grey when there is nothing at all: a fully transparent document has no scene to sample, and
    // 0 or 255 would bias every colour the model then invents.
    const int back_r = n > 0 ? static_cast<int>((have_ring ? ring_r : all_r) / n) : 128;
    const int back_g = n > 0 ? static_cast<int>((have_ring ? ring_g : all_g) / n) : 128;
    const int back_b = n > 0 ? static_cast<int>((have_ring ? ring_b : all_b) / n) : 128;

    Image out = Image::filled(crop.size(), 0, 0, 0, 255);
    for (int y = 0; y < crop.size().h; ++y) {
        for (int x = 0; x < crop.size().w; ++x) {
            const std::uint8_t* pixel = &crop.at(x, y)[0];
            const int alpha = pixel[3];
            std::uint8_t* write = &out.at(x, y)[0];
            write[0] = static_cast<std::uint8_t>((pixel[0] * alpha + back_r * (255 - alpha)) / 255);
            write[1] = static_cast<std::uint8_t>((pixel[1] * alpha + back_g * (255 - alpha)) / 255);
            write[2] = static_cast<std::uint8_t>((pixel[2] * alpha + back_b * (255 - alpha)) / 255);
            write[3] = 255;
        }
    }
    return out;
}

}  // namespace noise::ai
