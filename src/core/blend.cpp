// SPDX-License-Identifier: Apache-2.0
#include "core/blend.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <memory>
#include <vector>

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

std::size_t mode_index(BlendMode mode) {
    for (std::size_t i = 0; i < kBlendModeCount; ++i) {
        if (kBlendModes[i].mode == mode) {
            return i;
        }
    }
    std::abort();
}

// One byte per (backdrop, source) pair: what a mode does to two opaque pixels. With `ab = as = 1`
// the composite below collapses to `B`, so this table is exactly what the doubles compute, entry
// for entry - it is the definition read once, not a second definition, and `test_blend` checks all
// 65536 entries of every mode against `blend_separable`.
using BlendLut = std::array<std::uint8_t, 256 * 256>;

inline std::size_t lut_entry(std::uint8_t backdrop, std::uint8_t source) {
    return static_cast<std::size_t>(backdrop) * 256u + static_cast<std::size_t>(source);
}

const BlendLut& lut_for(BlendMode mode) {
    static const std::array<BlendLut, kBlendModeCount> tables = [] {
        std::array<BlendLut, kBlendModeCount> built{};
        for (const BlendModeInfo& info : kBlendModes) {
            BlendLut& table = built[mode_index(info.mode)];
            for (int backdrop = 0; backdrop < 256; ++backdrop) {
                for (int source = 0; source < 256; ++source) {
                    table[lut_entry(std::uint8_t(backdrop), std::uint8_t(source))] =
                        channel_from_unit(blend_separable(info.mode, backdrop / 255.0, source / 255.0));
                }
            }
        }
        return built;
    }();
    return tables[mode_index(mode)];
}

// The three terms of the composite for one channel, in 0..1, before the divide by the result
// alpha. Split out of the pixel so the byte tables below build from this expression rather than
// beside it - a table that re-derived the formula would be a second definition of blending.
inline double composite_over(double backdrop, double blended, double source, double src_alpha,
                            double dest_alpha) {
    return (1.0 - dest_alpha) * src_alpha * source + src_alpha * dest_alpha * blended +
           (1.0 - src_alpha) * dest_alpha * backdrop;
}

inline std::uint8_t channel_after(double over, double out_alpha) {
    return channel_from_unit(out_alpha > 0.0 ? over / out_alpha : 0.0);
}

inline double alpha_after(double src_alpha, double dest_alpha) {
    return src_alpha + dest_alpha * (1.0 - src_alpha);
}

// How far one call goes: pairs it remembers a count for, and tables it is willing to hold. Tracking
// is a byte and a counter; a table is 64 KB, so the two limits differ deliberately. A source whose
// alpha changes every pixel - a feathered mask, a soft brush edge - presents a new pair per pixel,
// registers at most `kTrackedPairs` of them, and then costs one indexed load and a branch.
inline constexpr std::size_t kTrackedPairs = 64;
inline constexpr std::size_t kTableCap = 4;

// A pair earns its table once the call has done this many pixels of its arithmetic. Building the
// 65536 entries measures 0.48 ms and a general-path pixel 15 ns, so the two sides cross near
// 30 000; 40 000 sits just past the crossing rather than before it, and every pixel after it is a
// load instead of a divide. Debug builds cross earlier, since the arithmetic slows down more than
// the build does.
inline constexpr int kEarnTableAfter = 40000;

// Below this area a call never allocates the cache: a repair of a brush dab is a few thousand
// pixels and would pay for a table it cannot amortise.
inline constexpr std::size_t kCacheAreaFloor = 1u << 16;

struct PairSlot {
    int seen = 0;
    std::uint8_t result_alpha = 0;
    std::unique_ptr<BlendLut> table;
};

// Once the two alphas are fixed, one channel of the composite is a function of two bytes, so a
// partial-strength layer over a backdrop of one alpha is a byte table per (source alpha, backdrop
// alpha) pair. Pairs are indexed directly rather than searched - a list scan costs a measurable
// slice of the general path when every pixel asks a different question - and the table is built
// through the same `composite_over` the pixel loop calls, so it cannot disagree with it.
class PairCache {
public:
    PairCache(BlendMode mode, double opacity) : mode_(mode), opacity_(opacity) {
        slots_.reserve(kTrackedPairs);
        // Slot numbers plus one, so 0 means "this pair has never been seen" without a second array.
        index_.assign(256u * 256u, 0u);
    }
    PairCache(const PairCache&) = delete;
    PairCache& operator=(const PairCache&) = delete;

    // The pair's slot, counting the pixel and building the table once the pair has earned one.
    // Nullptr when the pair is new and no slot is left, which means "do the arithmetic".
    bool holds_a_table() const { return tables_ > 0; }

    PairSlot* slot_for(std::uint8_t src_alpha, std::uint8_t dest_alpha) {
        const std::size_t key = lut_entry(dest_alpha, src_alpha);
        const std::uint16_t stored = index_[key];
        if (stored == 0u) {
            if (slots_.size() == kTrackedPairs) {
                return nullptr;
            }
            index_[key] = static_cast<std::uint16_t>(slots_.size() + 1u);
            slots_.push_back(PairSlot{1, 0, nullptr});
            return &slots_.back();
        }
        PairSlot& slot = slots_[stored - 1u];
        if (slot.table == nullptr && tables_ < kTableCap && ++slot.seen >= kEarnTableAfter) {
            build(slot, src_alpha, dest_alpha);
            ++tables_;
        }
        return &slot;
    }

private:
    void build(PairSlot& slot, std::uint8_t src_byte, std::uint8_t dest_byte) {
        const double src = (src_byte / 255.0) * opacity_;
        const double dest = dest_byte / 255.0;
        const double out_alpha = alpha_after(src, dest);
        slot.result_alpha = channel_from_unit(out_alpha);
        slot.table = std::make_unique<BlendLut>();
        for (int backdrop = 0; backdrop < 256; ++backdrop) {
            for (int source = 0; source < 256; ++source) {
                const double b = backdrop / 255.0;
                const double c = source / 255.0;
                (*slot.table)[lut_entry(std::uint8_t(backdrop), std::uint8_t(source))] =
                    channel_after(
                        composite_over(b, blend_separable(mode_, b, c), c, src, dest), out_alpha);
            }
        }
    }

    BlendMode mode_;
    double opacity_;
    std::vector<std::uint16_t> index_;
    std::vector<PairSlot> slots_;
    std::size_t tables_ = 0;
};

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
    const double out_alpha = alpha_after(src_alpha, dest_alpha);
    for (int channel = 0; channel < 3; ++channel) {
        const double backdrop = dst[channel] / 255.0;
        dst[channel] = channel_after(
            composite_over(backdrop, blended[channel], source[channel], src_alpha, dest_alpha),
            out_alpha);
    }
    dst[3] = channel_from_unit(out_alpha);
}

}  // namespace

void blend_over(std::uint8_t* dst, const std::uint8_t* src_rgb, double src_alpha) {
    if (src_alpha <= 0.0) {
        return;
    }
    if (src_alpha >= 1.0) {
        // `as = 1` cancels the backdrop out of the composite: a solid dab centre lands as itself at
        // full alpha whatever was under it, and a dab's centre is where most of its pixels are.
        for (int channel = 0; channel < 3; ++channel) {
            dst[channel] = src_rgb[channel];
        }
        dst[3] = 255;
        return;
    }
    if (dst[3] == 0) {
        // Nothing under it: `ab = 0` leaves `Co = as·Cs` and `ao = as`, so the divide returns the
        // source colour and only the alpha is a new number. Painting on empty canvas is this path.
        for (int channel = 0; channel < 3; ++channel) {
            dst[channel] = src_rgb[channel];
        }
        dst[3] = channel_from_unit(src_alpha);
        return;
    }
    // `normal` blends to the source itself, so both operands of the composite are Cs.
    const double source[3] = {src_rgb[0] / 255.0, src_rgb[1] / 255.0, src_rgb[2] / 255.0};
    composite_pixel(dst, source, source, src_alpha);
}

void blend_layer(Image& dst, const Image& src, Rect region, BlendMode mode, double opacity) {
    if (opacity <= 0.0) {
        return;
    }
    // A pixel whose alpha is zero is absent, not black, and a real layer is mostly absent: a
    // document-sized layer with a stroke on it is >99 % this branch. Everything below is the cost
    // of the pixels that are there.
    const BlendLut& lut = lut_for(mode);
    const bool full_strength = opacity >= 1.0;
    const bool plain_copy = full_strength && mode == BlendMode::normal;
    std::unique_ptr<PairCache> cache;
    if (area(region) >= kCacheAreaFloor) {
        cache = std::make_unique<PairCache>(mode, opacity);
    }
    for (int y = region.y; y < region.bottom(); ++y) {
        const std::uint8_t* here = src.at(region.x, y);
        std::uint8_t* onto = dst.at(region.x, y);
        for (int column = 0; column < region.w; ++column, here += kChannels, onto += kChannels) {
            const std::uint8_t src_alpha = here[3];
            if (src_alpha == 0) {
                continue;
            }
            if (plain_copy && src_alpha == 255) {
                for (int channel = 0; channel < kChannels; ++channel) {
                    onto[channel] = here[channel];
                }
                continue;
            }
            const double alpha = (src_alpha / 255.0) * opacity;
            if (onto[3] == 0) {
                for (int channel = 0; channel < 3; ++channel) {
                    onto[channel] = here[channel];
                }
                onto[3] = channel_from_unit(alpha);
                continue;
            }
            if (full_strength && src_alpha == 255 && onto[3] == 255) {
                for (int channel = 0; channel < 3; ++channel) {
                    onto[channel] = lut[lut_entry(onto[channel], here[channel])];
                }
                onto[3] = 255;
                continue;
            }
            // Partial strength is the case with no closed form to fall back on, and the one a layer
            // spends most of its life in once the opacity slider moves: it is also the only case the
            // pair table exists for.
            if (cache != nullptr) {
                PairSlot* slot = cache->slot_for(src_alpha, onto[3]);
                if (slot == nullptr && !cache->holds_a_table()) {
                    // A document this varied in coverage gives no pair enough pixels to be worth a
                    // table, and the indexed lookup costs more than it returns. Measured: leaving it
                    // in costs 14 % on a layer whose alpha changes every pixel.
                    cache.reset();
                } else if (slot != nullptr && slot->table != nullptr) {
                    const BlendLut& table = *slot->table;
                    for (int channel = 0; channel < 3; ++channel) {
                        onto[channel] = table[lut_entry(onto[channel], here[channel])];
                    }
                    onto[3] = slot->result_alpha;
                    continue;
                }
            }
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
