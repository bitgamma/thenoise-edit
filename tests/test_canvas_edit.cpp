// SPDX-License-Identifier: Apache-2.0
// Document geometry, outpaint selection, and editing the AI landing's mask.
#include "test.h"

#include "ai/compose.h"
#include "core/document.h"
#include "core/blur.h"
#include "core/maskops.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <optional>
#include <random>
#include <vector>
#include <utility>

using namespace noise;

namespace {

Image pattern(Extent extent, int bias = 0) {
    Image image = Image::filled(extent, 0, 0, 0, 0);
    for (int y = 0; y < extent.h; ++y) {
        for (int x = 0; x < extent.w; ++x) {
            auto* pixel = image.at(x, y);
            pixel[0] = static_cast<std::uint8_t>((x * 17 + y + bias) % 256);
            pixel[1] = static_cast<std::uint8_t>((y * 23 + x + bias) % 256);
            pixel[2] = static_cast<std::uint8_t>((x * 3 + y * 7 + bias) % 256);
            pixel[3] = static_cast<std::uint8_t>(x % 4 == 0 ? 128 : 255);
        }
    }
    return image;
}

std::vector<Layer> layers(Extent extent) {
    Layer bottom;
    bottom.name = "bottom";
    bottom.pixels = pattern(extent);
    Layer top;
    top.name = "top";
    top.pixels = pattern(extent, 63);
    top.opacity = 0.75;
    Mask mask(extent);
    mask.op(Rect{1, 2, 7, 5}, MaskOp::replace);
    top.mask = std::move(mask);
    std::vector<Layer> result;
    result.push_back(std::move(bottom));
    result.push_back(std::move(top));
    return result;
}

bool same_pixels(const Image& a, const Image& b) {
    return a.size() == b.size() && std::equal(a.pixels().begin(), a.pixels().end(),
                                              b.pixels().begin());
}

bool same_pixel(const Image& a, int ax, int ay, const Image& b, int bx, int by) {
    return std::memcmp(a.at(ax, ay), b.at(bx, by), kChannels) == 0;
}

void test_canvas_resize_moves_every_plane_and_marks_only_new_pixels() {
    const Extent original{13, 10};
    const Extent expanded{19, 15};
    Mask selected(original);
    selected.op(Rect{4, 3, 3, 2}, MaskOp::replace);
    Document doc;
    doc.reset(layers(original), selected, "fixture.noise");
    const std::vector<Layer> before(doc.layers().begin(), doc.layers().end());
    const Image before_composite = doc.composite();
    const std::string before_checksum = doc.checksum();
    const int depth_before = doc.history().undo_depth;
    const std::uint64_t first_geometry = doc.geometry_revision();

    CHECK(doc.resize_canvas(expanded, 4, 2, true));
    CHECK(doc.size() == expanded);
    CHECK_EQ(doc.geometry_revision(), first_geometry + 1);
    CHECK_EQ(doc.history().undo_depth, depth_before + 1);
    REQUIRE(doc.selection().has_value());
    for (int y = 0; y < expanded.h; ++y) {
        for (int x = 0; x < expanded.w; ++x) {
            const bool inside_old = x >= 4 && x < original.w + 4 && y >= 2 && y < original.h + 2;
            CHECK_EQ(doc.selection()->coverage(x, y), inside_old ? kMaskOut : kMaskIn);
            for (std::size_t index = 0; index < before.size(); ++index) {
                const Image& changed = doc.layer(index).pixels;
                if (inside_old) {
                    CHECK(same_pixel(changed, x, y, before[index].pixels, x - 4, y - 2));
                } else {
                    CHECK_EQ(changed.at(x, y)[3], 0);
                }
            }
            if (inside_old) {
                CHECK(same_pixel(doc.composite(), x, y, before_composite, x - 4, y - 2));
            } else {
                CHECK_EQ(doc.composite().at(x, y)[3], 0);
            }
            REQUIRE(doc.layer(1).mask.has_value());
            CHECK_EQ(doc.layer(1).mask->coverage(x, y),
                     inside_old ? before[1].mask->coverage(x - 4, y - 2) : kMaskOut);
        }
    }

    const Image expanded_composite = doc.composite();
    const std::string expanded_checksum = doc.checksum();
    CHECK(doc.undo());
    CHECK(doc.size() == original);
    CHECK_EQ(doc.geometry_revision(), first_geometry + 2);
    CHECK_EQ(doc.checksum(), before_checksum);
    CHECK(doc.selection().has_value() && *doc.selection() == selected);
    for (std::size_t i = 0; i < before.size(); ++i) {
        CHECK_EQ(doc.layer(i).id, before[i].id);
        CHECK(same_pixels(doc.layer(i).pixels, before[i].pixels));
        CHECK(doc.layer(i).mask == before[i].mask);
    }
    CHECK(doc.redo());
    CHECK(doc.size() == expanded);
    CHECK_EQ(doc.geometry_revision(), first_geometry + 3);
    CHECK_EQ(doc.checksum(), expanded_checksum);
    CHECK(same_pixels(doc.composite(), expanded_composite));
    REQUIRE(doc.selection().has_value());
    CHECK_EQ(doc.selection()->coverage(0, 0), kMaskIn);
    CHECK_EQ(doc.selection()->coverage(4, 2), kMaskOut);
}

void test_rotation_moves_all_layer_planes_and_selection_as_one_undo_step() {
    const Extent original{7, 5};
    Mask selected(original);
    selected.op(Rect{1, 2, 3, 2}, MaskOp::replace);
    Document doc;
    doc.reset(layers(original), selected, "rotation.noise");
    doc.select_layer(0);
    const std::vector<Layer> before(doc.layers().begin(), doc.layers().end());
    const Image before_composite = doc.composite();
    const std::string checksum = doc.checksum();
    const int depth = doc.history().undo_depth;
    const std::uint64_t revision = doc.geometry_revision();
    CHECK(doc.rotate_canvas(true));
    CHECK(doc.size() == (Extent{5, 7}));
    CHECK_EQ(doc.geometry_revision(), revision + 1);
    CHECK_EQ(doc.history().undo_depth, depth + 1);
    CHECK_EQ(doc.active(), 0);
    REQUIRE(doc.selection().has_value());
    for (int y = 0; y < original.h; ++y) {
        for (int x = 0; x < original.w; ++x) {
            const int rx = original.h - 1 - y;
            const int ry = x;
            for (std::size_t i = 0; i < before.size(); ++i) {
                CHECK_EQ(doc.layer(i).id, before[i].id);
                CHECK_EQ(doc.layer(i).name, before[i].name);
                CHECK_EQ(doc.layer(i).visible, before[i].visible);
                CHECK_EQ(doc.layer(i).opacity, before[i].opacity);
                CHECK(doc.layer(i).mode == before[i].mode);
                CHECK(same_pixel(doc.layer(i).pixels, rx, ry, before[i].pixels, x, y));
                if (before[i].mask) {
                    REQUIRE(doc.layer(i).mask.has_value());
                    CHECK_EQ(doc.layer(i).mask->coverage(rx, ry), before[i].mask->coverage(x, y));
                }
            }
            CHECK_EQ(doc.selection()->coverage(rx, ry), selected.coverage(x, y));
            CHECK(same_pixel(doc.composite(), rx, ry, before_composite, x, y));
        }
    }
    const std::string rotated_checksum = doc.checksum();
    CHECK(doc.undo());
    CHECK(doc.size() == original);
    CHECK_EQ(doc.checksum(), checksum);
    CHECK(doc.selection().has_value() && *doc.selection() == selected);
    CHECK(doc.redo());
    CHECK(doc.size() == (Extent{5, 7}));
    CHECK_EQ(doc.checksum(), rotated_checksum);
    CHECK(doc.rotate_canvas(false));
    CHECK(doc.size() == original);
    CHECK_EQ(doc.checksum(), checksum);
    CHECK(doc.selection().has_value() && *doc.selection() == selected);
    CHECK(!doc.empty());
}

void test_crop_and_undo_keep_geometry_and_previous_edits() {
    const Extent original{16, 12};
    Document doc;
    doc.reset(layers(original), std::nullopt, "fixture.noise");
    doc.select_layer(0);
    doc.paint(Rect{4, 3, 2, 3}, PaintFence::own_coverage, [](Image& layer) {
        const std::uint8_t ink[4]{200, 80, 17, 255};
        layer.fill_rect(Rect{4, 3, 2, 3}, ink);
    });
    const Image painted = doc.composite();
    const std::string after_paint = doc.checksum();
    CHECK_EQ(doc.history().undo_depth, 1);
    CHECK(!doc.crop_canvas(Rect{-1, 1, 4, 4}));
    CHECK(!doc.crop_canvas(Rect{0, 0, 0, 4}));
    CHECK_EQ(doc.history().undo_depth, 1);

    CHECK(doc.crop_canvas(Rect{3, 2, 8, 7}));
    CHECK(doc.size() == (Extent{8, 7}));
    CHECK_EQ(doc.history().undo_depth, 2);
    for (int y = 0; y < 7; ++y) {
        for (int x = 0; x < 8; ++x) {
            CHECK(same_pixel(doc.composite(), x, y, painted, x + 3, y + 2));
        }
    }
    const std::string cropped = doc.checksum();
    CHECK(doc.undo());
    CHECK(doc.size() == original);
    CHECK_EQ(doc.checksum(), after_paint);
    CHECK(doc.undo());
    CHECK(doc.size() == original);
    CHECK_NE(doc.checksum(), after_paint);
    CHECK(doc.redo());
    CHECK_EQ(doc.checksum(), after_paint);
    CHECK(doc.redo());
    CHECK_EQ(doc.checksum(), cropped);
    CHECK(doc.size() == (Extent{8, 7}));

    CHECK(doc.resize_canvas(Extent{8, 7}, 0, 0, true));
    CHECK_EQ(doc.history().undo_depth, 2); // no-op never creates history
    CHECK(!doc.resize_canvas(Extent{9, 8}, -20, 0, true));
    CHECK(!doc.resize_canvas(Extent{8193, 8193}, 0, 0, true));
}

void test_oversized_geometry_step_can_always_be_undone() {
    const Extent old_size{6, 5};
    const Extent new_size{9, 7};
    History journal(1); // One saved full-canvas geometry step exceeds this deliberately.
    std::vector<Layer> old = layers(old_size);
    std::vector<Layer> current = layers(new_size);
    Mask old_mark(old_size);
    old_mark.op(Rect{1, 1, 2, 2}, MaskOp::replace);
    std::optional<Mask> marked = Mask(new_size);
    marked->op(Rect{0, 0, 2, 7}, MaskOp::replace);
    journal.record_canvas_replaced(std::move(old), old_mark);
    CHECK_EQ(journal.stats().undo_depth, 1);
    REQUIRE(journal.undo(current, marked).has_value());
    CHECK(current[0].pixels.size() == old_size);
    CHECK(marked.has_value() && *marked == old_mark);
    REQUIRE(journal.redo(current, marked).has_value());
    CHECK(current[0].pixels.size() == new_size);
    REQUIRE(marked.has_value());
    CHECK_EQ(marked->coverage(0, 0), kMaskIn);
    CHECK_EQ(marked->coverage(2, 0), kMaskOut);
}

void test_mask_edge_is_hard_soft_and_can_shift_inside_its_canvas() {
    Mask original(Extent{24, 20});
    original.op(Rect{8, 7, 8, 6}, MaskOp::replace);
    const Mask grown = adjusted_edge(original, 2, true, 0);
    const Mask shrunk = adjusted_edge(original, -2, true, 0);
    CHECK_EQ(grown.coverage(6, 9), kMaskIn);
    CHECK_EQ(grown.coverage(5, 9), kMaskOut);
    CHECK_EQ(shrunk.coverage(9, 9), kMaskOut);
    CHECK_EQ(shrunk.coverage(10, 9), kMaskIn);
    CHECK_EQ(grown.coverage(7, 6), kMaskIn);
    CHECK_EQ(shrunk.coverage(15, 9), kMaskOut);
    CHECK_EQ(adjusted_edge(original, 0, false, 0), original);
    const Mask feathered = adjusted_edge(original, 0, true, 2);
    CHECK(feathered.coverage(7, 9) > kMaskOut);
    CHECK(feathered.coverage(7, 9) < kMaskIn);
    CHECK(feathered.coverage(12, 10) > feathered.coverage(7, 9));
}

void test_ai_layer_arrival_keeps_its_mask_in_one_undo_step() {
    const Extent canvas{18, 14};
    Document doc;
    doc.reset(layers(canvas), std::nullopt, "fixture.noise");
    const int before = doc.history().undo_depth;
    const int index = doc.add_layer_written("AI", Rect{3, 4, 10, 8}, [&](Layer& layer) {
        layer.pixels.fill_rect(Rect{3, 4, 10, 8},
                               std::array<std::uint8_t, 4>{30, 190, 80, 255}.data());
        Mask edge(canvas);
        edge.op(Rect{6, 6, 4, 3}, MaskOp::replace);
        layer.mask = std::move(edge);
        return true;
    });
    REQUIRE(index == 2);
    REQUIRE(doc.layer(2).mask.has_value());
    const Image after = doc.composite();
    CHECK_EQ(doc.history().undo_depth, before + 1);
    CHECK(doc.undo());
    CHECK_EQ(doc.layers().size(), std::size_t(2));
    CHECK(doc.redo());
    REQUIRE(doc.layer(2).mask.has_value());
    CHECK_EQ(doc.layer(2).mask->coverage(5, 6), kMaskOut);
    CHECK_EQ(doc.layer(2).mask->coverage(6, 6), kMaskIn);
    CHECK(same_pixels(doc.composite(), after));
}

void test_ai_result_stays_editable_without_changing_uncovered_pixels() {
    const Extent canvas{26, 20};
    const Image base = pattern(canvas);
    Mask selection(canvas);
    selection.op(Rect{8, 7, 6, 5}, MaskOp::replace);
    ai::RegionPlan plan;
    plan.crop = Rect{4, 4, 14, 12};
    plan.requested = Extent{14, 12};
    const Image answer = Image::filled(plan.requested, 250, 12, 30, 255);
    const ai::CompositePolicy policy{0, false, 0};

    Image baked = Image::filled(canvas, 0, 0, 0, 0);
    Image editable = Image::filled(canvas, 0, 0, 0, 0);
    Mask edge;
    const auto legacy = ai::compose_result(baked, base, answer, plan, selection, policy);
    const auto modern = ai::compose_editable_result(editable, edge, base, answer, plan, selection,
                                                     policy);
    REQUIRE(legacy.composed);
    REQUIRE(modern.composed);
    CHECK(legacy.coverage == edge);
    CHECK_EQ(modern.feather_px, 0);
    CHECK_EQ(modern.pixels_changed, legacy.pixels_changed);
    // Pixel data exists behind the initially hidden rim of the editable answer.
    CHECK_EQ(baked.at(7, 9)[3], 0);
    CHECK_EQ(editable.at(7, 9)[3], 255);
    CHECK_EQ(edge.coverage(7, 9), 0);

    Layer paper;
    paper.name = "paper";
    paper.pixels = base;
    Layer old_result;
    old_result.name = "old result";
    old_result.pixels = baked;
    Layer new_result;
    new_result.name = "new result";
    new_result.pixels = editable;
    new_result.mask = edge;
    Document flattened;
    Document adjustable;
    flattened.reset({paper, old_result}, std::nullopt, "a.noise");
    adjustable.reset({paper, new_result}, std::nullopt, "b.noise");
    CHECK(same_pixels(flattened.composite(), adjustable.composite()));
    const Image original_display = adjustable.composite();

    const Mask wider = adjusted_edge(edge, 1, true, 0);
    CHECK(adjustable.set_layer_mask(1, wider));
    CHECK_EQ(adjustable.composite().at(6, 9)[0], base.at(6, 9)[0]);
    CHECK_NE(adjustable.composite().at(7, 9)[0], base.at(7, 9)[0]);
    CHECK_NE(adjustable.composite().at(8, 6)[0], base.at(8, 6)[0]);
    for (int y = 0; y < canvas.h; ++y) {
        for (int x = 0; x < canvas.w; ++x) {
            if (wider.coverage(x, y) == kMaskOut) {
                CHECK(same_pixel(adjustable.composite(), x, y, base, x, y));
            }
        }
    }
    CHECK(adjustable.undo());
    CHECK(same_pixels(adjustable.composite(), original_display));
    CHECK(adjustable.redo());
    CHECK(adjustable.layer(1).mask == wider);
}

void test_outpaint_fills_transparent_expansion_without_alpha_seams() {
    const Extent original{10, 8};
    const Extent expanded{18, 8};
    Layer base;
    base.name = "original";
    base.pixels = Image::filled(original, 40, 60, 90, 255);
    Document doc;
    doc.reset({base}, std::nullopt, "picture.noise");
    REQUIRE(doc.resize_canvas(expanded, 4, 0, true));
    REQUIRE(doc.selection().has_value());
    const Mask added = *doc.selection();
    const Image reference = doc.composite();
    const Image answer = Image::filled(expanded, 200, 170, 130, 255);
    ai::RegionPlan plan;
    plan.crop = Rect{0, 0, expanded.w, expanded.h};
    plan.requested = expanded;
    for (const bool match_colour : {false, true}) {
        const ai::CompositePolicy policy{match_colour ? std::nullopt : std::optional<int>(3),
                                         match_colour, match_colour ? 4 : 0};
        Image patch = Image::filled(expanded, 0, 0, 0, 0);
        Mask edge;
        const auto report = ai::compose_editable_result(patch, edge, reference, answer, plan,
                                                        added, policy);
        REQUIRE(report.composed);
        for (int y = 0; y < expanded.h; ++y) {
            for (int x = 0; x < expanded.w; ++x) {
                const bool new_area = x < 4 || x >= 14;
                if (new_area) {
                    CHECK_EQ(edge.coverage(x, y), kMaskIn);
                }
            }
        }
        CHECK(edge.coverage(4, 4) > kMaskOut);
        CHECK(edge.coverage(4, 4) < kMaskIn);
        Layer result_layer;
        result_layer.name = "outpaint";
        result_layer.pixels = std::move(patch);
        result_layer.mask = std::move(edge);
        Document composed;
        composed.reset({doc.layer(0), result_layer}, std::nullopt, "result.noise");
        const Image& visible = composed.composite();
        for (int y = 0; y < expanded.h; ++y) {
            for (int x = 0; x < expanded.w; ++x) {
                const bool new_area = x < 4 || x >= 14;
                if (new_area) {
                    CHECK_EQ(visible.at(x, y)[3], 255);
                    if (!match_colour) {
                        CHECK_EQ(visible.at(x, y)[0], 200);
                    }
                } else if (composed.layer(1).mask->coverage(x, y) == 0) {
                    CHECK(same_pixel(visible, x, y, reference, x, y));
                }
            }
        }
    }
}

// Live edge controls keep one mask as their reference across multiple drags. Using the
// provisional document API verifies the same full-plane restoration used by the UI.
void test_absolute_edge_reversal_restores_every_original_mask_byte() {
    const Extent size{100, 80};
    Layer layer;
    layer.name = "generated";
    layer.pixels = Image::filled(size, 120, 140, 160, 255);
    Mask hard(size);
    hard.op(Rect{26, 19, 40, 35}, MaskOp::replace);
    const Mask source = feathered_mask(hard, 6);
    layer.mask = source;
    Document doc;
    doc.reset({layer}, std::nullopt, "masked.noise");
    const std::string first_checksum = doc.checksum();
    const Rect canvas{0, 0, size.w, size.h};
    const auto place = [&](int shift, int feather) {
        REQUIRE(doc.begin_provisional(canvas, LayerPlane::mask));
        const Mask changed = (shift == 0 && feather == 6)
            ? source : adjusted_edge(source, shift, true, feather);
        doc.repaint_provisional_mask(canvas, PaintFence::own_coverage,
                                     [&changed](Mask& mask, Rect) { mask = changed; });
        doc.commit_provisional();
    };
    place(10, 6);
    CHECK(doc.checksum() != first_checksum);
    place(0, 6);
    CHECK_EQ(doc.checksum(), first_checksum);
    place(-10, 6);
    CHECK(doc.checksum() != first_checksum);
    place(0, 6);
    CHECK_EQ(doc.checksum(), first_checksum);
    place(0, 0);
    CHECK_EQ(doc.layer(0).mask->coverage(32, 25), kMaskIn);
    CHECK(doc.checksum() != first_checksum);
    place(0, 6);
    CHECK_EQ(doc.checksum(), first_checksum);
    place(20, 0);
    place(0, 6);
    CHECK_EQ(doc.checksum(), first_checksum);
}

void test_edge_regions_match_whole_canvas_for_soft_and_hard_masks() {
    std::mt19937 random(2026);
    for (const Extent size : {Extent{2, 3}, Extent{7, 11}, Extent{32, 24},
                              Extent{63, 47}}) {
        for (int sample = 0; sample < 8; ++sample) {
            std::vector<std::uint8_t> pixels(static_cast<std::size_t>(size.w) *
                                             static_cast<std::size_t>(size.h));
            for (auto& value : pixels) {
                const unsigned r = static_cast<unsigned>(random() % 100u);
                value = sample < 3 ? (r < static_cast<unsigned>(sample * 25) ? 255u : 0u)
                                   : static_cast<std::uint8_t>(random() % 256u);
            }
            const Mask source = *Mask::from_plane(size, std::move(pixels));
            for (const int shift : {-11, -3, 0, 1, 6}) {
                for (const int feather : {0, 1, 4}) {
                    for (const bool hard : {false, true}) {
                        const Mask reference = adjusted_edge(source, shift, hard, feather);
                        const Mask cropped = adjusted_edge_region(source, shift, hard, feather,
                                                                 Rect{0, 0, size.w, size.h});
                        CHECK(cropped == reference);
                        const Rect support = adjusted_edge_bounds(source, shift, feather);
                        for (int y = 0; y < size.h; ++y) {
                            for (int x = 0; x < size.w; ++x) {
                                if (!support.contains(x, y)) {
                                    CHECK_EQ(reference.coverage(x, y), 0);
                                }
                            }
                        }
                        const int x = static_cast<int>(random() % static_cast<unsigned>(size.w));
                        const int y = static_cast<int>(random() % static_cast<unsigned>(size.h));
                        const Rect small{x, y, std::min(3, size.w - x),
                                         std::min(4, size.h - y)};
                        const Mask slice = adjusted_edge_region(source, shift, hard, feather, small);
                        CHECK(slice == reference.region(small));
                        if (hard) {
                            std::vector<std::uint8_t> binary = source.plane();
                            for (std::uint8_t& value : binary) {
                                value = value >= 128 ? kMaskIn : kMaskOut;
                            }
                            const Mask hard_baseline = *Mask::from_plane(size, std::move(binary));
                            CHECK(reference == adjusted_edge(hard_baseline, shift, false, feather));
                        }
                    }
                }
            }
        }
    }
}

void test_uniform_masks_skip_work_but_keep_the_edge_gesture() {
    const Extent size{64, 53};
    for (const std::uint8_t value : {kMaskOut, kMaskIn}) {
        Mask baseline(size);
        baseline.fill(value);
        Layer layer;
        layer.name = "uniform";
        layer.pixels = Image::filled(size, 120, 80, 160, 255);
        layer.mask = baseline;
        Document doc;
        doc.reset({layer}, std::nullopt, "uniform.noise");
        const int original_depth = doc.history().undo_depth;
        for (int shift : {-20, 0, 8}) {
            for (int feather : {0, 4, 17}) {
                const Rect small{0, 0, 1, 1};
                const Mask patch = adjusted_edge_region(baseline, shift, true, feather, small);
                CHECK(patch == baseline.region(small));
                CHECK(adjusted_edge(baseline, shift, true, feather) == baseline);
                REQUIRE(doc.begin_provisional(small, LayerPlane::mask));
                doc.repaint_provisional_mask(small, PaintFence::own_coverage,
                    [&](Mask& mask, Rect) { REQUIRE(mask.op(small, patch.plane(), MaskOp::replace)); });
                doc.commit_provisional();
                CHECK(doc.layer(0).mask == baseline);
                CHECK_EQ(doc.history().undo_depth, original_depth);
            }
        }
    }
}

void test_partial_edge_preview_matches_full_replacement_and_undo() {
    const Extent size{340, 260};
    Mask hard(size);
    hard.op(Rect{111, 75, 85, 70}, MaskOp::replace);
    const Mask baseline = feathered_mask(hard, 6);
    Layer layer;
    layer.name = "partial edge";
    layer.pixels = Image::filled(size, 105, 180, 240, 255);
    layer.mask = baseline;
    Document doc;
    doc.reset({layer}, std::nullopt, "fixture.noise");
    Document reference;
    reference.reset({layer}, std::nullopt, "fixture.noise");
    const std::string original = doc.checksum();
    int previous_shift = 0;
    int previous_feather = 6;
    for (const auto& [shift, feather] : std::array<std::pair<int, int>, 8>{
             {{10, 6}, {11, 6}, {2, 0}, {0, 6}, {-8, 0}, {-2, 14}, {0, 0}, {0, 6}}}) {
        const Rect changed = unite(adjusted_edge_bounds(baseline, previous_shift, previous_feather),
                                   adjusted_edge_bounds(baseline, shift, feather));
        REQUIRE(!changed.empty());
        REQUIRE(doc.begin_provisional(changed, LayerPlane::mask));
        const Mask patch = (shift == 0 && feather == 6)
                               ? baseline.region(changed)
                               : adjusted_edge_region(baseline, shift, true, feather, changed);
        doc.repaint_provisional_mask(changed, PaintFence::own_coverage,
            [&](Mask& mask, Rect) { REQUIRE(mask.op(changed, patch.plane(), MaskOp::replace)); });
        const Mask wanted = (shift == 0 && feather == 6)
                                ? baseline : adjusted_edge(baseline, shift, true, feather);
        REQUIRE(doc.layer(0).mask.has_value());
        CHECK(*doc.layer(0).mask == wanted);
        REQUIRE(reference.set_layer_mask(0, wanted));
        CHECK(std::equal(doc.composite().pixels().begin(), doc.composite().pixels().end(),
                         reference.composite().pixels().begin()));
        doc.commit_provisional();
        previous_shift = shift;
        previous_feather = feather;
    }
    CHECK_EQ(doc.checksum(), original);
    CHECK(doc.undo());
    CHECK(doc.checksum() != original);
    CHECK(doc.redo());
    CHECK_EQ(doc.checksum(), original);
}

}  // namespace

int main() {
    test_canvas_resize_moves_every_plane_and_marks_only_new_pixels();
    test_rotation_moves_all_layer_planes_and_selection_as_one_undo_step();
    test_crop_and_undo_keep_geometry_and_previous_edits();
    test_oversized_geometry_step_can_always_be_undone();
    test_mask_edge_is_hard_soft_and_can_shift_inside_its_canvas();
    test_ai_layer_arrival_keeps_its_mask_in_one_undo_step();
    test_ai_result_stays_editable_without_changing_uncovered_pixels();
    test_outpaint_fills_transparent_expansion_without_alpha_seams();
    test_absolute_edge_reversal_restores_every_original_mask_byte();
    test_edge_regions_match_whole_canvas_for_soft_and_hard_masks();
    test_uniform_masks_skip_work_but_keep_the_edge_gesture();
    test_partial_edge_preview_matches_full_replacement_and_undo();
    return noise::test::summary("canvas_edit");
}
