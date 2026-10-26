// SPDX-License-Identifier: Apache-2.0
//
// The result compositor, and the invariant it exists for: after an answer is landed on the
// document, every byte the feathered mask did not claim is the byte that was there. Whole-image
// diff, never a sampled point - the bug this is here to catch is the one-pixel leak at the far side
// of the crop.

#include "test.h"

#include "ai/colour_match.h"
#include "ai/compose.h"
#include "ai/region.h"
#include "core/mask.h"
#include "core/maskops.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

using namespace noise;
using noise::ai::auto_feather_radius;
using noise::ai::colour_match_drift;
using noise::ai::ComposeReport;
using noise::ai::compose_result;
using noise::ai::CompositePolicy;
using noise::ai::kAutoFeatherMaxPx;
using noise::ai::kAutoFeatherMinPx;
using noise::ai::kCompositePolicy;

using noise::ai::mean_thickness_px;
using noise::ai::plan_region;
using noise::ai::RegionPlan;
using noise::ai::RegionPolicy;

// The test's own region policy, chosen so the numbers in the assertions are the test's rather than the
// shipped table's: a compile is checked against the policy it was handed, and `test_roles` is what checks
// the table's values.
constexpr noise::ai::RegionPolicy kPolicy{0.35, 64, 16, 4096 * 4096};


namespace {

constexpr int kWidth = 320;
constexpr int kHeight = 240;
constexpr Extent kCanvas{kWidth, kHeight};

// Content with hard edges and no period: a composite that pasted the wrong rectangle, or at the
// wrong scale, cannot reproduce these bytes by coincidence.
Image source_layer() {
    Image image = Image::filled(kCanvas, 0, 0, 0, 255);
    for (int y = 0; y < kHeight; ++y) {
        for (int x = 0; x < kWidth; ++x) {
            std::uint8_t* p = image.at(x, y);
            p[0] = static_cast<std::uint8_t>((x * 3 + y * 5) & 0xff);
            p[1] = static_cast<std::uint8_t>((x ^ (y * 7)) & 0xff);
            p[2] = static_cast<std::uint8_t>((x * y) & 0xff);
            p[3] = 255;
        }
    }
    // Transparent, inside the selection: the layer's own silhouette is a promise the answer is not
    // allowed to extend. And beside it, half transparent, where the promise is proportional.
    for (int y = 95; y < 100; ++y) {
        for (int x = 20; x < 32; ++x) {
            std::memcpy(image.at(x, y), kEmptyPixel, kChannels);
        }
    }
    for (int y = 100; y < 105; ++y) {
        for (int x = 20; x < 32; ++x) {
            image.at(x, y)[3] = 128;
        }
    }
    return image;
}

// The two halves of that patch of the layer.
constexpr Rect kLayerHole{20, 95, 12, 5};
constexpr Rect kLayerSheer{20, 100, 12, 5};

// Three shapes at once: coverage that touches the canvas edge (so one crop edge has no canvas
// outside it), a partly covered rim (so the blend has a middle), and coverage over the transparent
// patch of the layer.
Mask selection() {
    Mask mask(kCanvas);
    mask.op(Rect{0, 80, 100, 60}, MaskOp::replace);
    const std::vector<std::uint8_t> rim(static_cast<std::size_t>(8) * 60u, 100);
    mask.op(Rect{100, 80, 8, 60}, rim, MaskOp::replace);
    return mask;
}

struct Fixture {
    Image source = source_layer();
    Mask coverage = selection();
    RegionPlan plan = plan_region(kCanvas, coverage, kPolicy).value();
};

// An answer that has nothing to do with the document, so nothing about it can be mistaken for the
// pixels that were already there.
Image answer_pattern(Extent size) {
    Image image = Image::filled(size, 0, 0, 0, 255);
    for (int y = 0; y < size.h; ++y) {
        for (int x = 0; x < size.w; ++x) {
            std::uint8_t* p = image.at(x, y);
            p[0] = static_cast<std::uint8_t>((11 * x + 3) & 0xff);
            p[1] = static_cast<std::uint8_t>((255 - 5 * y) & 0xff);
            p[2] = static_cast<std::uint8_t>((x + y) & 0xff);
            p[3] = 255;
        }
    }
    return image;
}

struct Diff {
    std::size_t changed = 0;
    // Pixels that changed where the coverage said 0: the invariant, counted over the whole image.
    std::size_t changed_uncovered = 0;
};

Diff compare(const Image& before, const Image& after, const Mask& coverage) {
    Diff diff;
    for (int y = 0; y < kHeight; ++y) {
        for (int x = 0; x < kWidth; ++x) {
            if (std::memcmp(before.at(x, y), after.at(x, y), kChannels) == 0) {
                continue;
            }
            ++diff.changed;
            if (coverage.coverage(x, y) == 0) {
                ++diff.changed_uncovered;
            }
        }
    }
    return diff;
}

std::size_t changed_outside(const Image& before, const Image& after, Rect crop) {
    std::size_t changed = 0;
    for (int y = 0; y < kHeight; ++y) {
        for (int x = 0; x < kWidth; ++x) {
            if (crop.contains(x, y)) {
                continue;
            }
            if (std::memcmp(before.at(x, y), after.at(x, y), kChannels) != 0) {
                ++changed;
            }
        }
    }
    return changed;
}

bool same(const Image& a, const Image& b, int x, int y) {
    return std::memcmp(a.at(x, y), b.at(x, y), kChannels) == 0;
}

// The fixture is the case the invariant needs, and says so: if the region policy moves, this stops
// being a test of an edge-touching mask at 1:1 and the assertions below would prove nothing.
void test_the_fixture_describes_the_case() {
    const Fixture f;
    CHECK_EQ(f.plan.crop.x, 0);              // the mask reaches the canvas edge
    CHECK(f.plan.crop.right() < kWidth);     // ... and the crop does not
    CHECK(f.plan.crop.y > 0);
    CHECK(f.plan.crop.bottom() < kHeight);
    CHECK_NEAR(f.plan.scale, 1.0, 1e-12);
    CHECK(f.plan.requested == Extent{f.plan.crop.w, f.plan.crop.h});
    CHECK(f.coverage.bounds().x == 0);
    CHECK(f.coverage.area() < static_cast<double>(area(f.coverage.bounds())));  // partly covered
}

void test_every_byte_outside_the_feather_is_the_byte_that_was_there() {
    Fixture f;
    Image target = f.source;
    const Image answer = answer_pattern(f.plan.requested);
    const CompositePolicy policy{3, false, 5};

    const ComposeReport report = compose_result(target, target, answer, f.plan, f.coverage, policy)
                                                ;
    CHECK(report.composed);
    CHECK(report.refusal.empty());
    CHECK_EQ(report.feather_px, 3);  // a policy that named a radius got that radius
    CHECK_EQ(report.result_dims, f.plan.requested);
    CHECK(report.answered_as_requested);
    CHECK(report.coverage.size() == kCanvas);
    CHECK(report.pixels_changed > 0);  // otherwise everything below passes by doing nothing

    const Diff diff = compare(f.source, target, report.coverage);
    CHECK_EQ(diff.changed_uncovered, std::size_t(0));
    CHECK_EQ(diff.changed, report.pixels_changed);
    CHECK_EQ(changed_outside(f.source, target, f.plan.crop), std::size_t(0));

    // The feather cannot teleport: coverage ends within the blur's own reach of the selection it
    // came from. Three passes of radius r reach 3r, which is a bound, not a claim.
    const int reach = 3 * report.feather_px;
    const Rect bounds = f.coverage.bounds();
    std::size_t far_coverage = 0;
    for (int y = 0; y < kHeight; ++y) {
        for (int x = 0; x < kWidth; ++x) {
            const bool outside_selection_reach = x < bounds.x - reach || x >= bounds.right() + reach ||
                                                 y < bounds.y - reach || y >= bounds.bottom() + reach;
            if (outside_selection_reach && report.coverage.coverage(x, y) != 0) {
                ++far_coverage;
            }
        }
    }
    CHECK_EQ(far_coverage, std::size_t(0));

    // And the mask the report shows is the one that was used, not a fresh one: it is empty wherever
    // the crop was not.
    std::size_t coverage_outside_crop = 0;
    for (int y = 0; y < kHeight; ++y) {
        for (int x = 0; x < kWidth; ++x) {
            if (!f.plan.crop.contains(x, y) && report.coverage.coverage(x, y) != 0) {
                ++coverage_outside_crop;
            }
        }
    }
    CHECK_EQ(coverage_outside_crop, std::size_t(0));
}

void test_the_two_ends_of_the_coverage_scale_are_exact() {
    Fixture f;
    Image target = f.source;
    const Image answer = Image::filled(f.plan.requested, 10, 200, 30, 255);
    const CompositePolicy policy{3, false, 0};

    const ComposeReport report = compose_result(target, target, answer, f.plan, f.coverage, policy)
                                                ;
    CHECK(report.composed);

    // Where the mask is wholly over a wholly opaque part of the layer, the answer is all there is.
    CHECK_EQ(report.coverage.coverage(60, 110), 255);
    const std::uint8_t* pasted = target.at(60, 110);
    CHECK_EQ(pasted[0], 10);
    CHECK_EQ(pasted[1], 200);
    CHECK_EQ(pasted[2], 30);
    CHECK_EQ(pasted[3], 255);

    // An empty pixel of the destination is not a veto. The answer is received by its own layer, so the
    // coverage is the only thing that says where a write goes - which is what lets an outpainted area
    // hold an answer at all, since by definition there was nothing there before.
    for (int y = kLayerHole.y; y < kLayerHole.bottom(); ++y) {
        for (int x = kLayerHole.x; x < kLayerHole.right(); ++x) {
            if (f.coverage.coverage(x, y) == 0) {
                continue;  // outside the region the compiler was asked about, so untouched
            }
            CHECK_EQ(report.coverage.coverage(x, y), 255);
            const std::uint8_t* landed = target.at(x, y);
            CHECK_EQ(landed[0], 10);
            CHECK_EQ(landed[3], 255);
        }
    }

    // A sheer destination pixel takes the answer in proportion to its own coverage of the region: not a
    // binary veto but not a whole paste either, the way a brush behaves over a half-painted pixel.
    for (int y = kLayerSheer.y; y < kLayerSheer.bottom(); ++y) {
        for (int x = kLayerSheer.x; x < kLayerSheer.right(); ++x) {
            if (f.coverage.coverage(x, y) == 0) {
                continue;
            }
            const std::uint8_t* coated = target.at(x, y);
            CHECK(coated[3] > 0 && coated[3] <= 255);
        }
    }

    // In between, the pixel is between: not the source, not the answer, and inside their bounds.
    std::size_t partial = 0;
    std::size_t outside_the_span = 0;
    for (int y = 0; y < kHeight; ++y) {
        for (int x = 0; x < kWidth; ++x) {
            const std::uint8_t coverage = report.coverage.coverage(x, y);
            if (coverage == 0 || coverage == 255) {
                continue;
            }
            ++partial;
            const int to[kChannels] = {10, 200, 30, 255};
            for (int c = 0; c < kChannels; ++c) {
                const int from = f.source.at(x, y)[c];
                const int got = target.at(x, y)[c];
                if (got < std::min(from, to[c]) - 1 || got > std::max(from, to[c]) + 1) {
                    ++outside_the_span;
                }
            }
        }
    }
    CHECK(partial > 0);
    CHECK_EQ(outside_the_span, std::size_t(0));
}

// A crop whose context is switched off, so the selection abuts the crop edge and the lock band has
// something to do: with the default context the mask is always further from the crop edge than any
// feather reaches, and a band that never bites is a band nobody can test.
constexpr RegionPolicy kTightPolicy{0.0, 0, 16, 1024 * 1024};

RegionPlan tight_plan(const Mask& coverage) {
    return plan_region(kCanvas, coverage, kTightPolicy).value();
}

void test_the_lock_band_holds_the_original_at_the_crop_edge() {
    const Fixture f;
    const RegionPlan plan = tight_plan(f.coverage);
    CHECK_EQ(plan.crop.x, 0);            // the edge with no canvas outside it
    CHECK(plan.crop.right() < kWidth);   // the edge with canvas outside it
    CHECK(plan.crop.y > 0);
    CHECK(f.coverage.bounds().right() == plan.crop.right() - 4);  // the snap's slack, no more

    constexpr int kBand = 8;
    Image target = f.source;
    const Image answer = Image::filled(plan.requested, 10, 200, 30, 255);
    const CompositePolicy policy{3, false, kBand};
    const ComposeReport report = compose_result(target, target, answer, plan, f.coverage, policy)
                                                ;
    CHECK(report.composed);
    CHECK(report.pixels_changed > 0);

    // The interior edges of the crop are the original for `kBand` pixels in, whatever the mask
    // claims there.
    std::size_t band_pixels = 0;
    std::size_t band_moved = 0;
    for (int y = plan.crop.y; y < plan.crop.bottom(); ++y) {
        for (int x = plan.crop.x; x < plan.crop.right(); ++x) {
            const bool at_interior_edge =
                (plan.crop.right() - x <= kBand) || (y - plan.crop.y < kBand) ||
                (plan.crop.bottom() - y <= kBand);
            if (!at_interior_edge) {
                continue;
            }
            ++band_pixels;
            if (!same(f.source, target, x, y)) {
                ++band_moved;
            }
            CHECK_EQ(report.coverage.coverage(x, y), 0);
        }
    }
    CHECK(band_pixels > 0);
    CHECK_EQ(band_moved, std::size_t(0));

    // The crop edge that *is* the canvas edge is not locked: there is no never-requested pixel
    // outside it to keep, and throwing away a strip of the paste there would be a hole in the
    // picture for no reason.
    CHECK_EQ(report.coverage.coverage(0, 110), 255);
    CHECK(!same(f.source, target, 0, 110));

    // Without the band the same pixels do move - the band is what stopped them, not the feather.
    Image unbanded = f.source;
    const ComposeReport wide = compose_result(unbanded, unbanded, answer, plan, f.coverage, CompositePolicy{3, false, 0})
                                              ;
    CHECK(wide.composed);
    std::size_t moved = 0;
    for (int y = plan.crop.y + kBand; y < plan.crop.bottom() - kBand; ++y) {
        if (!same(f.source, unbanded, plan.crop.right() - 1, y)) {
            ++moved;
        }
    }
    CHECK(moved > 0);
    CHECK(wide.pixels_changed > report.pixels_changed);
}

void test_a_refusal_leaves_every_byte_alone() {
    const Fixture f;
    const CompositePolicy policy{3, true, 6};

    Image target = f.source;
    const ComposeReport no_answer = compose_result(target, target, Image{}, f.plan, f.coverage, policy)
                                                   ;
    CHECK(!no_answer.composed);
    CHECK(no_answer.refusal.find("answer") != std::string::npos);
    CHECK_EQ(no_answer.pixels_changed, std::size_t(0));
    CHECK(!no_answer.coverage.sized());
    for (int y = 0; y < kHeight; ++y) {
        for (int x = 0; x < kWidth; ++x) {
            CHECK(same(f.source, target, x, y));
        }
    }

    // A plan for a canvas this target is not.
    Image small = Image::filled(Extent{16, 16}, 5, 5, 5, 255);
    const ComposeReport wrong_target = compose_result(small, small, answer_pattern(f.plan.requested), f.plan, f.coverage, policy)
                                                      ;
    CHECK(!wrong_target.composed);
    CHECK(wrong_target.refusal.find("crop") != std::string::npos);

    // Coverage that does not reach the crop the plan was compiled for: the caller handed the
    // compositor two different facts.
    Mask elsewhere(kCanvas);
    elsewhere.op(Rect{250, 200, 20, 20}, MaskOp::replace);
    Image untouched = f.source;
    const ComposeReport wrong_mask = compose_result(untouched, untouched, answer_pattern(f.plan.requested), f.plan, elsewhere, policy)
                                                    ;
    CHECK(!wrong_mask.composed);
    CHECK(wrong_mask.refusal.find("coverage") != std::string::npos);

    // And a mask with no pixels at all is not "everything", it is nothing: the distinction the
    // document draws, drawn here too.
    const ComposeReport no_mask = compose_result(untouched, untouched, answer_pattern(f.plan.requested), f.plan, Mask{}, policy)
                                                 ;
    CHECK(!no_mask.composed);
    CHECK(no_mask.refusal.find("coverage") != std::string::npos);
    for (int y = 0; y < kHeight; ++y) {
        for (int x = 0; x < kWidth; ++x) {
            CHECK(same(f.source, untouched, x, y));
        }
    }
}

Mask blob(Extent canvas, Rect box) {
    Mask mask(canvas);
    mask.op(box, MaskOp::replace);
    return mask;
}

void test_the_auto_feather_is_a_fraction_of_the_feature() {
    const Extent canvas{400, 400};
    const Mask fat = blob(canvas, Rect{50, 50, 200, 200});
    const Mask stroke = blob(canvas, Rect{50, 50, 4, 200});
    const Mask mid = blob(canvas, Rect{50, 50, 40, 200});

    // `2 * area / perimeter` names the width of what it is given: a 4 px band, not a 4 px square.
    CHECK(mean_thickness_px(stroke) > 3.0);
    CHECK(mean_thickness_px(stroke) < 5.0);
    CHECK(mean_thickness_px(fat) > mean_thickness_px(mid));
    CHECK(mean_thickness_px(mid) > mean_thickness_px(stroke));

    CHECK_EQ(auto_feather_radius(fat), kAutoFeatherMaxPx);
    CHECK_EQ(auto_feather_radius(stroke), kAutoFeatherMinPx);
    const int between = auto_feather_radius(mid);
    CHECK(between > kAutoFeatherMinPx);
    CHECK(between < kAutoFeatherMaxPx);

    // What the range is for: the thin feature keeps its edge, the fat one gets a seam hidden.
    Fixture f;
    Image target = f.source;
    const ComposeReport report = compose_result(target, target, answer_pattern(f.plan.requested), f.plan, f.coverage, CompositePolicy{std::nullopt, false, 0})
                                                ;
    CHECK(report.composed);
    CHECK(report.feather_px >= kAutoFeatherMinPx);
    CHECK(report.feather_px <= kAutoFeatherMaxPx);
    // A feather of none is a hard edge, and it is a legal policy.
    Image hard = f.source;
    const ComposeReport cut = compose_result(hard, hard, answer_pattern(f.plan.requested), f.plan, f.coverage, CompositePolicy{0, false, 0})
                                             ;
    CHECK(cut.composed);
    CHECK_EQ(cut.feather_px, 0);
    // A hard edge is the selection and nothing else: the coverage the compositor used equals the mask it
    // was handed, pixel for pixel, including where the destination had no alpha to "hold" it.
    std::size_t differs_from_the_mask = 0;
    for (int y = 0; y < kHeight; ++y) {
        for (int x = 0; x < kWidth; ++x) {
            if (cut.coverage.coverage(x, y) != f.coverage.coverage(x, y)) {
                ++differs_from_the_mask;
            }
        }
    }
    CHECK_EQ(differs_from_the_mask, std::size_t(0));
}

// The other side of the map: the compiler recorded where the request sits, the compositor lands the
// answer on the crop, and the two have to be the same fact.
void test_an_answer_lands_on_the_crop_it_came_from() {
    // 1:1, on a crop that is neither the whole canvas nor at the origin.
    {
        const Fixture f;
        Image target = Image::filled(kCanvas, 100, 100, 100, 255);
        Image answer = Image::filled(f.plan.requested, 0, 0, 0, 255);
        const int marker_x = 50;
        const int marker_y = 84;  // a request pixel whose document point is inside the selection
        std::uint8_t* bright = answer.at(marker_x, marker_y);
        bright[0] = bright[1] = bright[2] = 255;

        const ComposeReport report = compose_result(target, target, answer, f.plan, f.coverage, CompositePolicy{0, false, 0})
                                                    ;
        CHECK(report.composed);
        const Vec2 expected = f.plan.to_document(Vec2{static_cast<double>(marker_x) + 0.5,
                                                     static_cast<double>(marker_y) + 0.5});
        int found = -1;
        int found_value = -1;
        for (int y = f.plan.crop.y; y < f.plan.crop.bottom(); ++y) {
            for (int x = f.plan.crop.x; x < f.plan.crop.right(); ++x) {
                if (target.at(x, y)[0] > found_value) {
                    found_value = target.at(x, y)[0];
                    found = y * kWidth + x;
                }
            }
        }
        CHECK_EQ(found_value, 255);
        CHECK(std::abs(found % kWidth - (expected.x - 0.5)) <= 2.0);
        CHECK(std::abs(found / kWidth - (expected.y - 0.5)) <= 2.0);
    }

    // Fitted: the plan scaled the crop down to fit the budget, so the answer is smaller than the
    // crop and every pixel of it is a resample.
    {
        // A budget the test fits against, so the resample below is a real shrink rather than a 1:1
        // pass. The shipped limit belongs to `roles.json` and is checked in `test_roles`.
        RegionPolicy budget = kPolicy;
        budget.max_area_px = 1024 * 1024;
        const Extent canvas{1600, 1200};
        const Mask coverage = blob(canvas, Rect{50, 50, 1400, 1000});
        const RegionPlan plan = plan_region(canvas, coverage, budget).value();
        CHECK(plan.scale < 1.0);
        CHECK(plan.requested != Extent{plan.crop.w, plan.crop.h});

        Image target = Image::filled(canvas, 100, 100, 100, 255);
        Image answer = Image::filled(plan.requested, 0, 0, 0, 255);
        const int marker_x = 300;
        const int marker_y = 200;
        std::uint8_t* bright = answer.at(marker_x, marker_y);
        bright[0] = bright[1] = bright[2] = 255;

        const ComposeReport report = compose_result(target, target, answer, plan, coverage, CompositePolicy{0, false, 0})
                                                    ;
        CHECK(report.composed);
        CHECK(report.answered_as_requested);
        const Vec2 expected = plan.to_document(Vec2{static_cast<double>(marker_x) + 0.5,
                                                   static_cast<double>(marker_y) + 0.5});
        int found_x = -1;
        int found_y = -1;
        int found_value = -1;
        for (int y = plan.crop.y; y < plan.crop.bottom(); ++y) {
            for (int x = plan.crop.x; x < plan.crop.right(); ++x) {
                if (target.at(x, y)[0] > found_value) {
                    found_value = target.at(x, y)[0];
                    found_x = x;
                    found_y = y;
                }
            }
        }
        // Not necessarily 255: no destination sample need land exactly on the impulse, and a filter
        // with a finite tap is free to peak below it. Where the peak is is the assertion.
        CHECK(found_value >= 200);
        CHECK(std::abs(found_x - (expected.x - 0.5)) <= 2.0);
        CHECK(std::abs(found_y - (expected.y - 0.5)) <= 2.0);
    }

    // The engine answers the snapped size, which is not the size that was asked for. The answer still
    // covers the crop - the crop is the field of view, whatever the answer's pixel count - and the
    // report says the two disagreed rather than pretending they did not.
    {
        const Fixture f;
        Image target = Image::filled(kCanvas, 100, 100, 100, 255);
        const Extent snapped{f.plan.requested.w + 16, f.plan.requested.h + 16};
        Image answer = Image::filled(snapped, 0, 0, 0, 255);
        // A block rather than a single pixel: a reduction spreads one pixel over its neighbours and
        // the filter is free to bring no output sample back to full brightness.
        const Rect mark{48, 96, 8, 8};
        for (int y = mark.y; y < mark.bottom(); ++y) {
            for (int x = mark.x; x < mark.right(); ++x) {
                std::uint8_t* bright = answer.at(x, y);
                bright[0] = bright[1] = bright[2] = 255;
            }
        }

        const ComposeReport report = compose_result(target, target, answer, f.plan, f.coverage, CompositePolicy{0, false, 0})
                                                    ;
        CHECK(report.composed);
        CHECK(!report.answered_as_requested);
        CHECK(report.result_dims == snapped);

        // `plan.to_document` measures the request that was sent, not the answer that came back, so
        // the expected point here is the crop's own proportion of what arrived.
        const double want_x = f.plan.crop.x + (mark.x + mark.w / 2.0) * f.plan.crop.w / snapped.w;
        const double want_y = f.plan.crop.y + (mark.y + mark.h / 2.0) * f.plan.crop.h / snapped.h;
        double sum_x = 0.0;
        double sum_y = 0.0;
        int lit = 0;
        for (int y = f.plan.crop.y; y < f.plan.crop.bottom(); ++y) {
            for (int x = f.plan.crop.x; x < f.plan.crop.right(); ++x) {
                if (target.at(x, y)[0] > 128) {
                    sum_x += x;
                    sum_y += y;
                    ++lit;
                }
            }
        }
        CHECK(static_cast<std::size_t>(lit) >= area(mark) / 2);
        CHECK(std::abs(sum_x / static_cast<double>(lit) - want_x) <= 2.0);
        CHECK(std::abs(sum_y / static_cast<double>(lit) - want_y) <= 2.0);
    }
}

void test_the_flag_decides_and_a_missing_band_matches_nothing() {
    Fixture f;
    // An answer that came back a different colour from the picture it belongs to.
    Image drifted = Image::filled(f.plan.requested, 250, 250, 250, 255);

    Image matched = f.source;
    const ComposeReport with_match = compose_result(matched, matched, drifted, f.plan, f.coverage, CompositePolicy{2, true, 0})
                                                    ;
    Image unmatched = f.source;
    const ComposeReport without = compose_result(unmatched, unmatched, drifted, f.plan, f.coverage, CompositePolicy{2, false, 0})
                                                 ;
    CHECK(with_match.composed);
    CHECK(without.composed);
    CHECK(!without.colour_corrected);
    CHECK(with_match.colour_corrected);

    // Deep inside the selection the two differ - the match did something - and what it did was undo the
    // drift the answer put on the band the composite left alone, which is measured, not assumed: a flat
    // 250 answer over a picture that is nowhere near 250 drifted, so the paste is pulled back towards the
    // tone of the frame it belongs to.
    const std::uint8_t* a = matched.at(60, 110);
    const std::uint8_t* b = unmatched.at(60, 110);
    CHECK_NE(a[0], b[0]);

    double ring_sum = 0.0;
    std::size_t ring_pixels = 0;
    for (int y = f.plan.crop.y; y < f.plan.crop.bottom(); ++y) {
        for (int x = f.plan.crop.x; x < f.plan.crop.right(); ++x) {
            if (f.coverage.coverage(x, y) != 0 || f.source.at(x, y)[3] == 0) {
                continue;  // only pixels the composite guaranteed to leave as they were
            }
            ring_sum += static_cast<double>(f.source.at(x, y)[0]);
            ++ring_pixels;
        }
    }
    const double band = ring_sum / static_cast<double>(ring_pixels);
    CHECK(std::abs(static_cast<double>(a[0]) - band) <
          std::abs(static_cast<double>(b[0]) - band));

    // No band, no match: a mask over the whole crop has nothing beside it to compare the answer with.
    Mask whole(kCanvas);
    whole.fill(255);
    const RegionPlan full = plan_region(kCanvas, whole, kTightPolicy).value();
    Image all = Image::filled(kCanvas, 100, 90, 80, 255);
    const Image before = all;
    const ComposeReport report = compose_result(all, all, answer_pattern(full.requested), full, whole, CompositePolicy{0, true, 0})
                                                ;
    CHECK(report.composed);
    CHECK(!report.colour_corrected);
    for (int y = 0; y < kHeight; ++y) {
        for (int x = 0; x < kWidth; ++x) {
            CHECK(same(before, all, x, y) || report.coverage.coverage(x, y) > 0);
        }
    }
}

// The destination receives the answer; the **document** says what colour it should have been. An answer
// now lands on a fresh empty layer, so measuring drift against the destination would compare the answer
// with nothing, report no drift, and leave a visibly dark patch - which is exactly what an earlier build
// did while it still landed on the layer it came from.
void test_the_drift_is_measured_against_the_document_not_the_empty_destination() {
    Fixture f;
    const Image document = f.source;
    Image answer = document.region(f.plan.crop);  // the crop's own size: the resample is a pass-through
    for (int y = 0; y < answer.size().h; ++y) {
        for (int x = 0; x < answer.size().w; ++x) {
            for (int c = 0; c < 3; ++c) {
                answer.at(x, y)[c] =
                    static_cast<std::uint8_t>(static_cast<int>(answer.at(x, y)[c]) * 4 / 5);
            }
        }
    }
    const std::uint8_t* want = document.at(60, 110);  // wholly covered, textured, opaque
    const auto off_from = [](const Image& image, const std::uint8_t* truth) {
        return std::abs(static_cast<int>(image.at(60, 110)[0]) - static_cast<int>(truth[0]));
    };

    Image uncorrected = Image::filled(kCanvas, 0, 0, 0, 0);
    const ComposeReport off =
        compose_result(uncorrected, document, answer, f.plan, f.coverage, CompositePolicy{0, false, 0});
    REQUIRE(off.composed);
    CHECK(!off.colour_corrected);
    const int before = off_from(uncorrected, want);
    CHECK(before > 20);  // the drift is in the answer, and it is visible

    Image matched = Image::filled(kCanvas, 0, 0, 0, 0);
    const ComposeReport corrected =
        compose_result(matched, document, answer, f.plan, f.coverage, CompositePolicy{0, true, 0});
    REQUIRE(corrected.composed);
    // The claim an empty destination used to defeat: there *is* a drift to measure.
    CHECK(corrected.colour_corrected);
    const int after = off_from(matched, want);
    CHECK(after < before / 2);
}

// The colour match on its own, where its statistics can be read off the fixture.

Image flat_with_holes(Extent size, std::uint8_t grey) {
    Image image = Image::filled(size, grey, grey, grey, 255);
    for (int y = 0; y < size.h; y += 5) {
        for (int x = 0; x < size.w / 2; x += 3) {
            std::memcpy(image.at(x, y), kEmptyPixel, kChannels);
        }
    }
    return image;
}

// A bright subject in a dark frame, which is the shape of the complaint: an answer that changed the
// subject and nothing else must not be moved, because the frame around it is not the subject's palette.
// `colour_match_ring` used to pull the answer to the mean of that dark frame, and a Replace on a face
// came back as a visibly dark patch.
Image subject_in_shadow(Extent size, Rect subject) {
    Image image = Image::filled(size, 25, 22, 30, 255);
    for (int y = subject.y; y < subject.bottom(); ++y) {
        for (int x = subject.x; x < subject.right(); ++x) {
            std::uint8_t* p = image.at(x, y);
            p[0] = 230;
            p[1] = 190;
            p[2] = 170;
        }
    }
    return image;
}

void test_an_answer_that_changed_only_the_subject_is_left_exactly_alone() {
    const Extent size{64, 48};
    const Rect subject{20, 14, 24, 20};
    const Image original = subject_in_shadow(size, subject);
    Mask reach(size);
    reach.op(subject, MaskOp::replace);

    Image patch = original;
    for (int y = subject.y; y < subject.bottom(); ++y) {
        for (int x = subject.x; x < subject.right(); ++x) {
            std::uint8_t* p = patch.at(x, y);
            p[0] = 40;
            p[1] = 170;
            p[2] = 60;  // green eyes: the whole point of the request
        }
    }

    const Image as_returned = patch;
    CHECK(!colour_match_drift(patch, original, reach));
    // Nothing written anywhere: not to the frame, and not to the change either. Comparing against the
    // answer as it came back rather than against the document is the point - the subject is *meant* to
    // differ, and a match that "corrected" it would be repainting the edit.
    for (int y = 0; y < size.h; ++y) {
        for (int x = 0; x < size.w; ++x) {
            CHECK_EQ(std::memcmp(as_returned.at(x, y), patch.at(x, y), kChannels), 0);
        }
    }
}

void test_the_drift_on_the_band_is_taken_back_off_the_whole_answer() {
    const Extent size{64, 48};
    const Rect subject{20, 14, 24, 20};
    Image original = subject_in_shadow(size, subject);
    // A band with real variation, so the spread has something to be measured against.
    for (int y = 0; y < size.h; ++y) {
        for (int x = 0; x < size.w; ++x) {
            if (x >= subject.x && x < subject.right() && y >= subject.y && y < subject.bottom()) {
                continue;
            }
            const std::uint8_t value = (x / 4) % 2 == 0 ? 40 : 200;
            original.at(x, y)[0] = value;
            original.at(x, y)[1] = static_cast<std::uint8_t>(value / 2);
            original.at(x, y)[2] = static_cast<std::uint8_t>(value / 3);
        }
    }
    Mask reach(size);
    reach.op(subject, MaskOp::replace);

    // The answer came back a fifth darker, everywhere - the band as much as the subject. A proportional
    // loss is what an edit does to a frame, and it is deliberately *not* the shape the correction models
    // (a shift in Lab), so this measures how much drift comes back rather than inverting the arithmetic.
    const auto darker = [](std::uint8_t value) {
        return static_cast<std::uint8_t>(static_cast<int>(value) * 4 / 5);
    };
    Image patch = original;
    for (int y = 0; y < size.h; ++y) {
        for (int x = 0; x < size.w; ++x) {
            std::uint8_t* p = patch.at(x, y);
            p[0] = darker(p[0]);
            p[1] = darker(p[1]);
            p[2] = darker(p[2]);
        }
    }

    // Mean |difference from the document| over one side, before and after.
    const auto residual = [size, subject, &original](const Image& image) {
        std::array<double, 2> sum{};
        std::array<std::size_t, 2> count{};
        for (int y = 0; y < size.h; ++y) {
            for (int x = 0; x < size.w; ++x) {
                const bool in_subject = x >= subject.x && x < subject.right() &&
                                        y >= subject.y && y < subject.bottom();
                for (int c = 0; c < 3; ++c) {
                    sum[in_subject ? 1 : 0] += std::abs(static_cast<double>(original.at(x, y)[c]) -
                                                        static_cast<double>(image.at(x, y)[c]));
                    ++count[in_subject ? 1 : 0];
                }
            }
        }
        return std::array<double, 2>{sum[0] / static_cast<double>(count[0]),
                                     sum[1] / static_cast<double>(count[1])};
    };

    const std::array<double, 2> before = residual(patch);
    CHECK(before[0] > 10.0);  // the fixture really did drift
    CHECK(colour_match_drift(patch, original, reach));
    const std::array<double, 2> after = residual(patch);

    // Most of the drift is gone where it can be checked...
    CHECK(after[0] < before[0] / 4.0);
    // ...and a good deal of it is gone where it cannot be checked, which is the entire reason to measure
    // it on the band: the subject has no reference and inherits the correction the band paid for. Not the
    // whole of it - a proportional loss is a bigger shift in Lab at L 90 than at L 30, and the band never
    // held a tone that bright, so claiming a quarter here would be claiming the correction knows the
    // subject's colour. It does not, and must not pretend to.
    CHECK(after[1] < before[1] / 2.0);
}

void test_a_match_is_identity_when_there_is_no_band() {
    const Extent size{40, 30};
    // A mask over everything has no band: the answer has nothing beside it to be compared with, so the
    // patch is left exactly as it came back. This is the full-frame case.
    Image patch = Image::filled(size, 30, 140, 90, 255);
    const Image before = patch;
    Mask whole(size);
    whole.fill(255);
    CHECK(!colour_match_drift(patch, before, whole));
    for (int y = 0; y < size.h; ++y) {
        for (int x = 0; x < size.w; ++x) {
            CHECK_EQ(std::memcmp(before.at(x, y), patch.at(x, y), kChannels), 0);
        }
    }
}

void test_a_band_with_no_colour_in_it_is_not_colour() {
    // Holes in the band are not black pixels, they are no colour. Pairing one would read as a drift
    // towards dark and darken the paste for no reason the picture can show.
    const Extent size{40, 30};
    const Image original = flat_with_holes(size, 120);
    const Rect box{20, 8, 16, 14};
    Mask reach(size);
    reach.op(box, MaskOp::replace);
    Image patch = original;
    for (int y = 0; y < size.h; ++y) {
        for (int x = 0; x < size.w; ++x) {
            if (patch.at(x, y)[3] == 0) {
                continue;  // no colour on the document side either: not part of the pairing
            }
            std::uint8_t* p = patch.at(x, y);
            p[0] = p[1] = p[2] = static_cast<std::uint8_t>(p[0] > 30 ? p[0] - 30 : 0);
        }
    }
    // A pixel of the answer with no colour, inside the area being corrected.
    std::memcpy(patch.at(22, 9), kEmptyPixel, kChannels);

    CHECK(colour_match_drift(patch, original, reach));
    for (int y = box.y; y < box.bottom(); ++y) {
        for (int x = box.x; x < box.right(); ++x) {
            if (patch.at(x, y)[3] == 0) {
                // Nothing to correct, and inventing a colour under a zero alpha would put a pixel
                // where the layer has none.
                CHECK_EQ(std::memcmp(patch.at(x, y), kEmptyPixel, kChannels), 0);
                continue;
            }
            // Back to the 120 the band actually is, rather than to something the colourless holes
            // dragged down.
            CHECK(patch.at(x, y)[0] >= 115);
            CHECK(patch.at(x, y)[0] <= 125);
        }
    }
}

void test_a_flat_band_shifts_without_flattening() {
    const Extent size{40, 30};
    const Image original = flat_with_holes(size, 120);  // a band with no spread at all
    const Rect box{10, 8, 20, 14};
    Mask reach(size);
    reach.op(box, MaskOp::replace);
    Image patch = original;
    for (int y = 0; y < size.h; ++y) {
        for (int x = 0; x < size.w; ++x) {
            if (patch.at(x, y)[3] == 0) {
                continue;
            }
            std::uint8_t* p = patch.at(x, y);
            p[0] = p[1] = p[2] = static_cast<std::uint8_t>(p[0] > 20 ? p[0] - 20 : 0);
        }
    }
    for (int y = box.y; y < box.bottom(); ++y) {
        for (int x = box.x; x < box.right(); ++x) {
            const std::uint8_t value = (x + y) % 2 == 0 ? 60 : 180;
            std::memcpy(patch.at(x, y), &value, 1);
            patch.at(x, y)[1] = patch.at(x, y)[2] = value;
        }
    }

    CHECK(colour_match_drift(patch, original, reach));
    // The spread the answer brought with it is still there: matching a band with no variation would
    // otherwise flatten a textured patch to one grey, which is not a correction, it is a smear.
    int low = 255;
    int high = 0;
    for (int y = box.y; y < box.bottom(); ++y) {
        for (int x = box.x; x < box.right(); ++x) {
            low = std::min(low, static_cast<int>(patch.at(x, y)[0]));
            high = std::max(high, static_cast<int>(patch.at(x, y)[0]));
        }
    }
    CHECK(high - low > 80);
}

void test_the_spread_correction_stops_short_of_a_palette_change() {
    const Extent size{40, 30};
    // A band running the width of the range in the document, and an answer that returned it barely
    // varying: matching those spreads exactly would blow the new content apart.
    Image original = Image::filled(size, 0, 0, 0, 255);
    for (int y = 0; y < size.h; ++y) {
        for (int x = 0; x < size.w; ++x) {
            const std::uint8_t value = (x / 3) % 2 == 0 ? 0 : 255;
            original.at(x, y)[0] = original.at(x, y)[1] = original.at(x, y)[2] = value;
        }
    }
    const Rect box{12, 9, 16, 12};
    Mask reach(size);
    reach.op(box, MaskOp::replace);
    Image patch = original;
    for (int y = 0; y < size.h; ++y) {
        for (int x = 0; x < size.w; ++x) {
            if (x >= box.x && x < box.right() && y >= box.y && y < box.bottom()) {
                continue;
            }
            if (patch.at(x, y)[3] == 0) {
                continue;
            }
            // The band came back nearly flat, whatever the answer did to the subject.
            const std::uint8_t value = (x / 3) % 2 == 0 ? 110 : 130;
            patch.at(x, y)[0] = patch.at(x, y)[1] = patch.at(x, y)[2] = value;
        }
    }
    for (int y = box.y; y < box.bottom(); ++y) {
        for (int x = box.x; x < box.right(); ++x) {
            const std::uint8_t value = (x + y) % 2 == 0 ? 110 : 130;
            patch.at(x, y)[0] = patch.at(x, y)[1] = patch.at(x, y)[2] = value;
        }
    }

    CHECK(colour_match_drift(patch, original, reach));
    int low = 255;
    int high = 0;
    for (int y = box.y; y < box.bottom(); ++y) {
        for (int x = box.x; x < box.right(); ++x) {
            low = std::min(low, static_cast<int>(patch.at(x, y)[0]));
            high = std::max(high, static_cast<int>(patch.at(x, y)[0]));
        }
    }
    CHECK(high > low);  // it did correct something
    // The band's spread is far wider than the answer's, so an uncapped ratio would throw this 20-step
    // texture across the whole range. The cap is two, and two of a 20-step texture is 40.
    CHECK(high - low <= 2 * (130 - 110) + 6);
}


}  // namespace

int main() {
    test_the_fixture_describes_the_case();
    test_every_byte_outside_the_feather_is_the_byte_that_was_there();
    test_the_drift_is_measured_against_the_document_not_the_empty_destination();
    test_the_two_ends_of_the_coverage_scale_are_exact();
    test_the_lock_band_holds_the_original_at_the_crop_edge();
    test_a_refusal_leaves_every_byte_alone();
    test_the_auto_feather_is_a_fraction_of_the_feature();
    test_an_answer_lands_on_the_crop_it_came_from();
    test_the_flag_decides_and_a_missing_band_matches_nothing();
    test_an_answer_that_changed_only_the_subject_is_left_exactly_alone();
    test_the_drift_on_the_band_is_taken_back_off_the_whole_answer();
    test_a_match_is_identity_when_there_is_no_band();
    test_a_flat_band_shifts_without_flattening();
    test_a_band_with_no_colour_in_it_is_not_colour();
    test_the_spread_correction_stops_short_of_a_palette_change();
    return noise::test::summary(__FILE__);
}
