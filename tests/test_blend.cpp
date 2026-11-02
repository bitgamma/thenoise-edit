// SPDX-License-Identifier: Apache-2.0
#include "core/blend.h"
#include "core/image.h"
#include "test.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <functional>
#include <vector>

namespace noise {
namespace {

// The published definitions, written out again here on purpose: this file is the specification and
// `core/blend.cpp` is the implementation, and they share no helper. Agreement therefore means the
// implementation matches PDF 1.4 / CSS Compositing rather than matching itself - which is also the
// number a GPU pair has to reproduce when the compositor moves to the shader.
double spec_blend(BlendMode mode, double backdrop, double source) {
    switch (mode) {
        case BlendMode::normal:
            return source;
        case BlendMode::multiply:
            return backdrop * source;
        case BlendMode::screen:
            return backdrop + source - backdrop * source;
        case BlendMode::overlay:
            return backdrop <= 0.5 ? 2.0 * backdrop * source
                                   : 1.0 - 2.0 * (1.0 - backdrop) * (1.0 - source);
        case BlendMode::darken:
            return backdrop < source ? backdrop : source;
        case BlendMode::lighten:
            return backdrop > source ? backdrop : source;
        // The two conditions are evaluated in this order by both PDF 1.4 and CSS Compositing, and
        // the corners where they disagree - dodge(0, 1) and burn(1, 0) - are the reason: black
        // stays black under a dodge, white stays white under a burn.
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
    // A mode with no row here is a mode nobody specified, and the fixtures below will report it as
    // a mismatch rather than let it pass.
    return -1.0;
}

// Source-over with a separable blend inside it, in one channel: what `blend_layer` has to produce.
//
// `backdrop_byte` is the same channel as it stands in the destination, because when neither side
// has any coverage there is nothing to divide by and the answer is "that pixel was not painted" -
// the destination's own bytes, whatever colour they happen to hold under zero alpha. Expecting 0
// there would demand that a blend erase, which is `blend_erase`'s job and not this one's.
std::uint8_t spec_channel(std::uint8_t backdrop_byte, double backdrop, double source,
                          double blended, double source_alpha, double backdrop_alpha) {
    const double out_alpha = source_alpha + backdrop_alpha * (1.0 - source_alpha);
    if (out_alpha <= 0.0) {
        return backdrop_byte;
    }
    const double over = (1.0 - backdrop_alpha) * source_alpha * source +
                        source_alpha * backdrop_alpha * blended +
                        (1.0 - source_alpha) * backdrop_alpha * backdrop;
    const double unit = std::clamp(over / out_alpha, 0.0, 1.0);
    return static_cast<std::uint8_t>(std::lround(unit * 255.0));
}

constexpr std::size_t kModeCount = std::size(kBlendModes);

const BlendModeInfo& mode_at(std::size_t row) { return kBlendModes[row]; }

// The fixture: every combination of five backdrop levels x three backdrop alphas x five source
// levels x four source alphas, laid out as rows and columns so one `blend_layer` call covers it.
// The levels are the ends of the range plus the middle and both sides of the 0.5 crossing that
// `overlay` branches on; the alphas include nothing, one, and the two coverage values that a
// feathered brush actually produces.
constexpr int kLevels[]{0, 1, 127, 128, 255};
constexpr int kBackdropAlphas[]{0, 128, 255};
constexpr int kSourceAlphas[]{0, 1, 128, 255};

constexpr int kLevelCount = static_cast<int>(std::size(kLevels));
constexpr int kBackdropAlphaCount = static_cast<int>(std::size(kBackdropAlphas));
constexpr int kSourceAlphaCount = static_cast<int>(std::size(kSourceAlphas));

constexpr int width_of_fixture() { return kLevelCount * kSourceAlphaCount; }
constexpr int height_of_fixture() { return kLevelCount * kBackdropAlphaCount; }

struct Fixture {
    Image backdrop;
    Image source;
    Extent size{width_of_fixture(), height_of_fixture()};
};

Fixture make_fixture() {
    std::vector<std::uint8_t> back(static_cast<std::size_t>(width_of_fixture()) *
                                   static_cast<std::size_t>(height_of_fixture()) * 4u);
    std::vector<std::uint8_t> src = back;
    for (int y = 0; y < height_of_fixture(); ++y) {
        const int backdrop_level = kLevels[y % kLevelCount];
        const int backdrop_alpha = kBackdropAlphas[y / kLevelCount];
        for (int x = 0; x < width_of_fixture(); ++x) {
            const int source_level = kLevels[x % kLevelCount];
            const int source_alpha = kSourceAlphas[x / kLevelCount];
            const std::size_t at = (static_cast<std::size_t>(y) *
                                        static_cast<std::size_t>(width_of_fixture()) +
                                    static_cast<std::size_t>(x)) *
                                   4u;
            // Two hues rather than grey, so a channel that reads the wrong neighbour shows up: a
            // grey fixture passes a channel-swapped implementation.
            back[at + 0] = static_cast<std::uint8_t>(backdrop_level);
            back[at + 1] = static_cast<std::uint8_t>(255 - backdrop_level);
            back[at + 2] = static_cast<std::uint8_t>(backdrop_level / 2);
            back[at + 3] = static_cast<std::uint8_t>(backdrop_alpha);
            src[at + 0] = static_cast<std::uint8_t>(source_level);
            src[at + 1] = static_cast<std::uint8_t>(255 - source_level);
            src[at + 2] = static_cast<std::uint8_t>(255 - source_level / 2);
            src[at + 3] = static_cast<std::uint8_t>(source_alpha);
        }
    }
    Fixture fixture;
    fixture.backdrop = Image::from_rgba(fixture.size, std::move(back));
    fixture.source = Image::from_rgba(fixture.size, std::move(src));
    return fixture;
}

Rect whole(const Extent size) { return Rect{0, 0, size.w, size.h}; }

// Every mode against the definitions, at every combination the fixture holds, colour and alpha.
// A wrong formula, operands the wrong way round (`overlay` is hard-light swapped, and only the
// fixture's asymmetry catches that), a missing end-of-range guard, or a mode that meddles with
// coverage all fail here.
void test_every_mode_matches_the_definition_pixel_for_pixel() {
    const Fixture fixture = make_fixture();
    for (std::size_t row = 0; row < kModeCount; ++row) {
        const BlendModeInfo& info = mode_at(row);
        Image onto = fixture.backdrop;
        blend_layer(onto, fixture.source, whole(fixture.size), info.mode, 1.0);
        for (int y = 0; y < fixture.size.h; ++y) {
            const int backdrop_level = kLevels[y % kLevelCount];
            const double backdrop_alpha = kBackdropAlphas[y / kLevelCount] /
                                          255.0;
            for (int x = 0; x < fixture.size.w; ++x) {
                const int source_level = kLevels[x % kLevelCount];
                const double source_alpha =
                    kSourceAlphas[x / kLevelCount] / 255.0;
                const double backdrop[3] = {backdrop_level / 255.0, (255 - backdrop_level) / 255.0,
                                            (backdrop_level / 2) / 255.0};
                const double source[3] = {source_level / 255.0, (255 - source_level) / 255.0,
                                          (255 - source_level / 2) / 255.0};
                const std::uint8_t* got = onto.at(x, y);
                for (int channel = 0; channel < 3; ++channel) {
                    const std::uint8_t want = spec_channel(
                        fixture.backdrop.at(x, y)[channel], backdrop[channel], source[channel],
                        spec_blend(info.mode, backdrop[channel], source[channel]), source_alpha,
                        backdrop_alpha);
                    if (got[channel] != want) {
                        noise::test::report(
                            "mode matches the definition", __FILE__, __LINE__,
                            std::string(info.name) + " channel " + std::to_string(channel) +
                                " at " + std::to_string(x) + "," + std::to_string(y) + ": got " +
                                std::to_string(got[channel]) + " want " + std::to_string(want));
                    }
                }
                const double want_alpha = source_alpha + backdrop_alpha * (1.0 - source_alpha);
                const std::uint8_t want_byte =
                    static_cast<std::uint8_t>(std::lround(want_alpha * 255.0));
                if (got[3] != want_byte) {
                    noise::test::report("coverage is the mode's business", __FILE__, __LINE__,
                                        std::string(info.name) + " alpha at " + std::to_string(x) +
                                            "," + std::to_string(y) + ": got " +
                                            std::to_string(got[3]) + " want " +
                                            std::to_string(want_byte));
                }
                ++noise::test::checks;
            }
        }
    }
    // The count is one per pixel per mode (300 x 8 = 2400): the comparison of a pixel is its three
    // channels and its coverage together, and a failure names the mode and the coordinates.
    CHECK(kModeCount == 8u);
}

// The properties that hold for a mode regardless of arithmetic: they constrain the functions from
// outside, so an implementation that agrees with the test only by copying it would still have to
// obey them.
void test_the_order_properties_each_mode_claims() {
    const double steps[]{0.0, 1.0 / 255.0, 0.25, 0.5 - 1.0 / 510.0, 0.5, 0.5 + 1.0 / 510.0, 0.75,
                         1.0};
    for (const double backdrop : steps) {
        for (const double source : steps) {
            // Multiply can only darken, screen can only lighten, and each is exact at the ends.
            const double multiply = blend_separable(BlendMode::multiply, backdrop, source);
            CHECK(multiply <= std::min(backdrop, source) + 1e-12);
            CHECK(multiply >= 0.0 - 1e-12);
            if (backdrop == 1.0) {
                CHECK_NEAR(multiply, source, 1e-12);
            }
            if (backdrop == 0.0 || source == 0.0) {
                CHECK_NEAR(multiply, 0.0, 1e-12);
            }

            const double screen = blend_separable(BlendMode::screen, backdrop, source);
            CHECK(screen >= std::max(backdrop, source) - 1e-12);
            CHECK(screen <= 1.0 + 1e-12);
            if (backdrop == 0.0) {
                CHECK_NEAR(screen, source, 1e-12);
            }
            if (backdrop == 1.0 || source == 1.0) {
                CHECK_NEAR(screen, 1.0, 1e-12);
            }

            CHECK_EQ(blend_separable(BlendMode::darken, backdrop, source),
                     std::min(backdrop, source));
            CHECK_EQ(blend_separable(BlendMode::lighten, backdrop, source),
                     std::max(backdrop, source));
            CHECK(blend_separable(BlendMode::darken, backdrop, source) <=
                  blend_separable(BlendMode::lighten, backdrop, source) + 1e-12);

            // Dodge lightens the backdrop and burn darkens it, and neither leaves the range.
            const double dodged = blend_separable(BlendMode::color_dodge, backdrop, source);
            CHECK(dodged >= backdrop - 1e-12);
            CHECK(dodged <= 1.0 + 1e-12);
            const double burnt = blend_separable(BlendMode::color_burn, backdrop, source);
            CHECK(burnt <= backdrop + 1e-12);
            CHECK(burnt >= 0.0 - 1e-12);

            // `overlay` is `hard light` with the operands the other way round - which is the whole
            // difference between the two, so it is worth pinning: hard light branches on the source,
            // overlay branches on the backdrop. Written from the definition here rather than shared
            // with the implementation, so the swap cannot be lost in both places at once.
            const double hard_light_of_swapped =
                backdrop <= 0.5 ? 2.0 * backdrop * source
                                : 1.0 - 2.0 * (1.0 - backdrop) * (1.0 - source);
            CHECK_NEAR(blend_separable(BlendMode::overlay, backdrop, source), hard_light_of_swapped,
                       1e-12);

            CHECK_EQ(blend_separable(BlendMode::normal, backdrop, source), source);
        }
    }
}

// The ends of the range, where dodge and burn divide by something that can be zero. Getting these
// wrong is the classic failure: a source of pure white through `color-dodge` is white, and a black
// source through `color-burn` is black, whatever the backdrop.
void test_the_ends_of_the_range_are_pinned() {
    for (const double backdrop : {0.0, 0.2, 0.5, 1.0}) {
        CHECK_EQ(blend_separable(BlendMode::color_dodge, backdrop, 1.0), backdrop == 0.0 ? 0.0 : 1.0);
        CHECK_EQ(blend_separable(BlendMode::color_dodge, 0.0, 0.5), 0.0);
        CHECK_EQ(blend_separable(BlendMode::color_burn, backdrop, 0.0), backdrop == 1.0 ? 1.0 : 0.0);
        CHECK_EQ(blend_separable(BlendMode::color_burn, 1.0, 0.5), 1.0);
        // The corners the two conditions disagree about, named: the black of a dodge is blacker
        // than the formula says, and the white of a burn is whiter.
        CHECK_EQ(blend_separable(BlendMode::color_dodge, 0.0, 1.0), 0.0);
        CHECK_EQ(blend_separable(BlendMode::color_burn, 1.0, 0.0), 1.0);
        CHECK_EQ(blend_separable(BlendMode::multiply, 0.0, 1.0), 0.0);
        CHECK_EQ(blend_separable(BlendMode::screen, 1.0, 0.0), 1.0);
        // `screen` of anything with white is white; `multiply` of anything with black is black.
        CHECK_EQ(blend_separable(BlendMode::screen, 0.3, 1.0), 1.0);
        CHECK_EQ(blend_separable(BlendMode::multiply, 0.7, 0.0), 0.0);
    }
}

// A lone layer over nothing has no second operand, whatever its mode says, and
// `Document::composite` takes the shortcut of handing back the layer's own pixels instead of
// blending. This is what makes that shortcut legal - and it is stated as "the same picture" rather
// than "the same bytes", because a pixel with no coverage keeps whatever colour the destination
// held, so a layer whose transparent pixels still carry colour and an empty canvas differ in bytes
// that no renderer can show. Any mode that changed a covered pixel, or any alpha that came back
// different, fails here.
void test_a_lone_layer_over_nothing_pictures_itself_whatever_the_mode() {
    const Fixture fixture = make_fixture();
    const Image empty = Image::filled(fixture.size, 0, 0, 0, 0);
    for (std::size_t row = 0; row < kModeCount; ++row) {
        Image onto = empty;
        blend_layer(onto, fixture.source, whole(fixture.size), mode_at(row).mode, 1.0);
        for (int y = 0; y < fixture.size.h; ++y) {
            for (int x = 0; x < fixture.size.w; ++x) {
                const std::uint8_t* got = onto.at(x, y);
                const std::uint8_t* want = fixture.source.at(x, y);
                CHECK_EQ(static_cast<int>(got[3]), static_cast<int>(want[3]));
                if (want[3] > 0) {
                    for (int channel = 0; channel < 3; ++channel) {
                        CHECK_EQ(static_cast<int>(got[channel]),
                                 static_cast<int>(want[channel]));
                    }
                }
            }
        }
    }
}

// Coverage is not a mode's business: a mode changes colour, never alpha. Same stack, every mode,
// alpha compared against the first mode's answer.
void test_the_alpha_is_the_same_whatever_the_mode() {
    const Fixture fixture = make_fixture();
    Image reference = fixture.backdrop;
    blend_layer(reference, fixture.source, whole(fixture.size), BlendMode::normal, 0.4);
    for (std::size_t row = 1; row < kModeCount; ++row) {
        Image onto = fixture.backdrop;
        blend_layer(onto, fixture.source, whole(fixture.size), mode_at(row).mode, 0.4);
        for (int y = 0; y < fixture.size.h; ++y) {
            for (int x = 0; x < fixture.size.w; ++x) {
                CHECK_EQ(static_cast<int>(onto.at(x, y)[3]),
                         static_cast<int>(reference.at(x, y)[3]));
            }
        }
    }
}

// A layer at 0 % paints nothing at all, and a pixel the layer does not cover stays exactly as it
// was: the two ways a blend can be asked to do nothing, both of which must be a no-op rather than a
// blend with a black source.
void test_nothing_to_paint_leaves_the_backdrop_alone() {
    const Fixture fixture = make_fixture();
    for (std::size_t row = 0; row < kModeCount; ++row) {
        Image at_zero = fixture.backdrop;
        blend_layer(at_zero, fixture.source, whole(fixture.size), mode_at(row).mode, 0.0);
        CHECK_EQ(at_zero.checksum_hex(), fixture.backdrop.checksum_hex());

        const std::uint8_t uncovered[4] = {0, 0, 0, 0};
        Image source = fixture.source;
        source.fill_rect(Rect{2, 2, 4, 4}, uncovered);
        Image onto = fixture.backdrop;
        blend_layer(onto, source, Rect{0, 0, fixture.size.w, fixture.size.h}, mode_at(row).mode,
                    1.0);
        for (int y = 2; y < 6; ++y) {
            for (int x = 2; x < 6; ++x) {
                for (int channel = 0; channel < 4; ++channel) {
                    CHECK_EQ(static_cast<int>(onto.at(x, y)[channel]),
                             static_cast<int>(fixture.backdrop.at(x, y)[channel]));
                }
            }
        }
    }
}

// Blending a region blends that region: what is outside keeps its bytes, which is what lets the
// compositor repair only what changed.
void test_a_region_blend_touches_only_the_region() {
    const Fixture fixture = make_fixture();
    const Rect region{3, 4, 5, 6};
    for (std::size_t row = 0; row < kModeCount; ++row) {
        Image onto = fixture.backdrop;
        blend_layer(onto, fixture.source, region, mode_at(row).mode, 1.0);
        for (int y = 0; y < fixture.size.h; ++y) {
            for (int x = 0; x < fixture.size.w; ++x) {
                const bool inside = x >= region.x && x < region.right() && y >= region.y &&
                                    y < region.bottom();
                if (inside) {
                    continue;
                }
                for (int channel = 0; channel < 4; ++channel) {
                    CHECK_EQ(static_cast<int>(onto.at(x, y)[channel]),
                             static_cast<int>(fixture.backdrop.at(x, y)[channel]));
                }
            }
        }
    }
}

// The separable functions are per channel, so a grey pair stays grey and a permutation of channels
// permutes the result. A luminosity-style slip (mixing channels) fails here and nowhere else.
void test_channels_never_leak_into_each_other() {
    const std::uint8_t grey_values[]{0, 40, 128, 200, 255};
    for (std::size_t row = 0; row < kModeCount; ++row) {
        for (const std::uint8_t backdrop_level : grey_values) {
            for (const std::uint8_t source_level : grey_values) {
                Image backdrop = Image::filled(Extent{1, 1}, backdrop_level, backdrop_level,
                                               backdrop_level, 255);
                const std::uint8_t source_rgba[4] = {source_level, source_level, source_level, 255};
                Image source = Image::from_rgba(Extent{1, 1},
                                                std::vector<std::uint8_t>(source_rgba,
                                                                          source_rgba + 4));
                blend_layer(backdrop, source, Rect{0, 0, 1, 1}, mode_at(row).mode, 1.0);
                const std::uint8_t* got = backdrop.at(0, 0);
                CHECK_EQ(static_cast<int>(got[0]), static_cast<int>(got[1]));
                CHECK_EQ(static_cast<int>(got[1]), static_cast<int>(got[2]));
                CHECK_EQ(static_cast<int>(got[3]), 255);
            }
        }
    }
}

// A brush dab is `normal`, and its coverage is a fraction rather than a byte: 37 % of an opaque
// violet over opaque red, worked out from the definitions rather than read back out of the code.
void test_a_brush_dab_blends_normal_at_its_coverage() {
    Image onto = Image::filled(Extent{2, 2}, 255, 0, 0, 255);
    const std::uint8_t violet[3] = {40, 90, 200};
    blend_over(onto.at(0, 0), violet, 0.37);
    CHECK_EQ(static_cast<int>(onto.at(0, 0)[0]),
             spec_channel(255, 1.0, 40.0 / 255.0, 40.0 / 255.0, 0.37, 1.0));
    CHECK_EQ(static_cast<int>(onto.at(0, 0)[1]),
             spec_channel(0, 0.0, 90.0 / 255.0, 90.0 / 255.0, 0.37, 1.0));
    CHECK_EQ(static_cast<int>(onto.at(0, 0)[2]),
             spec_channel(0, 0.0, 200.0 / 255.0, 200.0 / 255.0, 0.37, 1.0));
    CHECK_EQ(static_cast<int>(onto.at(0, 0)[3]), 255);

    // Zero coverage is the same pixel, untouched: a dab that painted at 0 would leave a mark.
    blend_over(onto.at(1, 1), violet, 0.0);
    CHECK_EQ(static_cast<int>(onto.at(1, 1)[0]), 255);
    CHECK_EQ(static_cast<int>(onto.at(1, 1)[1]), 0);
    CHECK_EQ(static_cast<int>(onto.at(1, 1)[2]), 0);

    // Full coverage replaces the colour and keeps the pixel opaque.
    blend_over(onto.at(1, 1), violet, 1.0);
    CHECK_EQ(static_cast<int>(onto.at(1, 1)[0]), 40);
    CHECK_EQ(static_cast<int>(onto.at(1, 1)[1]), 90);
    CHECK_EQ(static_cast<int>(onto.at(1, 1)[2]), 200);
    CHECK_EQ(static_cast<int>(onto.at(1, 1)[3]), 255);
}

// Layer opacity scales the layer's own alpha before the mode is applied - so a multiply layer at
// half strength is half a multiply, not multiply of a faded colour.
void test_opacity_scales_coverage_and_not_the_colour() {
    Image onto = Image::filled(Extent{1, 1}, 255, 255, 255, 255);
    Image source = Image::filled(Extent{1, 1}, 128, 128, 128, 255);
    blend_layer(onto, source, Rect{0, 0, 1, 1}, BlendMode::multiply, 0.5);
    // Half of an opaque 50 % grey over opaque white: coverage 0.5, so the multiply lands halfway
    // between white and its own darkened value.
    const double blended = 1.0 * (128.0 / 255.0);
    CHECK_EQ(static_cast<int>(onto.at(0, 0)[0]),
             spec_channel(255, 1.0, 128.0 / 255.0, blended, 0.5, 1.0));
    CHECK_EQ(static_cast<int>(onto.at(0, 0)[3]), 255);
}

// A wide region with a layer at partial strength is the case the per-alpha-pair table exists for,
// and a table is only acceptable if it is the same number the doubles produce - not close to it,
// the same. The fixture is 256x256 with four alpha bands (two backdrop x two source), so one call
// builds up to four tables, and every pixel is then checked against the specification written at
// the top of this file. `blend_layer` decides internally when a pair has earned a table, so the
// test pins the outcome rather than the policy.
// 512 so the dominant alpha pair (384x384 = 147 456 pixels) clears the threshold the
// implementation uses to decide a table is worth building, while the smallest of the four pairs
// (128x128) stays under it and is blended by arithmetic in the same call.
constexpr int kWideSide = 512;

struct WideFixture {
    Image backdrop;
    Image source;
    Extent size{kWideSide, kWideSide};
};

WideFixture make_wide_fixture() {
    std::vector<std::uint8_t> back(std::size_t(kWideSide) * std::size_t(kWideSide) * 4u);
    std::vector<std::uint8_t> src = back;
    for (int y = 0; y < kWideSide; ++y) {
        for (int x = 0; x < kWideSide; ++x) {
            const std::size_t at = (std::size_t(y) * std::size_t(kWideSide) + std::size_t(x)) * 4u;
            back[at + 0] = static_cast<std::uint8_t>(x);
            back[at + 1] = static_cast<std::uint8_t>(y);
            back[at + 2] = static_cast<std::uint8_t>((x * 3 + y * 5) % 256);
            back[at + 3] = y < kWideSide * 3 / 4 ? 255 : 120;
            src[at + 0] = static_cast<std::uint8_t>(255 - x);
            src[at + 1] = static_cast<std::uint8_t>((x + y) % 256);
            src[at + 2] = static_cast<std::uint8_t>(y);
            src[at + 3] = x < kWideSide * 3 / 4 ? 255 : 200;
        }
    }
    WideFixture fixture;
    fixture.backdrop = Image::from_rgba(fixture.size, std::move(back));
    fixture.source = Image::from_rgba(fixture.size, std::move(src));
    return fixture;
}

void test_a_wide_partial_strength_blend_matches_the_definition() {
    const WideFixture fixture = make_wide_fixture();
    const double strengths[]{1.0, 0.5, 0.13};
    for (std::size_t row = 0; row < kModeCount; ++row) {
        const BlendModeInfo& info = mode_at(row);
        for (const double opacity : strengths) {
            Image onto = fixture.backdrop;
            blend_layer(onto, fixture.source, whole(fixture.size), info.mode, opacity);
            for (int y = 0; y < kWideSide; ++y) {
                const double backdrop_alpha = fixture.backdrop.at(0, y)[3] / 255.0;
                for (int x = 0; x < kWideSide; ++x) {
                    const double source_alpha = (fixture.source.at(x, 0)[3] / 255.0) * opacity;
                    const std::uint8_t* got = onto.at(x, y);
                    for (int channel = 0; channel < 3; ++channel) {
                        const double backdrop = fixture.backdrop.at(x, y)[channel] / 255.0;
                        const double source = fixture.source.at(x, y)[channel] / 255.0;
                        const std::uint8_t want = spec_channel(
                            fixture.backdrop.at(x, y)[channel], backdrop, source,
                            spec_blend(info.mode, backdrop, source), source_alpha, backdrop_alpha);
                        if (got[channel] != want) {
                            noise::test::report(
                                "wide blend matches the definition", __FILE__, __LINE__,
                                std::string(info.name) + " at opacity " +
                                    std::to_string(opacity) + " channel " +
                                    std::to_string(channel) + " at " + std::to_string(x) + "," +
                                    std::to_string(y) + ": got " + std::to_string(got[channel]) +
                                    " want " + std::to_string(want));
                        }
                        ++noise::test::checks;
                    }
                }
            }
        }
    }
}

// The same pixels blended in one wide call and in tiles too small for the machinery to engage: if
// the table and the pixel loop ever stop agreeing - one updated, the other not - the two results
// differ and this goes red without needing to know what the answer should have been.
void test_the_table_and_the_arithmetic_are_the_same_pixels() {
    const WideFixture fixture = make_wide_fixture();
    constexpr int kTile = 32;  // 1024 pixels: below any threshold worth having
    for (std::size_t row = 0; row < kModeCount; ++row) {
        const BlendModeInfo& info = mode_at(row);
        for (const double opacity : {0.62, 0.25}) {
            Image wide = fixture.backdrop;
            blend_layer(wide, fixture.source, whole(fixture.size), info.mode, opacity);
            Image tiled = fixture.backdrop;
            for (int y = 0; y < kWideSide; y += kTile) {
                for (int x = 0; x < kWideSide; x += kTile) {
                    blend_layer(tiled, fixture.source, Rect{x, y, kTile, kTile}, info.mode,
                                opacity);
                }
            }
            for (int y = 0; y < kWideSide; ++y) {
                for (int x = 0; x < kWideSide; ++x) {
                    const std::uint8_t* a = wide.at(x, y);
                    const std::uint8_t* b = tiled.at(x, y);
                    for (int channel = 0; channel < 4; ++channel) {
                        if (a[channel] != b[channel]) {
                            noise::test::report("table == arithmetic", __FILE__, __LINE__,
                                                std::string(info.name) + " at opacity " +
                                                    std::to_string(opacity) + " channel " +
                                                    std::to_string(channel) + " at " +
                                                    std::to_string(x) + "," + std::to_string(y) +
                                                    ": table " + std::to_string(a[channel]) +
                                                    " arithmetic " + std::to_string(b[channel]));
                        }
                        ++noise::test::checks;
                    }
                }
            }
        }
    }
}

// Identical work through the wide-call fast path and tiled calls must produce identical pixels.
// Report timings for all runs; the 5% performance threshold is opt-in (NOISE_BLEND_PERF=1)
// because shared CI hosts cannot guarantee a stable benchmark environment.
void test_a_wide_blend_beats_tiling_the_same_pixels(const std::function<double()>& now_ms) {
    constexpr int kSide = 2048;
    constexpr int kTile = 32;
    constexpr Extent size{kSide, kSide};
    const Image backdrop = Image::filled(size, 40, 90, 200, 255);
    const Image source = Image::filled(size, 220, 60, 30, 255);

    Image wide = backdrop;
    double before = now_ms();
    blend_layer(wide, source, whole(size), BlendMode::multiply, 0.55);
    const double wide_ms = now_ms() - before;

    Image tiled = backdrop;
    before = now_ms();
    for (int y = 0; y < kSide; y += kTile) {
        for (int x = 0; x < kSide; x += kTile) {
            blend_layer(tiled, source, Rect{x, y, kTile, kTile}, BlendMode::multiply, 0.55);
        }
    }
    const double tiled_ms = now_ms() - before;

    CHECK_EQ(wide.checksum_hex(), tiled.checksum_hex());
    std::fprintf(stderr, "blend performance: wide %.3f ms, tiled %.3f ms\n", wide_ms, tiled_ms);
    // Shared CI hosts cannot give a stable relative timing. Keep the speed regression check
    // available for dedicated benchmark runs, but never confuse a noisy clock with bad pixels.
    const char* require_perf = std::getenv("NOISE_BLEND_PERF");
    if (require_perf != nullptr && std::string(require_perf) == "1") {
        CHECK(wide_ms < tiled_ms * 0.95);
    }
}

}  // namespace

}  // namespace noise

int main() {
    noise::test_every_mode_matches_the_definition_pixel_for_pixel();
    noise::test_the_order_properties_each_mode_claims();
    noise::test_the_ends_of_the_range_are_pinned();
    noise::test_a_lone_layer_over_nothing_pictures_itself_whatever_the_mode();
    noise::test_the_alpha_is_the_same_whatever_the_mode();
    noise::test_nothing_to_paint_leaves_the_backdrop_alone();
    noise::test_a_region_blend_touches_only_the_region();
    noise::test_channels_never_leak_into_each_other();
    noise::test_a_brush_dab_blends_normal_at_its_coverage();
    noise::test_opacity_scales_coverage_and_not_the_colour();
    noise::test_a_wide_partial_strength_blend_matches_the_definition();
    noise::test_the_table_and_the_arithmetic_are_the_same_pixels();
    const auto epoch = std::chrono::steady_clock::now();
    noise::test_a_wide_blend_beats_tiling_the_same_pixels([&epoch] {
        return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - epoch)
            .count();
    });
    return noise::test::summary("blend");
}
