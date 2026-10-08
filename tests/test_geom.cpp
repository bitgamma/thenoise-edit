// SPDX-License-Identifier: Apache-2.0
#include "test.h"

#include "core/geom.h"

#include <cstdint>
#include <vector>

using namespace noise;

namespace {

// Deterministic generator: no time, no rand().
struct Rng {
    std::uint64_t state = 0x2545F4914F6CDD1Dull;
    double next() {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        return static_cast<double>(state >> 11) / static_cast<double>(1ull << 53);
    }
};

ViewTransform make_view(Extent image, double scale, Vec2 origin) {
    ViewTransform view;
    view.image = image;
    view.scale = scale;
    view.origin = origin;
    return view;
}

void test_view_round_trip() {
    Rng rng;
    for (int i = 0; i < 2000; ++i) {
        const Extent image{1 + static_cast<int>(rng.next() * 4000.0),
                           1 + static_cast<int>(rng.next() * 4000.0)};
        const double scale = 0.02 + rng.next() * 24.0;
        const ViewTransform view = make_view(image, scale, {rng.next() * 500.0 - 250.0,
                                                            rng.next() * 500.0 - 250.0});
        const Vec2 world{rng.next() * static_cast<double>(image.w),
                         rng.next() * static_cast<double>(image.h)};
        const Vec2 back = view.view_to_world(view.world_to_view(world));
        CHECK_NEAR(back.x, world.x, 1e-9);
        CHECK_NEAR(back.y, world.y, 1e-9);
    }
}

void test_zoom_keeps_anchor_fixed() {
    Rng rng;
    const Extent viewport{1600, 900};
    for (int i = 0; i < 2000; ++i) {
        const Extent image{1 + static_cast<int>(rng.next() * 3000.0),
                           1 + static_cast<int>(rng.next() * 3000.0)};
        ViewTransform view = make_view(image, 0.1 + rng.next() * 4.0,
                                       {rng.next() * 200.0, rng.next() * 200.0});
        const Vec2 anchor{rng.next() * viewport.w, rng.next() * viewport.h};
        const double factor = 0.2 + rng.next() * 5.0;
        const Vec2 world_under_anchor = view.view_to_world(anchor);

        // Limits wide enough that the scale clamp cannot interfere with the anchor property.
        const ViewLimits limits{1e-6, 1e6};
        const ViewTransform zoomed = zoom_at(view, anchor, factor, limits);
        const Vec2 still_under = zoomed.view_to_world(anchor);
        CHECK_NEAR(still_under.x, world_under_anchor.x, 1e-6);
        CHECK_NEAR(still_under.y, world_under_anchor.y, 1e-6);
        CHECK_NEAR(zoomed.scale, view.scale * factor, 1e-12);
    }
}

void test_zoom_is_reversible() {
    ViewTransform view = make_view(Extent{2000, 1500}, 0.5, {120.0, 80.0});
    const ViewTransform start = view;
    const Vec2 anchor{333.0, 222.0};
    const ViewLimits limits{0.01, 100.0};
    view = zoom_at(view, anchor, 1.1, limits);
    view = zoom_at(view, anchor, 1.0 / 1.1, limits);
    CHECK_NEAR(view.scale, start.scale, 1e-9);
    CHECK_NEAR(view.origin.x, start.origin.x, 1e-9);
    CHECK_NEAR(view.origin.y, start.origin.y, 1e-9);
}

void test_scale_clamped_to_limits() {
    const Extent image{1000, 1000};
    const Extent viewport{1000, 1000};
    const ViewLimits limits = scale_limits(image, viewport, 0.05, 64.0);
    CHECK_NEAR(limits.min_scale, 0.05, 1e-12);
    CHECK_NEAR(limits.max_scale, 64.0, 1e-12);
    for (const double candidate : {1e-9, 0.01, 0.5, 1.0, 32.0, 1000.0}) {
        const double clamped = clamp_scale(candidate, limits);
        CHECK(clamped >= limits.min_scale - 1e-12);
        CHECK(clamped <= limits.max_scale + 1e-12);
    }
    // A fit fraction above max_scale must not produce min > max.
    const ViewLimits inverted = scale_limits(image, viewport, 2.0, 1.0);
    CHECK(inverted.min_scale <= inverted.max_scale);
}

void test_fit_view_contains_and_centres() {
    struct Case {
        Extent image;
        Extent viewport;
    };
    const Case cases[] = {
        Case{Extent{4000, 1000}, Extent{1280, 800}},
        Case{Extent{100, 100}, Extent{1280, 800}},
        Case{Extent{1280, 800}, Extent{1280, 800}},
        Case{Extent{3, 8000}, Extent{640, 480}},
    };
    for (const Case& c : cases) {
        const ViewTransform view = fit_view(c.image, c.viewport, 16, 64.0);
        CHECK(view.scale <= 64.0 + 1e-12);
        const Vec2 topleft = view.world_to_view({0.0, 0.0});
        const Vec2 bottomright = view.world_to_view(
            {static_cast<double>(c.image.w), static_cast<double>(c.image.h)});
        CHECK(topleft.x >= 15.999);
        CHECK(topleft.y >= 15.999);
        CHECK(bottomright.x <= c.viewport.w - 15.999);
        CHECK(bottomright.y <= c.viewport.h - 15.999);
        const double margin_x = topleft.x;
        const double margin_y = topleft.y;
        // Centred: on the axis the fit constrains, both margins are equal.
        const bool wide = static_cast<double>(c.image.w) / static_cast<double>(c.image.h) >
                          static_cast<double>(c.viewport.w) / static_cast<double>(c.viewport.h);
        if (wide) {
            CHECK_NEAR(margin_x, c.viewport.w - bottomright.x, 1e-6);
        } else {
            CHECK_NEAR(margin_y, c.viewport.h - bottomright.y, 1e-6);
        }
    }
}

void test_clamp_origin_keeps_image_reachable() {
    const Extent viewport{800, 600};
    ViewTransform view = make_view(Extent{2000, 2000}, 1.0, {100000.0, -100000.0});
    view = clamp_origin(view, viewport, 48);
    CHECK(view.origin.x <= 2000.0 - 800.0 + 48.0 + 1e-9);
    CHECK(view.origin.x >= -48.0 - 1e-9);
    CHECK(view.origin.y <= 2000.0 - 600.0 + 48.0 + 1e-9);
    CHECK(view.origin.y >= -48.0 - 1e-9);

    // Smaller than the viewport on an axis: that axis centres.
    const ViewTransform small = make_view(Extent{100, 100}, 1.0, {-1000.0, 5000.0});
    const ViewTransform centred = clamp_origin(small, viewport, 48);
    CHECK_NEAR(centred.origin.x, (100.0 - 800.0) * 0.5, 1e-9);
    CHECK_NEAR(centred.origin.y, (100.0 - 600.0) * 0.5, 1e-9);
}

void test_rect_helpers() {
    const Rect a{10, 10, 100, 100};
    CHECK_EQ(a.right(), 110);
    CHECK(a.contains(10, 10));
    CHECK(a.contains(109, 109));
    CHECK(!a.contains(110, 10));
    CHECK(!a.contains(9, 50));

    const Rect b{100, 100, 50, 50};
    const Rect hit = intersect(a, b);
    CHECK_EQ(hit.x, 100);
    CHECK_EQ(hit.y, 100);
    CHECK_EQ(hit.w, 10);
    CHECK_EQ(hit.h, 10);

    const Rect disjoint = intersect(a, Rect{500, 500, 10, 10});
    CHECK(disjoint.empty());
    const Rect contained = intersect(a, Rect{20, 20, 5, 5});
    CHECK(contained == Rect{20, 20, 5, 5});

    // Overhang on the near edge is dropped from the far edge; a rect wholly outside is
    // empty and its coordinates mean nothing.
    const Rect from_negative = clamp_rect(Rect{-10, -10, 100, 100}, Extent{50, 50});
    const Rect from_inside = clamp_rect(Rect{40, 40, 100, 100}, Extent{50, 50});
    const Rect fully_outside = clamp_rect(Rect{60, 0, 10, 10}, Extent{50, 50});
    CHECK(from_negative == Rect{0, 0, 50, 50});
    CHECK(from_inside == Rect{40, 40, 10, 10});
    CHECK(fully_outside.empty());
}

}

int main() {
    test_view_round_trip();
    test_zoom_keeps_anchor_fixed();
    test_zoom_is_reversible();
    test_scale_clamped_to_limits();
    test_fit_view_contains_and_centres();
    test_clamp_origin_keeps_image_reachable();
    test_rect_helpers();
    return test::summary("geom");
}
