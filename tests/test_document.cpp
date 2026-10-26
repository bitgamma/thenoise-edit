// SPDX-License-Identifier: Apache-2.0
#include "test.h"

#include "core/document.h"
#include "core/mask.h"
#include "core/maskops.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cmath>
#include <cstring>
#include <limits>
#include <functional>
#include <map>
#include <span>
#include <string>
#include <vector>

using namespace noise;

namespace {

std::vector<Layer> stack(std::vector<std::pair<std::string, Image>> contents) {
    std::vector<Layer> layers;
    for (auto& [name, pixels] : contents) {
        Layer layer;
        layer.name = std::move(name);
        layer.pixels = std::move(pixels);
        layers.push_back(std::move(layer));
    }
    return layers;
}

// The stack as "bottom,middle,top", bottom first: the shape a reorder or a flatten is claimed against.
std::string layer_names(const Document& doc) {
    std::string out;
    for (const Layer& layer : doc.layers()) {
        if (!out.empty()) {
            out += ",";
        }
        out += layer.name;
    }
    return out;
}

// The stack's identities, bottom first. Names can be typed twice; these cannot.
std::string layer_ids(const Document& doc) {
    std::string out;
    for (const Layer& layer : doc.layers()) {
        if (!out.empty()) {
            out += ",";
        }
        out += std::to_string(layer.id);
    }
    return out;
}

// What each layer's own counter says, keyed by layer: the fact a viewer of one layer - a thumbnail -
// caches against, seen from outside the document. Keyed rather than listed, because a reorder moves a
// layer between indices, so a list taken by index cannot tell "unchanged" from "carried off with it".
std::map<std::uint64_t, std::uint64_t> content_revisions(const Document& doc) {
    std::map<std::uint64_t, std::uint64_t> out;
    for (const Layer& layer : doc.layers()) {
        out[layer.id] = layer.content_revision;
    }
    return out;
}

// A canvas with one rectangle of solid grey on it: the picture a scale is aimed at, with everything
// around it transparent so "nothing else moved" is checkable.
Image block(Extent canvas, Rect on, std::uint8_t grey) {
    Image image = Image::filled(canvas, 0, 0, 0, 0);
    const std::uint8_t rgba[4] = {grey, grey, grey, 255};
    image.fill_rect(on, rgba);
    return image;
}

std::string rect_text(Rect r) {
    return std::to_string(r.x) + "," + std::to_string(r.y) + "," + std::to_string(r.w) + "x" +
           std::to_string(r.h);
}

// A coat of flat ink for `area`, in the units `Mask::paint` wants: one byte per pixel of the region.
std::vector<std::uint8_t> coat(Rect area, std::uint8_t value) {
    return std::vector<std::uint8_t>(static_cast<std::size_t>(area.w) *
                                         static_cast<std::size_t>(area.h),
                                     value);
}

void paint_on(Document& doc, Rect rect, std::uint8_t grey) {
    doc.paint(rect, PaintFence::selection, [rect, grey](Image& image) {
        const std::uint8_t rgba[4] = {grey, grey, grey, 255};
        image.fill_rect(rect, rgba);
    });
}

// The composite is repaired over the rectangles that changed, so the load-bearing property is
// that it ends up identical to one built from scratch. A partial repair that missed a region, or
// blended a layer twice, shows up here and nowhere else.
void composite_matches_scratch(Document& live, const char* where) {
    std::vector<Layer> copy;
    for (const Layer& layer : live.layers()) {
        Layer clone;
        clone.name = layer.name;
        clone.pixels = layer.pixels;
        clone.visible = layer.visible;
        clone.opacity = layer.opacity;
        clone.mode = layer.mode;
        clone.mask = layer.mask;
        copy.push_back(std::move(clone));
    }
    Document fresh;
    fresh.reset(std::move(copy), std::nullopt, "x.noise");
    const std::string built = live.composite().checksum_hex();
    const std::string scratch = fresh.composite().checksum_hex();
    if (built == scratch) {
        ++noise::test::checks;
        return;
    }
    noise::test::report("incremental composite == composite from scratch", __FILE__, __LINE__,
                        std::string(where) + ": got " + built + " want " + scratch);
}

void test_the_incremental_composite_equals_one_built_from_scratch() {
    Document live;
    live.reset(stack({{"Paper", Image::filled(Extent{24, 18}, 20, 20, 20, 255)}}), std::nullopt, "x.noise");

    auto check_against_scratch = [&live](const char* where) { composite_matches_scratch(live, where); };

    check_against_scratch("one layer, nothing painted");
    paint_on(live, Rect{1, 1, 5, 4}, 200);
    check_against_scratch("after a stroke");
    live.add_layer("Mid", Rgba8{0, 0, 0, 0});
    check_against_scratch("after adding a layer");
    paint_on(live, Rect{10, 2, 6, 6}, 30);
    check_against_scratch("on the new layer");
    live.select_layer(0);
    paint_on(live, Rect{4, 10, 8, 3}, 120);
    check_against_scratch("on the bottom layer, under a semi-transparent one");
    live.add_layer("Top", Rgba8{0, 0, 0, 0});
    paint_on(live, Rect{0, 0, 24, 18}, 90);
    check_against_scratch("a layer covering everything");
    live.undo();
    check_against_scratch("after undo");
    live.undo();
    check_against_scratch("after a second undo");
    live.redo();
    check_against_scratch("after redo");
    live.remove_layer(1);
    check_against_scratch("after removing a middle layer");
    CHECK_EQ(live.layers().size(), 2u);
    // Visibility and opacity widen the pending region to the whole canvas, and a repair that
    // blended a layer it should have skipped - or skipped one it should have faded - only shows
    // up against a stack built from nothing.
    live.set_layer_visible(live.active(), false);
    check_against_scratch("with the top layer hidden");
    live.set_layer_opacity(0, 0.4);
    check_against_scratch("with the bottom layer faded");
    live.set_layer_visible(live.active(), true);
    check_against_scratch("with both visible and one faded");
    paint_on(live, Rect{2, 2, 3, 3}, 250);
    check_against_scratch("a stroke over a faded stack");
    // A mode change widens the pending region the same way, and a repair that blended a layer with
    // the wrong arithmetic - or with the arithmetic of the mode it used to have - only shows up
    // against a stack built from nothing.
    live.set_layer_mode(1, BlendMode::multiply);
    check_against_scratch("with a layer multiplying what is under it");
    live.set_layer_mode(1, BlendMode::color_dodge);
    check_against_scratch("with a layer dodging what is under it");
    live.set_layer_mode(0, BlendMode::screen);
    check_against_scratch("with the bottom layer screening too");
    live.set_layer_mode(1, BlendMode::normal);
    check_against_scratch("back to normal, one mode still changed");
    // A mask widens the pending region over the boxes the old and the new mask cover, which is the
    // one place a repair can be *narrower* than the change: a mask moved across the canvas leaves
    // pixels behind that a full rebuild would have taken care of.
    Mask corner(Extent{24, 18});
    corner.op(Rect{2, 2, 5, 4}, MaskOp::replace);
    live.set_layer_mask(1, corner);
    check_against_scratch("with a layer masked to a corner");
    Mask elsewhere(Extent{24, 18});
    elsewhere.op(Rect{14, 10, 6, 6}, MaskOp::replace);
    live.set_layer_mask(1, elsewhere);
    check_against_scratch("with the same layer masked somewhere else");
    live.set_layer_mask(0, Mask(Extent{24, 18}));
    check_against_scratch("with the bottom layer masked to nothing at all");
    live.set_layer_mask(1, std::nullopt);
    check_against_scratch("with the mask taken off again");
}

// `Document::composite` hands back a lone layer's own pixels instead of blending it. That is legal
// for a mode that is not `normal` only because a layer over nothing has no second operand, and the
// picture it makes has to be the same one the blend path makes - alpha identical everywhere, colour
// identical wherever there is coverage. Where there is no coverage the two paths may hold different
// colour bytes, and no renderer can tell.
void test_a_lone_layers_composite_is_the_blend_paths_answer_for_every_mode() {
    const Extent size{6, 5};
    Image pixels = Image::filled(size, 0, 0, 0, 0);
    for (int y = 0; y < size.h; ++y) {
        for (int x = 0; x < size.w; ++x) {
            std::uint8_t* here = pixels.at(x, y);
            here[0] = static_cast<std::uint8_t>(30 + x * 30);
            here[1] = static_cast<std::uint8_t>(200 - y * 20);
            here[2] = 90;
            // Coverage including none at all, and colour under the none, which is the only place
            // the two paths are allowed to disagree.
            here[3] = static_cast<std::uint8_t>((x + y) % 3 == 0 ? 0 : 40 + 100 * ((x + y) % 3));
        }
    }
    for (const BlendModeInfo& info : kBlendModes) {
        Layer lone;
        lone.name = "Lone";
        lone.pixels = pixels;
        lone.mode = info.mode;
        Document one;
        std::vector<Layer> stack;
        stack.push_back(lone);
        one.reset(std::move(stack), std::nullopt, "x.noise");

        Image blended = Image::filled(size, 0, 0, 0, 0);
        blend_layer(blended, pixels, Rect{0, 0, size.w, size.h}, info.mode, 1.0);

        const Image& got = one.composite();
        for (int y = 0; y < size.h; ++y) {
            for (int x = 0; x < size.w; ++x) {
                const std::uint8_t* here = got.at(x, y);
                const std::uint8_t* want = blended.at(x, y);
                CHECK_EQ(static_cast<int>(here[3]), static_cast<int>(want[3]));
                if (want[3] > 0) {
                    for (int channel = 0; channel < 3; ++channel) {
                        CHECK_EQ(static_cast<int>(here[channel]),
                                 static_cast<int>(want[channel]));
                    }
                }
            }
        }
    }
}

// The shortcut that hands a lone layer's own pixels straight back is only correct at full
// strength, and the failure if it is not is a canvas that ignores the opacity slider whenever the
// document happens to have one layer - which is most of them, most of the time.
void test_a_lone_layer_is_shown_whole_only_at_full_strength() {
    const Extent size{5, 4};
    Image pixels = Image::filled(size, 0, 0, 0, 0);
    for (int y = 0; y < size.h; ++y) {
        for (int x = 0; x < size.w; ++x) {
            std::uint8_t* here = pixels.at(x, y);
            here[0] = static_cast<std::uint8_t>(20 + x * 40);
            here[1] = 60;
            here[2] = static_cast<std::uint8_t>(250 - y * 30);
            here[3] = static_cast<std::uint8_t>((x + y) % 2 == 0 ? 0 : 90 + 50 * ((x + y) % 3));
        }
    }
    Document doc;
    doc.reset(stack({{"Lone", pixels}}), std::nullopt, "x.noise");
    CHECK(doc.set_layer_mode(0, BlendMode::overlay));
    // Whole: same coverage everywhere, same colour wherever there is any. Not "the same bytes",
    // because a pixel with no coverage keeps whatever colour its destination held and the shortcut
    // and the blend path hold different ones there - which no renderer can show. That the shortcut
    // is taken at all is a cost question for the probe to answer, not a property to assert.
    const Image& whole = doc.composite();
    for (int y = 0; y < size.h; ++y) {
        for (int x = 0; x < size.w; ++x) {
            const std::uint8_t* want = pixels.at(x, y);
            const std::uint8_t* got = whole.at(x, y);
            CHECK_EQ(static_cast<int>(got[3]), static_cast<int>(want[3]));
            if (want[3] > 0) {
                for (int channel = 0; channel < 3; ++channel) {
                    CHECK_EQ(static_cast<int>(got[channel]), static_cast<int>(want[channel]));
                }
            }
        }
    }

    // Half: every covered pixel keeps its colour and loses half its coverage, and a pixel with no
    // coverage stays a pixel with no coverage. Only the covered ones are compared, because the
    // colour under zero alpha is not on screen in either path.
    CHECK(doc.set_layer_opacity(0, 0.5));
    const Image& faded = doc.composite();
    for (int y = 0; y < size.h; ++y) {
        for (int x = 0; x < size.w; ++x) {
            const std::uint8_t* want = pixels.at(x, y);
            const std::uint8_t* got = faded.at(x, y);
            if (want[3] == 0) {
                CHECK_EQ(static_cast<int>(got[3]), 0);
                continue;
            }
            CHECK_EQ(static_cast<int>(got[3]), (static_cast<int>(want[3]) + 1) / 2);
            for (int channel = 0; channel < 3; ++channel) {
                CHECK_EQ(static_cast<int>(got[channel]), static_cast<int>(want[channel]));
            }
        }
    }
}

// Setting a mode is layer state, so it has to do exactly what visibility does and nothing more:
// move the picture, mark the document dirty, bump the revision the renderer uploads on, and leave
// both the pixels and the undo stack alone.
void test_setting_a_mode_moves_the_picture_and_touches_nothing_else() {
    Document doc;
    doc.reset(stack({{"Paper", Image::filled(Extent{8, 6}, 200, 100, 50, 255)},
                     {"Ink", Image::filled(Extent{8, 6}, 128, 128, 128, 255)}}),
              std::nullopt, "x.noise");
    const std::string normal = doc.composite().checksum_hex();
    const std::string ink_before = doc.layers()[1].pixels.checksum_hex();
    const int undos_before = doc.history().undo_depth;
    const std::uint64_t revision = doc.revision();

    CHECK(doc.set_layer_mode(1, BlendMode::multiply));
    CHECK_NE(doc.composite().checksum_hex(), normal);
    // Red under a 50 % grey, multiplied: the arithmetic of `test_blend` at the document level, so
    // a mode wired to the wrong layer order or the wrong operand shows here.
    CHECK_EQ(static_cast<int>(doc.composite().at(0, 0)[0]), 100);
    CHECK_EQ(static_cast<int>(doc.composite().at(0, 0)[1]), 50);
    CHECK_EQ(static_cast<int>(doc.composite().at(0, 0)[2]), 25);
    CHECK_EQ(static_cast<int>(doc.composite().at(0, 0)[3]), 255);
    CHECK(doc.dirty());
    CHECK(doc.revision() > revision);
    CHECK_EQ(doc.layers()[1].pixels.checksum_hex(), ink_before);
    CHECK_EQ(doc.history().undo_depth, undos_before);

    // The same mode again is not a change, and an index outside the stack is a false rather than a
    // clamp onto somebody else's layer.
    const std::uint64_t after = doc.revision();
    CHECK(doc.set_layer_mode(1, BlendMode::multiply));
    CHECK_EQ(doc.revision(), after);
    CHECK(!doc.set_layer_mode(2, BlendMode::screen));
    CHECK(!doc.set_layer_mode(-1, BlendMode::screen));
}

// A hidden layer is not in the picture at all: the composite is the image the stack makes
// without it, byte for byte, and the layer itself is untouched - hiding is not erasing.
void test_a_hidden_layer_contributes_nothing() {
    const Extent size{8, 6};
    const Image bottom = Image::filled(size, 10, 20, 30, 255);
    const Image top = Image::filled(size, 200, 0, 0, 255);

    Document doc;
    doc.reset(stack({{"Bottom", bottom}, {"Top", top}}), std::nullopt, "x.noise");
    const std::string with_top = doc.composite().checksum_hex();

    CHECK(doc.set_layer_visible(1, false));
    Document without;
    without.reset(stack({{"Bottom", bottom}}), std::nullopt, "x.noise");
    CHECK_EQ(doc.composite().checksum_hex(), without.composite().checksum_hex());
    CHECK_NE(with_top, doc.composite().checksum_hex());
    CHECK_EQ(doc.layer(1).pixels.checksum_hex(), top.checksum_hex());

    // The single-layer shortcut has to respect it too: one hidden layer is a transparent canvas,
    // not that layer handed straight to the renderer.
    Document single;
    single.reset(stack({{"Only", top}}), std::nullopt, "x.noise");
    CHECK(single.set_layer_visible(0, false));
    CHECK(!single.composite().empty());
    CHECK_EQ(single.composite().checksum_hex(), Image::filled(size, 0, 0, 0, 0).checksum_hex());
}

// A layer's opacity scales the layer's own alpha where the stack is flattened, and nothing else:
// the numbers are worked out here rather than read back out of the code.
void test_layer_opacity_scales_the_blend() {
    const Extent size{4, 4};
    const std::uint8_t transparent[4] = {0, 0, 0, 0};

    Image on_red = Image::filled(size, 255, 0, 0, 255);
    Image green = Image::filled(size, 0, 255, 0, 255);
    blend_layer(on_red, green, Rect{0, 0, 4, 4}, BlendMode::normal, 0.5);
    // Half of an opaque green over opaque red: both alpha terms are 0.5, so the result is opaque
    // and each channel is 127.5, which rounds to 128.
    CHECK_EQ(static_cast<int>(on_red.at(0, 0)[0]), 128);
    CHECK_EQ(static_cast<int>(on_red.at(0, 0)[1]), 128);
    CHECK_EQ(static_cast<int>(on_red.at(0, 0)[3]), 255);

    // Over nothing, the colour survives whole and only the alpha carries the layer's 50 %.
    Image on_empty = Image::filled(size, 0, 0, 0, 0);
    blend_layer(on_empty, green, Rect{0, 0, 4, 4}, BlendMode::normal, 0.5);
    CHECK_EQ(static_cast<int>(on_empty.at(0, 0)[1]), 255);
    CHECK_EQ(static_cast<int>(on_empty.at(0, 0)[3]), 128);

    // A pixel the layer does not paint stays exactly as it was, whatever the layer's opacity.
    Image hole = Image::filled(size, 0, 255, 0, 255);
    hole.fill_rect(Rect{1, 1, 2, 2}, transparent);
    Image base = Image::filled(size, 9, 9, 9, 255);
    blend_layer(base, hole, Rect{0, 0, 4, 4}, BlendMode::normal, 0.25);
    CHECK_EQ(static_cast<int>(base.at(1, 1)[0]), 9);
    CHECK_EQ(static_cast<int>(base.at(1, 1)[3]), 255);

    // Through the document, the same two ends of the slider are the two images that can be named
    // without recomputing a blend: 0 is the bottom layer, 1 is the stack as it was.
    Document doc;
    doc.reset(stack({{"Bottom", Image::filled(size, 255, 0, 0, 255)},
                     {"Top", Image::filled(size, 0, 255, 0, 255)}}),
              std::nullopt, "x.noise");
    const std::string shown = doc.composite().checksum_hex();
    CHECK(doc.set_layer_opacity(1, 0.0));
    Document only_bottom;
    only_bottom.reset(stack({{"Bottom", Image::filled(size, 255, 0, 0, 255)}}), std::nullopt, "x.noise");
    CHECK_EQ(doc.composite().checksum_hex(), only_bottom.composite().checksum_hex());
    CHECK(doc.set_layer_opacity(1, 1.0));
    CHECK_EQ(doc.composite().checksum_hex(), shown);
}

// The renderer uploads on the revision, so a change that alters what the canvas shows without
// altering a pixel has to bump it - or a hidden layer stays on screen until something else moves.
void test_layer_state_changes_reach_the_renderer() {
    Document doc;
    doc.reset(stack({{"Bottom", Image::filled(Extent{4, 4}, 1, 1, 1, 255)},
                     {"Top", Image::filled(Extent{4, 4}, 2, 2, 2, 255)}}),
              std::nullopt, "x.noise");
    doc.composite();
    const std::uint64_t before = doc.revision();
    CHECK(doc.set_layer_visible(1, false));
    CHECK(doc.revision() > before);
    CHECK(doc.set_layer_opacity(1, 0.5));
    CHECK(doc.revision() > before + 1);
    CHECK(doc.dirty());

    // Setting what it already is changes nothing, and claiming so is the point: a panel that
    // dragged a revision per frame would re-upload a canvas-sized texture per frame.
    const std::uint64_t settled = doc.revision();
    CHECK(doc.set_layer_visible(1, false));
    CHECK(doc.set_layer_opacity(1, 0.5));
    CHECK_EQ(doc.revision(), settled);
}

// Out of range is a refusal that leaves the layer alone, not a clamped value.
void test_layer_state_refuses_what_was_asked_for() {
    Document doc;
    doc.reset(stack({{"Only", Image::filled(Extent{4, 4}, 1, 1, 1, 255)}}), std::nullopt, "x.noise");
    CHECK(!doc.set_layer_opacity(0, 1.5));
    CHECK(!doc.set_layer_opacity(0, -0.5));
    CHECK(!doc.set_layer_opacity(-1, 0.5));
    CHECK(!doc.set_layer_opacity(1, 0.5));
    CHECK_EQ(doc.layer(0).opacity, 1.0);
    CHECK(!doc.set_layer_visible(3, false));
    CHECK(doc.layer(0).visible);
    CHECK(doc.set_layer_opacity(0, 0.0));
    CHECK(doc.set_layer_opacity(0, 1.0));
}

// Two layers of the same size and shape, one over the other: what lands on the canvas is the
// blend, not the top layer and not the sum.
void test_the_stack_blends_bottom_first() {
    const Extent size{6, 4};
    Image bottom = Image::filled(size, 255, 0, 0, 255);
    Image top = Image::filled(size, 0, 0, 0, 0);
    const std::uint8_t half_green[4] = {0, 255, 0, 128};
    top.fill_rect(Rect{0, 0, 6, 4}, half_green);

    Document doc;
    doc.reset(stack({{"Bottom", bottom}, {"Top", top}}), std::nullopt, "x.noise");
    const Image& out = doc.composite();
    // Alpha 128 is not exactly a half: 128/255 over opaque red keeps 127/255 of the red, and
    // contributes 128/255 of the green. Both channels survive, alpha stays opaque.
    CHECK_EQ(static_cast<int>(out.at(0, 0)[0]), 127);
    CHECK_EQ(static_cast<int>(out.at(0, 0)[1]), 128);
    CHECK_EQ(static_cast<int>(out.at(0, 0)[3]), 255);
}

// A repair over one rectangle must not disturb the rest of the canvas, so a pixel far from the
// stroke is byte-identical before and after.
void test_repairing_a_rectangle_leaves_the_rest_of_the_canvas_alone() {
    const Extent size{40, 30};
    Document doc;
    doc.reset(stack({{"Bottom", Image::filled(size, 10, 20, 30, 255)},
                     {"Top", Image::filled(size, 0, 0, 0, 0)}}),
              std::nullopt, "x.noise");
    paint_on(doc, Rect{2, 2, 4, 4}, 210);
    const std::span<const std::uint8_t> raw_before = doc.composite().pixels();
    const std::vector<std::uint8_t> before(raw_before.begin(), raw_before.end());
    paint_on(doc, Rect{30, 25, 3, 3}, 40);
    const std::span<const std::uint8_t> raw_after = doc.composite().pixels();
    const std::vector<std::uint8_t> after(raw_after.begin(), raw_after.end());

    std::size_t changed = 0;
    for (std::size_t i = 0; i < before.size(); ++i) {
        changed += before[i] != after[i] ? 1u : 0u;
    }
    // Only the second rectangle's bytes differ, and only in the three colour channels: the
    // bottom layer is opaque there, so alpha is the one channel the repair cannot change.
    CHECK_EQ(changed, 3u * 3u * 3u);
}

// The top of the stack is what the user paints on after opening a file, and the layer under a
// removed one takes the selection rather than nothing.
void test_the_stack_keeps_a_usable_selection() {
    Document doc;
    doc.reset(stack({{"A", Image::filled(Extent{4, 4}, 0, 0, 0, 0)},
                     {"B", Image::filled(Extent{4, 4}, 0, 0, 0, 0)},
                     {"C", Image::filled(Extent{4, 4}, 0, 0, 0, 0)}}),
              std::nullopt, "x.noise");
    CHECK_EQ(doc.active(), 2);
    CHECK_EQ(doc.layer(0).name, std::string("A"));
    CHECK(doc.select_layer(0));
    CHECK_EQ(doc.active(), 0);
    CHECK(!doc.select_layer(3));
    CHECK(!doc.select_layer(-1));
    CHECK(doc.remove_layer(0));
    CHECK_EQ(doc.layers().size(), 2u);
    // Removing the layer below the selection must not leave the selection pointing at nothing.
    CHECK(doc.select_layer(1));
    CHECK(doc.remove_layer(1));
    CHECK_EQ(doc.active(), 0);
    CHECK(!doc.remove_layer(0));
    CHECK_EQ(doc.layers().size(), 1u);
}

// Generated names belong to the position in the stack, not to a counter that remembers deleted
// layers.
void test_names_are_generated_from_the_stack_size() {
    Document doc;
    doc.reset(stack({{"Kept", Image::filled(Extent{4, 4}, 0, 0, 0, 0)}}), std::nullopt, "x.noise");
    doc.add_layer("", Rgba8{0, 0, 0, 0});
    CHECK_EQ(doc.layer(1).name, std::string("Layer 2"));
    doc.add_layer("", Rgba8{0, 0, 0, 0});
    CHECK_EQ(doc.layer(2).name, std::string("Layer 3"));
    doc.remove_layer(1);
    doc.add_layer("", Rgba8{0, 0, 0, 0});
    CHECK_EQ(doc.layer(1).name, std::string("Layer 3"));
    CHECK_EQ(doc.layer(static_cast<std::size_t>(doc.active())).name, std::string("Layer 3"));
}

// A document checksum that ignored the stack could not tell two different documents apart.
void test_the_document_checksum_follows_the_stack() {
    Document one;
    one.reset(stack({{"A", Image::filled(Extent{8, 8}, 1, 1, 1, 255)}}), std::nullopt, "x.noise");
    Document two;
    two.reset(stack({{"A", Image::filled(Extent{8, 8}, 1, 1, 1, 255)},
                     {"B", Image::filled(Extent{8, 8}, 0, 0, 0, 0)}}),
              std::nullopt, "x.noise");
    CHECK_NE(one.checksum(), two.checksum());
    // Same two layers, opposite order: the flattened result differs, and so must the checksum.
    Document three;
    three.reset(stack({{"B", Image::filled(Extent{8, 8}, 0, 0, 0, 0)},
                       {"A", Image::filled(Extent{8, 8}, 1, 1, 1, 255)}}),
                std::nullopt, "x.noise");
    CHECK_NE(two.checksum(), three.checksum());
}

// A layer mask is document content, so a stack that hides pixels has to differ from one that does
// not - and from a stack hidden the other way. A round trip that dropped `mask.m8`, or a mask that
// lost its coverage, has to be visible here rather than only in a panel.
void test_the_document_checksum_sees_a_layer_mask() {
    Document doc;
    doc.reset(stack({{"A", Image::filled(Extent{8, 8}, 1, 1, 1, 255)},
                     {"B", Image::filled(Extent{8, 8}, 2, 2, 2, 255)}}),
              std::nullopt, "x.noise");
    const std::string unmasked = doc.checksum();

    Mask shown(Extent{8, 8});
    shown.fill(kMaskIn);
    CHECK(doc.set_layer_mask(1, shown));
    const std::string with_mask = doc.checksum();
    CHECK_NE(with_mask, unmasked);

    Mask hidden(Extent{8, 8});
    hidden.fill(kMaskOut);
    CHECK(doc.set_layer_mask(1, hidden));
    const std::string inverted = doc.checksum();
    CHECK_NE(inverted, with_mask);

    // Same content, same hash: the checksum is a fact about the document, not about what happened
    // to it - otherwise the round trip below could not be compared against anything.
    CHECK(doc.set_layer_mask(1, shown));
    CHECK_EQ(doc.checksum(), with_mask);

    // The same coverage on a different layer is a different document.
    CHECK(doc.set_layer_mask(1, std::nullopt));
    CHECK(doc.set_layer_mask(0, shown));
    CHECK_NE(doc.checksum(), with_mask);

    CHECK(doc.set_layer_mask(0, std::nullopt));
    CHECK_EQ(doc.checksum(), unmasked);
}

// A correctness test cannot see the composite's two shortcuts, so compare work: repairing the same
// rectangle on a layer with a wholly shown mask against the same repair with no mask at all, on the
// same document. The copy the mask would need is pure waste, so the masked repair must cost about
// what the unmasked one does. Relative, so a slower machine is not a failure - measured here the two
// are within 1.2x of each other, and about 4x apart with the questions taken out of `composite()`.
void test_a_wholly_shown_mask_costs_the_composite_nothing(
    const std::function<double()>& now_ms) {
    constexpr Extent size{1024, 1024};
    const Rect repair{300, 300, 300, 300};
    Document doc;
    doc.reset(stack({{"base", Image::filled(size, 10, 20, 30, 255)},
                     {"top", Image::filled(size, 200, 40, 60, 255)}}),
              std::nullopt, "x.noise");

    double unmasked = 0.0;
    for (int round = 0; round < 20; ++round) {
        paint_on(doc, repair, static_cast<std::uint8_t>(round + 1));
        const double before = now_ms();
        doc.composite();
        unmasked = std::max(unmasked, now_ms() - before);
    }

    Mask shown(size);
    shown.fill(kMaskIn);
    CHECK(doc.set_layer_mask(1, shown));
    double masked = 0.0;
    for (int round = 0; round < 20; ++round) {
        paint_on(doc, repair, static_cast<std::uint8_t>(round + 200));
        const double before = now_ms();
        doc.composite();
        masked = std::max(masked, now_ms() - before);
    }
    if (!(masked < unmasked * 1.6)) {
        noise::test::report("a wholly shown mask costs about nothing", __FILE__, __LINE__,
                            "masked " + std::to_string(masked) + " ms, unmasked " +
                                std::to_string(unmasked) + " ms");
    }
}

// What a mask owes the picture, in the two cases the composite answers from the counts instead of a
// pass over the layer. Both are pixel claims: a wholly shown mask leaves every byte as the unmasked
// stack had it, and a wholly hidden one takes that layer out of the picture. A shortcut that lied
// would otherwise surface as a stroke half-hidden, somewhere far from here.
void test_a_wholly_shown_mask_hides_nothing_and_a_wholly_hidden_one_hides_everything() {
    const Extent size{12, 9};
    Document doc;
    doc.reset(stack({{"base", Image::filled(size, 10, 20, 30, 255)},
                     {"top", Image::filled(size, 200, 40, 60, 255)}}),
              std::nullopt, "x.noise");
    const std::string unmasked = doc.composite().checksum_hex();

    Mask shown(size);
    shown.fill(kMaskIn);
    CHECK(doc.set_layer_mask(1, shown));
    CHECK_EQ(doc.composite().checksum_hex(), unmasked);
    composite_matches_scratch(doc, "wholly shown mask");

    Mask hidden(size);
    hidden.fill(kMaskOut);
    CHECK(doc.set_layer_mask(1, hidden));
    CHECK_EQ(doc.composite().checksum_hex(), Image::filled(size, 10, 20, 30, 255).checksum_hex());
    composite_matches_scratch(doc, "wholly hidden mask");

    // One layer under a wholly shown mask is still the layer itself, which is the case the cheapest
    // answer covers: no scratch, no pass, the layer's own bytes.
    Document one;
    one.reset(stack({{"only", Image::filled(size, 7, 8, 9, 255)}}), std::nullopt, "x.noise");
    const std::string bare = one.composite().checksum_hex();
    CHECK(one.set_layer_mask(0, shown));
    CHECK_EQ(one.composite().checksum_hex(), bare);

    // And one row taken off it is one row gone from the picture, with the rest untouched.
    Mask holed(size);
    holed.fill(kMaskIn);
    holed.op(Rect{0, 0, size.w, 1}, MaskOp::subtract);
    CHECK(one.set_layer_mask(0, holed));
    const Image& punched = one.composite();
    for (int x = 0; x < size.w; ++x) {
        CHECK_EQ(static_cast<int>(punched.at(x, 0)[3]), 0);
        CHECK_EQ(static_cast<int>(punched.at(x, 1)[3]), 255);
    }
    composite_matches_scratch(one, "one row of a mask subtracted");
}

// The selection is a claim about *writable pixels*, so the two empty-looking states must differ: no
// selection lets everything through, a mask that covers nothing stops everything.
void test_no_selection_and_an_empty_selection_are_different_things() {
    Document doc;
    doc.reset(stack({{"A", Image::filled(Extent{6, 6}, 10, 20, 30, 255)}}), std::nullopt, "x.noise");
    const std::string untouched = doc.checksum();

    paint_on(doc, Rect{1, 1, 2, 2}, 200);
    CHECK_NE(doc.checksum(), untouched);
    CHECK(doc.dirty());

    Mask nothing(Extent{6, 6});
    CHECK(doc.set_selection(nothing));
    const std::string blocked_start = doc.checksum();
    const int steps_before = doc.history().undo_depth;
    paint_on(doc, Rect{0, 0, 6, 6}, 99);
    // Not one pixel, not one undo step, no new dirty mark: a write the selection stopped is not an
    // edit, and a gesture that lands outside the selection must not spend the undo budget.
    CHECK_EQ(doc.checksum(), blocked_start);
    CHECK_EQ(doc.history().undo_depth, steps_before);

    // The same write with the whole canvas selected is exactly the write.
    Mask everything(Extent{6, 6});
    everything.fill(255);
    CHECK(doc.set_selection(everything));
    paint_on(doc, Rect{0, 0, 6, 6}, 99);
    const std::string painted_all = doc.checksum();
    doc.undo();
    doc.undo();
    doc.set_selection(std::nullopt);
    paint_on(doc, Rect{0, 0, 6, 6}, 99);
    CHECK_EQ(doc.checksum(), painted_all);
}

// The load-bearing invariant: with a selection on, no byte outside it changes, whatever the writer
// asked for. Whole-image comparison, not a sampled point.
void test_a_selection_holds_every_write_inside_it() {
    const Extent size{12, 9};
    Document doc;
    doc.reset(stack({{"A", Image::filled(size, 0, 0, 0, 0)}}), std::nullopt, "x.noise");

    Mask selection(size);
    selection.op(Rect{3, 2, 5, 4}, MaskOp::replace);
    CHECK(doc.set_selection(selection));

    const Image before = doc.layer(0).pixels;
    // A write covering the whole canvas, in one call, through the plain door.
    paint_on(doc, Rect{0, 0, size.w, size.h}, 128);
    const Image after = doc.layer(0).pixels;
    for (int y = 0; y < size.h; ++y) {
        for (int x = 0; x < size.w; ++x) {
            const bool inside = selection.coverage(x, y) == 255;
            for (int channel = 0; channel < kChannels; ++channel) {
                const int expected = inside ? after.at(x, y)[channel] : before.at(x, y)[channel];
                CHECK_EQ(static_cast<int>(after.at(x, y)[channel]), expected);
            }
            if (inside) {
                CHECK_EQ(static_cast<int>(after.at(x, y)[3]), 255);
            }
        }
    }
    CHECK_NE(doc.checksum(), std::string{});
}

// The other half of the door: an edit that arrives with its own coverage - an AI composite's feathered
// mask, compiled from the selection and then widened - must not be re-clipped by that selection, or the
// hard edge returns and the report the caller printed describes an edit the document did not perform.
// It still has to be one undo step that gives every byte back.
void test_an_edit_carrying_its_own_coverage_reaches_past_the_selection() {
    const Extent size{12, 9};
    Document doc;
    doc.reset(stack({{"A", Image::filled(size, 10, 20, 30, 255)}}), std::nullopt, "x.noise");

    Mask selection(size);
    selection.op(Rect{4, 3, 4, 3}, MaskOp::replace);
    CHECK(doc.set_selection(selection));

    const Image before = doc.layer(0).pixels;
    const int steps_before = doc.history().undo_depth;
    const std::uint8_t patch[4] = {200, 200, 0, 255};
    doc.paint(Rect{0, 0, size.w, size.h}, PaintFence::own_coverage,
              [&](Image& image) { image.fill_rect(Rect{3, 2, 6, 5}, patch); });

    const Image after = doc.layer(0).pixels;
    std::size_t past = 0;
    for (int y = 0; y < size.h; ++y) {
        for (int x = 0; x < size.w; ++x) {
            if (selection.coverage(x, y) == 0 &&
                std::memcmp(after.at(x, y), before.at(x, y), kChannels) != 0) {
                ++past;
            }
        }
    }
    CHECK(past > 0);  // the margin landed, which is the whole claim of this door
    // And it is undone whole. A step that restored only what the selection would have allowed would
    // leave the margin behind as a permanent smear.
    CHECK_EQ(doc.history().undo_depth, steps_before + 1);
    CHECK(doc.undo());
    CHECK_EQ(doc.layer(0).pixels.checksum_hex(), before.checksum_hex());
}

// A shape whose edge covers part of a pixel is the case the coverage plane exists for: the write is
// mixed by how much, not switched on and off by whether there is any. Painting white onto black makes
// the expected result exact - the coverage byte itself - so this pins the whole path from a shape's
// raster to the pixel, selection door included.
void test_a_partly_covered_selection_paints_partly() {
    const Extent size{40, 20};
    Document doc;
    doc.reset(stack({{"A", Image::filled(size, 0, 0, 0, 0)}}), std::nullopt, "x.noise");

    Mask selection(size);
    op_shape(selection, MaskShape::ellipse, Rect{0, 0, size.w, size.h}, MaskOp::replace);
    CHECK(doc.set_selection(selection));
    CHECK_EQ(selection.coverage(20, 10), 255);
    CHECK_EQ(selection.coverage(0, 0), 0);

    const std::string before = doc.checksum();
    paint_on(doc, Rect{0, 0, size.w, size.h}, 255);
    const Image after = doc.layer(0).pixels;
    std::size_t partial = 0;
    for (int y = 0; y < size.h; ++y) {
        for (int x = 0; x < size.w; ++x) {
            const int coverage = selection.coverage(x, y);
            // Alpha takes the coverage; the colour is the paint's, unchanged. That is what mixing on
            // premultiplied values buys, and a test that expected the colour to fade would be
            // asserting the bug this rule exists to avoid.
            CHECK_EQ(static_cast<int>(after.at(x, y)[3]), coverage);
            if (coverage != 0) {
                CHECK_EQ(static_cast<int>(after.at(x, y)[0]), 255);
            }
            partial += coverage != 0 && coverage != 255 ? 1u : 0u;
        }
    }
    // If the raster had come back as a staircase there would be no partial pixel to mix, and this
    // test would be checking the wrong thing without noticing.
    CHECK(partial > 40u);

    // And undo puts all of it back, partial mixes included.
    doc.undo();
    CHECK_EQ(doc.checksum(), before);
}

// The gesture door has to honour the selection too - it is the one the brush and the move tool go
// through - and the step it commits has to put the pixels back exactly.
void test_a_gesture_clips_to_the_selection_and_undoes_exactly() {
    const Extent size{16, 5};
    Document doc;
    doc.reset(stack({{"A", Image::filled(size, 1, 2, 3, 255)}}), std::nullopt, "x.noise");

    Mask selection(size);
    selection.op(Rect{4, 0, 3, 5}, MaskOp::replace);
    CHECK(doc.set_selection(selection));

    const std::string start = doc.checksum();
    const int marked = doc.history().undo_depth;  // putting the mark down is a step of its own
    // Three events of one drag, each repainting its own rectangle, which is how a live stroke
    // reaches the document. The last rectangle overlaps the earlier ones, so a writer that
    // compounded its own work instead of starting from the held-aside pixels would show up.
    for (const Rect event : {Rect{0, 0, 6, 5}, Rect{3, 0, 6, 5}, Rect{8, 0, 8, 5}}) {  // third is past the selection
        doc.begin_provisional(event, LayerPlane::pixels);
        doc.repaint_provisional(event, [event](Image& image, Rect area) {
            const std::uint8_t stroke_rgba[4] = {240, 10, 10, 255};
            image.fill_rect(intersect(area, event), stroke_rgba);
        });
    }
    const Image during = doc.layer(0).pixels;
    const std::string gestured = doc.checksum();
    doc.commit_provisional();
    CHECK_EQ(doc.checksum(), gestured);
    CHECK_EQ(doc.history().undo_depth, marked + 1);  // the gesture, and nothing per event

    for (int y = 0; y < size.h; ++y) {
        for (int x = 0; x < size.w; ++x) {
            const bool inside = selection.coverage(x, y) == 255;
            const bool reached = x >= 4 && x < 11;
            for (int channel = 0; channel < kChannels; ++channel) {
                // Outside the selection the layer still holds what it started with, including the part
            // the third event reached: the selection is what limits a write, not the gesture's own
            // rectangle.
            const int found[kChannels] = {1, 2, 3, 255};
            const int expected = inside && reached ? during.at(x, y)[channel] : found[channel];
                CHECK_EQ(static_cast<int>(doc.layer(0).pixels.at(x, y)[channel]), expected);
            }
        }
    }

    CHECK(doc.undo());
    CHECK_EQ(doc.checksum(), start);

    // A whole gesture outside the selection costs nothing at all: no step, no dirty bump, because
    // what it would put back is what is already there.
    Mask corner(size);
    corner.op(Rect{12, 0, 4, 5}, MaskOp::replace);
    CHECK(doc.set_selection(corner));
    const int steps = doc.history().undo_depth;
    doc.begin_provisional(Rect{0, 0, 6, 5}, LayerPlane::pixels);
    doc.repaint_provisional(Rect{0, 0, 6, 5}, [](Image& image, Rect area) {
        const std::uint8_t stroke_rgba[4] = {0, 255, 0, 255};
        image.fill_rect(area, stroke_rgba);
    });
    CHECK_EQ(doc.layer(0).pixels.checksum_hex(), Image::filled(size, 1, 2, 3, 255).checksum_hex());
    doc.commit_provisional();
    CHECK_EQ(doc.history().undo_depth, steps);

    // Undo puts pixels back; it does not move the selection, which was never part of the step.
    CHECK(doc.selection().has_value());
    CHECK(doc.selection()->size() == size);
}

// A mask of another size is a claim about pixels this document does not have.
// A mask is a writable plane, so a gesture over it has to earn the same guarantees as a gesture over
// colour: the composite follows it frame by frame, the whole drag is one undo step, and undo puts
// every coverage byte back - not approximately, and not the pixels with it.
void test_painting_a_mask_is_one_undo_step_that_hides_and_shows() {
    const Extent size{16, 8};
    Document doc;
    doc.reset(stack({{"A", Image::filled(size, 4, 5, 6, 255)},
                     {"B", Image::filled(size, 200, 30, 30, 255)}}),
              std::nullopt, "x.noise");
    const std::string unmasked = doc.checksum();
    Mask shown(size);
    shown.fill(kMaskIn);
    CHECK(doc.set_layer_mask(1, shown));
    doc.mark_saved();
    const std::string start = doc.checksum();
    const std::string pixels = doc.layer(1).pixels.checksum_hex();
    CHECK_EQ(doc.history().undo_depth, 1);  // putting the plane down is itself a step

    // Three events of one drag, each repainting its own rectangle: the last overlaps the second, so a
    // gesture that compounded its own work instead of starting from the held-aside coverage would
    // come back darker rather than identical.
    // None of these starts at the top row, and the third leaves the rectangle the previous two grew,
    // so the held-aside coverage is copied at a non-zero origin in both directions - the arithmetic
    // that put a stroke outside the plane the first time it was written.
    for (const Rect event : {Rect{0, 2, 6, 4}, Rect{3, 3, 6, 4}, Rect{8, 1, 4, 6}}) {
        CHECK(doc.begin_provisional(event, LayerPlane::mask));
        doc.repaint_provisional_mask(event, [event](Mask& mask, Rect area) {
            mask.op(intersect(area, event), MaskOp::subtract);
        });
        composite_matches_scratch(doc, "mid-gesture, after a mask event");
    }
    // Hidden where the drag reached, shown everywhere else. The reach is the union of the three
    // rectangles: x 0..11 for rows 2..6, and x 8..11 for rows 1 and 7 as well.
    std::size_t shown_cells = 0;
    for (int x = 0; x < size.w; ++x) {
        for (int y = 0; y < size.h; ++y) {
            const bool reached = (x < 6 && y >= 2 && y < 6) || (x >= 3 && x < 9 && y >= 3 && y < 7) ||
                                 (x >= 8 && x < 12 && y >= 1 && y < 7);
            const std::uint8_t want = reached ? kMaskOut : kMaskIn;
            shown_cells += reached ? 0u : 1u;
            CHECK_EQ(static_cast<int>(doc.layer(1).mask->coverage(x, y)),
                     static_cast<int>(want));
        }
    }
    // The canvas shows it, through the repaired composite rather than a rebuilt one: every event of
    // the gesture is compared against a composite built from scratch, because a mask write reaches the
    // compositor by a route (masked layers, the blends-to-itself shortcut) that a colour write does
    // not, and a repair that forgot the vacated half of a move would look fine here and nowhere else.
    composite_matches_scratch(doc, "after a mask gesture");
    CHECK_EQ(doc.composite().at(0, 0)[0], 200);   // row 0 was never reached, so the layer still shows
    CHECK_EQ(doc.composite().at(15, 4)[0], 200);
    CHECK_EQ(doc.composite().at(9, 4)[0], 4);     // reached, so the layer below shows through
    CHECK_EQ(doc.layer(1).mask->covered(), shown_cells);
    // The tight rectangle is the whole plane, because the drag missed enough of every edge for
    // coverage to survive to all four sides - which is the check that the missed rows stayed missed.
    CHECK(doc.layer(1).mask->bounds() == Rect{0, 0, size.w, size.h});

    CHECK(doc.in_provisional());
    doc.commit_provisional();
    CHECK(!doc.in_provisional());
    CHECK_EQ(doc.history().undo_depth, 2);  // the stroke, and the plane it was written into
    CHECK(doc.dirty());
    const std::string painted = doc.checksum();
    CHECK_NE(painted, start);
    CHECK_EQ(doc.layer(1).pixels.checksum_hex(), pixels);  // the write was coverage only

    CHECK(doc.undo());
    CHECK_EQ(doc.checksum(), start);
    CHECK_EQ(doc.layer(1).mask->covered(), static_cast<std::size_t>(size.w * size.h));
    // The stroke was one step and the plane put down is the other; undoing that takes the mask off
    // altogether, and what the canvas shows is the document as it stood before the mask existed.
    CHECK(doc.undo());
    CHECK(!doc.layer(1).mask.has_value());
    CHECK_EQ(doc.checksum(), unmasked);
    CHECK(!doc.can_undo());
    CHECK(doc.redo());
    CHECK(doc.redo());
    CHECK_EQ(doc.checksum(), painted);
}

// The selection fences every write, and a mask write is one: what a stroke did to coverage happens
// only as far as the selection reaches, and a partly covered pixel is partly changed.
void test_the_selection_fences_a_mask_stroke_too() {
    const Extent size{8, 4};
    Document doc;
    doc.reset(stack({{"A", Image::filled(size, 10, 10, 10, 255)}}), std::nullopt, "x.noise");
    Mask shown(size);
    shown.fill(kMaskIn);
    CHECK(doc.set_layer_mask(0, shown));

    std::vector<std::uint8_t> coverage;
    for (int y = 0; y < size.h; ++y) {
        for (int x = 0; x < size.w; ++x) {
            coverage.push_back(x < 2 ? kMaskOut
                                     : (x < 4 ? kMaskIn
                                              : (x == 4 ? static_cast<std::uint8_t>(128) : kMaskOut)));
        }
    }
    const Mask selection = Mask::from_plane(size, coverage).value();
    CHECK(doc.set_selection(selection));
    const int marked = doc.history().undo_depth;  // the mark is a step; what follows is the write

    // Hide the whole canvas: only the selected columns move, and the half-selected one moves
    // half way - 255 of coverage travelling to 0 by 128/255 is 127, which is the byte the door's own
    // rounding gives and is why the ends are pinned exactly rather than the middle.
    CHECK(doc.paint_mask(Rect{0, 0, size.w, size.h}, [](Mask& mask) { mask.fill(kMaskOut); }));
    for (int x = 0; x < size.w; ++x) {
        for (int y = 0; y < size.h; ++y) {
            const std::uint8_t want = x < 2 ? kMaskIn
                                            : (x < 4 ? kMaskOut
                                                     : (x == 4 ? static_cast<std::uint8_t>(127)
                                                               : kMaskIn));
            CHECK_EQ(static_cast<int>(doc.layer(0).mask->coverage(x, y)), static_cast<int>(want));
        }
    }
    CHECK_EQ(doc.history().undo_depth, marked + 1);
    CHECK(doc.undo());
    CHECK_EQ(doc.layer(0).mask->covered(), static_cast<std::size_t>(size.w * size.h));
}

// The pointer path fences a coverage write as well as the one-call door does, and what a pixel outside
// the selection returns to is the gesture's own held-aside byte rather than the write. `commit` then has
// to leave the fenced state standing and record the fenced rectangle, so undo returns the same bytes the
// gesture started from - not the unfenced ones the brush would have written.
void test_a_mask_gesture_is_fenced_by_the_selection() {
    const Extent size{10, 4};
    Document doc;
    doc.reset(stack({{"A", Image::filled(size, 5, 5, 5, 255)}}), std::nullopt, "x.noise");
    Mask shown(size);
    shown.fill(kMaskIn);
    CHECK(doc.set_layer_mask(0, shown));
    doc.mark_saved();

    Mask selection(size);
    selection.op(Rect{3, 0, 4, size.h}, MaskOp::replace);
    CHECK(doc.set_selection(selection));

    // Hide the whole plane through the gesture door.
    CHECK(doc.begin_provisional(Rect{0, 0, size.w, size.h}, LayerPlane::mask));
    doc.repaint_provisional_mask(Rect{0, 0, size.w, size.h}, [](Mask& mask, Rect) {
        mask.fill(kMaskOut);
    });
    const auto expect_fenced = [&doc, &size](const char* where) {
        for (int x = 0; x < size.w; ++x) {
            for (int y = 0; y < size.h; ++y) {
                const std::uint8_t want = x >= 3 && x < 7 ? kMaskOut : kMaskIn;
                if (doc.layer(0).mask->coverage(x, y) != want) {
                    noise::test::report("mask gesture fenced by the selection", __FILE__, __LINE__,
                                        std::string(where) + " at x=" + std::to_string(x) +
                                            " y=" + std::to_string(y) + ": got " +
                                            std::to_string(doc.layer(0).mask->coverage(x, y)) +
                                            " want " + std::to_string(want));
                }
            }
        }
    };
    expect_fenced("during the gesture");
    doc.commit_provisional();
    expect_fenced("after the commit");
    // The mark the fence was drawn with, the plane put down to be fenced, and the one step the gesture
    // committed.
    CHECK_EQ(doc.history().undo_depth, 3);

    CHECK(doc.undo());
    CHECK_EQ(doc.layer(0).mask->covered(), static_cast<std::size_t>(size.w * size.h));
    CHECK(doc.redo());
    expect_fenced("after the redo");
}

// A stroke that changed no coverage is not an edit: no step, no dirty mark, no revision, exactly as
// for pixels - otherwise dragging over an already-hidden part of a mask would spend the undo budget.
void test_a_mask_stroke_that_changed_nothing_costs_nothing() {
    Document doc;
    doc.reset(stack({{"A", Image::filled(Extent{6, 6}, 1, 1, 1, 255)}}), std::nullopt, "x.noise");
    Mask hidden(Extent{6, 6});
    hidden.fill(kMaskOut);
    CHECK(doc.set_layer_mask(0, hidden));
    doc.mark_saved();
    const std::uint64_t revision = doc.revision();
    const int put_down = doc.history().undo_depth;  // the plane arriving is a step; nothing else here is

    CHECK(doc.paint_mask(Rect{0, 0, 6, 6}, [](Mask& mask) { mask.fill(kMaskOut); }));
    CHECK_EQ(doc.history().undo_depth, put_down);
    CHECK(!doc.dirty());
    CHECK_EQ(doc.revision(), revision);

    // The same through the gesture door: a press and release that reached nothing.
    CHECK(doc.begin_provisional(Rect{1, 1, 2, 2}, LayerPlane::mask));
    doc.repaint_provisional_mask(Rect{1, 1, 2, 2}, [](Mask& mask, Rect area) {
        mask.op(area, MaskOp::subtract);
    });
    doc.commit_provisional();
    // A live repaint moves the revision whatever it wrote - that is how the canvas keeps up with a
    // drag - so what a gesture that changed nothing must not leave behind is a step or the unsaved
    // mark.
    CHECK_EQ(doc.history().undo_depth, put_down);
    CHECK(!doc.dirty());
}

// A layer with no mask has no coverage to write: the refusal is the answer, and no mask appears under
// the brush.
void test_painting_a_layer_with_no_mask_is_refused() {
    Document doc;
    doc.reset(stack({{"A", Image::filled(Extent{4, 4}, 9, 9, 9, 255)}}), std::nullopt, "x.noise");
    CHECK(!doc.paint_mask(Rect{0, 0, 4, 4}, [](Mask& mask) { mask.fill(kMaskOut); }));
    CHECK(!doc.layer(0).mask.has_value());
    CHECK(!doc.begin_provisional(Rect{0, 0, 4, 4}, LayerPlane::mask));
    CHECK(!doc.in_provisional());
}

// One gesture writes one plane. A stroke that started on colour cannot start writing coverage while
// the button is down - the held-aside bytes are colour's, and committing them over coverage would
// restore something the gesture never touched.
void test_a_gesture_holds_one_plane() {
    const Extent size{8, 2};
    Document doc;
    doc.reset(stack({{"A", Image::filled(size, 3, 3, 3, 255)}}), std::nullopt, "x.noise");
    Mask shown(size);
    shown.fill(kMaskIn);
    CHECK(doc.set_layer_mask(0, shown));
    const int put_down = doc.history().undo_depth;  // the plane arriving is a step; nothing else here is

    CHECK(doc.begin_provisional(Rect{0, 0, 4, 2}, LayerPlane::pixels));
    CHECK(!doc.begin_provisional(Rect{0, 0, 8, 2}, LayerPlane::mask));
    doc.repaint_provisional_mask(Rect{0, 0, 8, 2}, [](Mask& mask, Rect area) {
        mask.op(area, MaskOp::subtract);
    });
    CHECK_EQ(doc.layer(0).mask->covered(), static_cast<std::size_t>(size.w * size.h));
    doc.commit_provisional();
    CHECK_EQ(doc.history().undo_depth, put_down);  // and it painted nothing either

    // The other way round still works: a coverage gesture writes coverage.
    CHECK(doc.begin_provisional(Rect{0, 0, 4, 2}, LayerPlane::mask));
    doc.repaint_provisional_mask(Rect{0, 0, 4, 2}, [](Mask& mask, Rect area) {
        mask.op(area, MaskOp::subtract);
    });
    CHECK_EQ(doc.layer(0).mask->covered(), 8u);
    doc.commit_provisional();
    CHECK_EQ(doc.history().undo_depth, put_down + 1);
}

// Taking the mask off ends a gesture that holds it - its ink is restored, not committed - and the step
// it records is the plane, so the coverage written into the mask stays reachable underneath and the
// colour steps of the same layer survive.
void test_taking_the_mask_off_ends_the_gesture_and_is_itself_undoable() {
    const Extent size{6, 6};
    Document doc;
    doc.reset(stack({{"A", Image::filled(size, 7, 7, 7, 255)}}), std::nullopt, "x.noise");
    Mask shown(size);
    shown.fill(kMaskIn);
    CHECK(doc.set_layer_mask(0, shown));
    paint_on(doc, Rect{0, 0, 2, 2}, 190);            // a colour step to keep
    const std::string painted = doc.layer(0).pixels.checksum_hex();

    CHECK(doc.begin_provisional(Rect{1, 1, 3, 3}, LayerPlane::mask));
    doc.repaint_provisional_mask(Rect{1, 1, 3, 3}, [](Mask& mask, Rect area) {
        mask.op(area, MaskOp::subtract);
    });
    CHECK_EQ(doc.layer(0).mask->covered(), static_cast<std::size_t>(size.w * size.h) - 9u);

    CHECK(doc.set_layer_mask(0, std::nullopt));
    CHECK(!doc.in_provisional());
    CHECK(!doc.layer(0).mask.has_value());
    CHECK_EQ(doc.layer(0).pixels.checksum_hex(), painted);
    CHECK_EQ(doc.history().undo_depth, 3);  // the plane down, the colour, the plane off

    // Undo walks the removal back before anything else, and what comes back is the plane as the
    // gesture left it - the aborted stroke's ink was restored when the mask went, so the coverage the
    // press found is what returns.
    CHECK(doc.undo());
    CHECK(doc.layer(0).mask.has_value());
    CHECK_EQ(doc.layer(0).mask->covered(), static_cast<std::size_t>(size.w * size.h));
    // Then the colour step, which moves the pixels and leaves the plane alone, then the plane's own
    // arrival: nothing was thrown away for taking the mask off.
    CHECK(doc.undo());
    CHECK_NE(doc.layer(0).pixels.checksum_hex(), painted);
    CHECK(doc.layer(0).mask.has_value());
    CHECK(doc.undo());
    CHECK(!doc.layer(0).mask.has_value());
    CHECK(!doc.can_undo());
}

void test_a_selection_of_another_size_is_refused() {
    Document doc;
    doc.reset(stack({{"A", Image::filled(Extent{4, 4}, 0, 0, 0, 0)}}), std::nullopt, "x.noise");
    Mask too_big(Extent{5, 4});
    CHECK(!doc.set_selection(too_big));
    CHECK(!doc.selection().has_value());
    Mask too_small(Extent{4, 3});
    CHECK(!doc.set_selection(too_small));
    CHECK(!doc.selection().has_value());

    // Opening a document is not continuing the one that was open: the selection belongs to the
    // document, so a fresh stack starts with none.
    Mask inside(Extent{4, 4});
    inside.op(Rect{0, 0, 2, 2}, MaskOp::replace);
    CHECK(doc.set_selection(inside));
    doc.reset(stack({{"B", Image::filled(Extent{4, 4}, 0, 0, 0, 0)}}), std::nullopt, "y.noise");
    CHECK(!doc.selection().has_value());
}

// What the selection costs, and what it must not cost: setting it marks the document dirty because it
// is written to the file, and saying the same thing twice must not.
void test_a_selection_change_is_a_document_change() {
    Document doc;
    doc.reset(stack({{"A", Image::filled(Extent{8, 8}, 0, 0, 0, 0)}}), std::nullopt, "x.noise");
    Mask selection(Extent{8, 8});
    selection.op(Rect{1, 1, 2, 2}, MaskOp::replace);
    CHECK(doc.set_selection(selection));
    CHECK(doc.dirty());
    CHECK_EQ(doc.history().undo_depth, 1);  // the mark is a step

    const std::uint64_t revision_after = doc.revision();
    doc.mark_saved();
    CHECK(doc.set_selection(selection));
    CHECK(!doc.dirty());
    CHECK_EQ(doc.revision(), revision_after);
    // Writing the coverage that is already there changed nothing, so it is not an edit: no step, no
    // depth spent restoring bytes nobody moved.
    CHECK_EQ(doc.history().undo_depth, 1);

    Mask wider(Extent{8, 8});
    wider.op(Rect{1, 1, 4, 4}, MaskOp::replace);
    CHECK(doc.set_selection(wider));
    CHECK(doc.dirty());

    // Clearing is a change too, and the state that results is "no selection", not a mask of nothing.
    CHECK(doc.set_selection(std::nullopt));
    CHECK(doc.dirty());
    CHECK(!doc.selection().has_value());
    CHECK_EQ(doc.history().undo_depth, 3);  // the mark, the wider mark, the clearing
}

// A layer mask says what shows, not what exists: it changes the composite and leaves the layer's
// pixels, and therefore the document checksum, exactly as they were.
void test_a_layer_mask_hides_without_editing_the_layer() {
    Document doc;
    doc.reset(stack({{"Ink", Image::filled(Extent{8, 6}, 200, 40, 20, 255)}}), std::nullopt,
              "x.noise");
    const std::string pixels_before = doc.layer(0).pixels.checksum_hex();
    const std::string whole = doc.composite().checksum_hex();

    Mask mask(Extent{8, 6});
    mask.op(Rect{2, 1, 3, 3}, MaskOp::replace);
    CHECK(doc.set_layer_mask(0, mask));
    CHECK(doc.dirty());
    // Hiding moves no pixel of the layer's own picture: the step it costs holds the plane, and the
    // bytes underneath are what the undo of it has to leave alone.
    CHECK(doc.can_undo());
    CHECK_EQ(doc.layer(0).pixels.checksum_hex(), pixels_before);
    CHECK(doc.composite().checksum_hex() != whole);

    for (int y = 0; y < 6; ++y) {
        for (int x = 0; x < 8; ++x) {
            const std::uint8_t* shown = doc.composite().at(x, y);
            const bool inside = x >= 2 && x < 5 && y >= 1 && y < 4;
            CHECK_EQ(shown[3], inside ? 255 : 0);
        }
    }

    // A lone masked layer is *not* the layer: the shortcut that hands back the layer by reference
    // has no coverage in it, so it would show pixels the mask hides.
    CHECK(doc.composite().checksum_hex() != pixels_before);

    // A mask that covers nothing hides the layer entirely, which is a different claim from removing
    // it - and both are one `set_layer_mask` apart.
    Mask hides_everything(Extent{8, 6});
    CHECK(doc.set_layer_mask(0, hides_everything));
    for (int y = 0; y < 6; ++y) {
        for (int x = 0; x < 8; ++x) {
            CHECK_EQ(doc.composite().at(x, y)[3], 0);
        }
    }
    CHECK(doc.set_layer_mask(0, std::nullopt));
    CHECK_EQ(doc.composite().checksum_hex(), whole);
    CHECK(doc.layer(0).mask == std::nullopt);

    // Three plane changes, and undo walks all three back to the document that had no mask at all - each
    // one returning the coverage that was there, not merely "a mask".
    CHECK(doc.undo());
    CHECK_EQ(doc.composite().at(0, 0)[3], 0);       // the mask that hid everything, back
    CHECK(doc.undo());
    CHECK_EQ(doc.composite().at(2, 1)[3], 255);     // replaced by the one that hid three cells
    CHECK(doc.undo());
    CHECK(!doc.layer(0).mask.has_value());
    CHECK_EQ(doc.composite().checksum_hex(), whole);
    CHECK_EQ(doc.layer(0).pixels.checksum_hex(), pixels_before);
}

void test_a_layer_mask_of_another_size_is_refused() {
    Document doc;
    doc.reset(stack({{"A", Image::filled(Extent{4, 4}, 0, 0, 0, 0)}}), std::nullopt, "x.noise");
    CHECK(!doc.set_layer_mask(0, Mask(Extent{5, 4})));
    CHECK(!doc.set_layer_mask(0, Mask(Extent{4, 3})));
    CHECK(!doc.layer(0).mask.has_value());
    CHECK(!doc.set_layer_mask(9, Mask(Extent{4, 4})));
}

// Masking is composite state, like visibility: it marks the document dirty because it is saved and it
// bumps the revision because the canvas changes. Saying the same thing twice must cost nothing; saying
// it once costs the plane's own step, and nothing else's.
void test_a_layer_mask_change_is_a_document_change() {
    Document doc;
    doc.reset(stack({{"A", Image::filled(Extent{8, 8}, 0, 0, 0, 0)}}), std::nullopt, "x.noise");
    // A stroke first, so there is a step in the timeline for a mask change to destroy: the claim
    // being tested is not "a mask pushes a step" but "a mask leaves the steps that exist alone".
    paint_on(doc, Rect{6, 6, 2, 2}, 210);
    const std::string painted = doc.layer(0).pixels.checksum_hex();
    CHECK(doc.can_undo());

    Mask mask(Extent{8, 8});
    mask.op(Rect{1, 1, 2, 2}, MaskOp::replace);
    CHECK(doc.set_layer_mask(0, mask));
    CHECK(doc.dirty());
    const std::uint64_t revision = doc.revision();
    CHECK_EQ(doc.history().undo_depth, 2);  // the stroke, and the plane put down

    doc.mark_saved();
    CHECK(doc.set_layer_mask(0, mask));
    CHECK(!doc.dirty());
    CHECK_EQ(doc.revision(), revision);
    // Coverage that is already there is not an edit, so it costs no step and no dirty mark - the same
    // rule a pixel write follows.
    CHECK_EQ(doc.history().undo_depth, 2);

    const std::string before_untouched = doc.layer(0).pixels.checksum_hex();
    // Undo takes the plane off first, and the layer's pixels sit in neither direction of that step.
    CHECK(doc.undo());
    CHECK(!doc.layer(0).mask.has_value());
    CHECK_EQ(doc.layer(0).pixels.checksum_hex(), before_untouched);
    CHECK(doc.undo());
    CHECK(doc.layer(0).pixels.checksum_hex() != painted);
    CHECK(!doc.can_undo());

    // A plane replaced returns the coverage it replaced, which is the claim a per-rectangle entry
    // cannot make about a mask that was not there before.
    CHECK(doc.redo());
    CHECK(doc.redo());
    CHECK_EQ(doc.layer(0).mask->covered(), mask.covered());
    Mask wider(Extent{8, 8});
    wider.op(Rect{1, 1, 5, 5}, MaskOp::replace);
    CHECK(doc.set_layer_mask(0, wider));
    CHECK(doc.dirty());
    CHECK(doc.revision() > revision);
    CHECK_EQ(doc.layer(0).mask->covered(), wider.covered());
    CHECK(doc.undo());
    CHECK_EQ(doc.layer(0).mask->covered(), mask.covered());
}

// A viewer of one layer - a thumbnail - caches against `content_revision`, so that counter has to move
// when the layer's own content moves and not when the composite merely looks different. Both directions
// are load-bearing: a counter that misses a write leaves a stale thumbnail on screen claiming pixels
// that are not there, and one that moves for a visibility click re-samples every layer on every frame of
// an opacity drag.
void test_what_a_layer_holds_and_what_the_composite_uses_are_two_counters() {
    Document doc;
    doc.reset(stack({{"A", Image::filled(Extent{8, 8}, 0, 0, 0, 0)},
                     {"B", Image::filled(Extent{8, 8}, 0, 0, 0, 0)}}),
              std::nullopt, "x.noise");
    const auto revs = [&doc] {
        std::vector<std::uint64_t> out;
        for (const Layer& layer : doc.layers()) {
            out.push_back(layer.content_revision);
        }
        return out;
    };
    const std::vector<std::uint64_t> start = revs();
    const std::size_t active = static_cast<std::size_t>(doc.active());
    const std::size_t other = 1 - active;

    // The three facts the composite reads and no layer holds: the canvas changes, and no card does.
    CHECK(doc.set_layer_visible(0, false));
    CHECK(doc.set_layer_opacity(0, 0.5));
    CHECK(doc.set_layer_mode(0, BlendMode::multiply));
    CHECK(doc.revision() > 0);
    CHECK(revs() == start);

    // A write touches the layer it was made on.
    paint_on(doc, Rect{2, 2, 2, 2}, 200);
    CHECK_EQ(revs()[active], start[active] + 1);
    CHECK_EQ(revs()[other], start[other]);

    // Coverage is content a card draws, so a mask counts as a write to its own layer - and only that one.
    Mask mask(Extent{8, 8});
    mask.op(Rect{0, 0, 2, 2}, MaskOp::replace);
    const std::vector<std::uint64_t> before_mask = revs();
    CHECK(doc.set_layer_mask(1, mask));
    CHECK_EQ(revs()[1], before_mask[1] + 1);
    CHECK_EQ(revs()[0], before_mask[0]);

    // Undo answers a step that could have moved the stack by invalidating every card: the truth rather
    // than a guess about which layers it touched.
    CHECK(doc.undo());
    CHECK(revs()[active] > before_mask[active]);
    CHECK(revs()[other] > before_mask[other]);

    // A layer arriving moves no other layer's content, and the compositing order is not content either:
    // a card is a picture of one layer, keyed to that layer, so neither change makes an existing picture
    // wrong. The new layer has no card yet, which is the only thing its own counter would be for.
    const std::vector<std::uint64_t> before_add = revs();
    const std::uint64_t shown = doc.revision();
    CHECK(doc.add_layer("C", Rgba8{0, 0, 0, 0}) >= 0);
    CHECK_EQ(revs().size(), 3u);
    CHECK(revs()[0] == before_add[0] && revs()[1] == before_add[1]);
    CHECK(doc.revision() > shown);
    // Nor is the order of the stack: the composite is made differently, and every layer is other than
    // nothing - each still holds what its own counter says it holds.
    const std::map<std::uint64_t, std::uint64_t> held = content_revisions(doc);
    const std::uint64_t stacked = doc.revision();
    CHECK(doc.move_layer(0, 2));
    CHECK_EQ(content_revisions(doc), held);
    CHECK(doc.revision() > stacked);
}

// A reorder is one edit, and the order it held is what comes back: the stack is put back rather than
// painted back, since no pixel moved.
void test_a_layer_moves_as_one_step_that_undoes_itself() {
    Document doc;
    doc.reset(stack({{"bottom", Image::filled(Extent{4, 4}, 40, 40, 40, 255)},
                     {"middle", Image::filled(Extent{4, 4}, 80, 80, 80, 255)},
                     {"top", Image::filled(Extent{4, 4}, 200, 200, 200, 255)}}),
              std::nullopt, "x.noise");
    const auto names = [&doc] { return layer_names(doc); };
    const std::string whole = doc.checksum();
    const std::string shown = doc.composite().checksum_hex();
    const auto held = content_revisions(doc);

    CHECK(doc.move_layer(0, 2));
    // No layer's own content moved, so every card is still a true picture of the layer it belongs to -
    // which is what lets a reorder cost no resample of any of them.
    CHECK_EQ(content_revisions(doc), held);
    CHECK_EQ(names(), std::string("middle,top,bottom"));
    CHECK_EQ(doc.active(), 2);
    CHECK(doc.dirty());
    CHECK(doc.composite().checksum_hex() != shown);
    CHECK_EQ(doc.history().undo_depth, 1);

    CHECK(doc.undo());
    CHECK_EQ(names(), std::string("bottom,middle,top"));
    CHECK_EQ(doc.checksum(), whole);
    CHECK_EQ(doc.composite().checksum_hex(), shown);

    CHECK(doc.redo());
    CHECK_EQ(names(), std::string("middle,top,bottom"));
    CHECK(doc.undo());

    // Where a layer already is, nothing moves: no step, no revision, nothing dirty.
    Document still;
    still.reset(stack({{"only", Image::filled(Extent{2, 2}, 0, 0, 0, 0)}}), std::nullopt,
              "y.noise");
    const std::uint64_t revision = still.revision();
    CHECK(still.move_layer(0, 0));
    CHECK_EQ(still.revision(), revision);
    CHECK(!still.dirty());
    CHECK(!still.move_layer(0, 1));
    CHECK(!still.move_layer(-1, 0));
}

// The invariant a merge lives or dies by: what the canvas showed is what it shows afterwards, exactly -
// the upper layer's mask, opacity and blend mode land on the way down. And undo has to give the layer
// back as a layer, not as an empty one with the right name.
void test_merging_down_shows_what_the_canvas_already_showed() {
    Document doc;
    doc.reset(stack({{"sky", Image::filled(Extent{4, 4}, 30, 60, 90, 255)},
                     {"bird", Image::filled(Extent{4, 4}, 250, 250, 250, 255)}}),
              std::nullopt, "x.noise");
    Mask mask(Extent{4, 4});
    mask.op(Rect{0, 0, 4, 2}, MaskOp::replace);
    CHECK(doc.set_layer_mask(1, mask));
    CHECK(doc.set_layer_opacity(1, 0.5));
    CHECK(doc.set_layer_mode(1, BlendMode::multiply));

    const std::string shown = doc.composite().checksum_hex();
    const std::string bird_pixels = doc.layer(1).pixels.checksum_hex();
    const std::string whole = doc.checksum();

    CHECK(!doc.merge_down(0));
    CHECK(doc.merge_down(1));
    CHECK_EQ(doc.layers().size(), 1u);
    CHECK_EQ(doc.composite().checksum_hex(), shown);
    CHECK_EQ(doc.history().undo_depth, 2);  // the mask put down, and the merge

    CHECK(doc.undo());
    CHECK_EQ(doc.layers().size(), 2u);
    CHECK_EQ(doc.layer(1).name, std::string("bird"));
    CHECK(doc.layer(1).mask.has_value());
    CHECK_EQ(doc.layer(1).pixels.checksum_hex(), bird_pixels);
    CHECK_EQ(doc.composite().checksum_hex(), shown);
    CHECK_EQ(doc.checksum(), whole);

    // One step below the merge sits the plane that layer was wearing, so the next undo takes the mask
    // off the layer the merge had just handed back - the pair a step re-inserting a layer and a step
    // naming its mask have to get right, in both directions.
    CHECK(doc.undo());
    CHECK(!doc.layer(1).mask.has_value());
    CHECK_EQ(doc.layer(1).pixels.checksum_hex(), bird_pixels);
    CHECK(!doc.can_undo());

    CHECK(doc.redo());
    CHECK(doc.layer(1).mask.has_value());

    CHECK(doc.redo());
    CHECK_EQ(doc.layers().size(), 1u);
    CHECK_EQ(doc.composite().checksum_hex(), shown);

    // A layer nobody can see contributes nothing, so merging it takes the layer away without moving a
    // pixel - and undo gives it back exactly as it was, hidden included: the merge is what changed, not
    // the visibility that was already there.
    Document hidden;
    hidden.reset(stack({{"base", Image::filled(Extent{2, 2}, 10, 10, 10, 255)},
                        {"ghost", Image::filled(Extent{2, 2}, 200, 0, 0, 255)}}),
                 std::nullopt, "z.noise");
    CHECK(hidden.set_layer_visible(1, false));
    const std::string base_pixels = hidden.layer(0).pixels.checksum_hex();
    CHECK(hidden.merge_down(1));
    CHECK_EQ(hidden.layer(0).pixels.checksum_hex(), base_pixels);
    const std::string ghost_pixels = hidden.layer(0).pixels.checksum_hex();
    CHECK(hidden.undo());
    CHECK_EQ(hidden.layers().size(), 2u);
    CHECK(!hidden.layer(1).visible);
    CHECK(hidden.layer(1).pixels.checksum_hex() != ghost_pixels);
}

// The beat this exists for: "I am lost in layers" - Flatten, and the picture is one layer. It cannot have
// changed, because the layer holds exactly what the canvas was showing; and undo has to hand back every
// layer as it was, since nothing was written to any of them.
void test_flattening_leaves_one_layer_that_shows_what_the_stack_showed() {
    Document doc;
    doc.reset(stack({{"sky", Image::filled(Extent{4, 4}, 30, 60, 90, 255)},
                     {"bird", Image::filled(Extent{4, 4}, 250, 250, 250, 200)},
                     {"haze", Image::filled(Extent{4, 4}, 12, 12, 12, 255)},
                     {"ghost", Image::filled(Extent{4, 4}, 200, 0, 0, 255)}}),
              std::nullopt, "x.noise");
    Mask mask(Extent{4, 4});
    mask.op(Rect{0, 0, 4, 2}, MaskOp::replace);
    CHECK(doc.set_layer_mask(1, mask));
    CHECK(doc.set_layer_opacity(2, 0.4));
    CHECK(doc.set_layer_mode(2, BlendMode::multiply));
    // A layer nobody can see is not part of the picture, and is not part of the flattened layer either -
    // but it is part of the document, so undo gives it back.
    CHECK(doc.set_layer_visible(3, false));

    const std::string shown = doc.composite().checksum_hex();
    const std::string whole = doc.checksum();
    const std::string names = layer_names(doc);
    const std::string bird_pixels = doc.layer(1).pixels.checksum_hex();
    // Ids, not just names: a card caches a thumbnail against the id, the active layer indexes a stack
    // that has just changed shape, and every later step names its layer by id - an id minted twice is not
    // a cosmetic slip.
    const std::string ids = layer_ids(doc);

    Document alone;
    alone.reset(stack({{"only", Image::filled(Extent{2, 2}, 1, 1, 1, 255)}}), std::nullopt,
                "y.noise");
    CHECK(!alone.flatten_layers());

    CHECK(doc.flatten_layers());
    CHECK_EQ(doc.layers().size(), 1u);
    CHECK_EQ(doc.layer(0).name, std::string(kFlattenedLayerName));
    // Neutral by necessity, not by tidiness: with these the composite *is* the layer, so the canvas
    // cannot re-blend what was just baked into it.
    CHECK(doc.layer(0).visible);
    CHECK(doc.layer(0).opacity == kLayerOpacityDefault);
    CHECK(doc.layer(0).mode == BlendMode::normal);
    CHECK(!doc.layer(0).mask.has_value());
    CHECK_EQ(doc.composite().checksum_hex(), shown);
    CHECK_EQ(doc.history().undo_depth, 2);  // the mask put down below, and the replaced stack
    CHECK(doc.dirty());

    CHECK(doc.undo());
    CHECK_EQ(doc.layers().size(), 4u);
    CHECK_EQ(layer_names(doc), names);
    CHECK_EQ(doc.layer(1).pixels.checksum_hex(), bird_pixels);
    CHECK(doc.layer(1).mask.has_value());
    CHECK(doc.layer(2).opacity == 0.4);
    CHECK(doc.layer(2).mode == BlendMode::multiply);
    CHECK(!doc.layer(3).visible);
    CHECK_EQ(doc.composite().checksum_hex(), shown);
    CHECK_EQ(doc.checksum(), whole);

    CHECK(doc.redo());
    CHECK_EQ(doc.layers().size(), 1u);
    CHECK_EQ(doc.composite().checksum_hex(), shown);
    // The second round trip is as exact as the first: undoing a replaced stack and redoing it are the
    // same swap, so nothing about the order the layers return in can drift with how often it is run.
    CHECK(doc.undo());
    CHECK_EQ(layer_names(doc), names);
    CHECK_EQ(layer_ids(doc), ids);  // the same identities, in the same places, second time round
    CHECK_EQ(doc.checksum(), whole);
    // The bottom of the timeline is one step away: opacity, mode and visibility travel with their layer
    // and enter no step, but the mask the layer was wearing does, and undoing it takes the coverage off
    // the layer the flatten had just given back.
    CHECK(doc.undo());
    CHECK(!doc.layer(1).mask.has_value());
    CHECK_EQ(doc.layer(1).pixels.checksum_hex(), bird_pixels);
    CHECK(!doc.can_undo());
    CHECK_EQ(doc.history().undo_depth, 0);

    // A step recorded *after* the flatten names the very layer the swap takes away. Stepping back has to
    // undo the paint while that layer is still there and only then replace the stack - the one case that
    // decides whether a whole-stack entry and a pixel entry can share a timeline.
    CHECK(doc.redo());  // the mask back on the stack that is there
    CHECK(doc.layer(1).mask.has_value());
    CHECK(doc.redo());  // and the stack replaced again
    paint_on(doc, Rect{0, 0, 2, 2}, 7);
    const std::string painted = doc.composite().checksum_hex();
    CHECK(painted != shown);
    CHECK(doc.undo());
    CHECK_EQ(doc.composite().checksum_hex(), shown);
    CHECK(doc.undo());
    CHECK_EQ(layer_names(doc), names);
    CHECK_EQ(doc.checksum(), whole);
    CHECK(doc.redo());
    CHECK(doc.redo());
    CHECK_EQ(doc.layers().size(), 1u);
    CHECK_EQ(doc.composite().checksum_hex(), painted);

    // A stack of one has nothing to flatten, and refusing costs the timeline nothing: the steps are
    // still exactly the mask, the flatten and the paint.
    CHECK(!doc.flatten_layers());
    CHECK_EQ(doc.history().undo_depth, 3);
}

// A name is what a person calls a layer afterwards, so it is saved, it is undone, and losing one in a
// round trip is data loss like losing a `visible` flag. It is not a pixel: nothing the renderer uploads,
// and nothing a card's picture shows, may move over it.
// A layer is the size of the canvas and stays that way, so scaling one scales what it *holds*, about the
// middle of that, and leaves the rest of its rectangle transparent. Every claim here is one a wrong anchor,
// a filter that spread past the picture, or a half-restored undo would break.
void test_a_layers_picture_scales_about_the_middle_of_what_it_holds() {
    Document doc;
    doc.reset(stack({{"under", Image::filled(Extent{48, 32}, 9, 9, 9, 255)},
                     {"sky", block(Extent{48, 32}, Rect{16, 12, 12, 8}, 128)}}),
              std::nullopt, "x.noise");
    const std::string before_layer = doc.layer(1).pixels.checksum_hex();
    const std::string before_under = doc.layer(0).pixels.checksum_hex();
    const int steps = doc.history().undo_depth;

    // The block spans x 16..28, y 12..20, so its middle is the document point (22, 16) and twice the size
    // it lands x 10..34, y 8..24. An anchor a pixel off, or a size measured from a corner, fails here.
    const LayerScale grown = doc.scale_layer(1, 2.0);
    CHECK(grown.done);
    CHECK_EQ(rect_text(grown.picture), std::string("10,8,24x16"));
    CHECK(doc.dirty());
    CHECK_EQ(doc.history().undo_depth, steps + 1);

    // The middle is still the middle, and a flat opaque field well inside the picture is still that field:
    // Lanczos is zero at every integer offset but its own.
    const Image& grown_pixels = doc.layer(1).pixels;
    CHECK_EQ(grown_pixels.at(22, 16)[0], 128);
    CHECK_EQ(grown_pixels.at(22, 16)[3], 255);
    CHECK_EQ(grown_pixels.at(0, 0)[3], 0);   // the filter's ring stays near the picture
    CHECK_EQ(grown_pixels.at(47, 31)[3], 0);
    CHECK_EQ(doc.layer(0).pixels.checksum_hex(), before_under);  // one layer scaled, not the stack

    // What the layer holds afterwards is exactly the box the command reported - the ring the filter throws
    // past an edge is cut at that edge. This is what lets a second scale be aimed at the same picture
    // rather than at the picture plus a halo, which is how chained scales would otherwise creep.
    CHECK_EQ(rect_text(grown_pixels.content_bounds()), std::string("10,8,24x16"));

    // Halving goes back through the same middle: twice as big, then half, is the box that went in.
    const LayerScale shrunk = doc.scale_layer(1, 0.5);
    CHECK(shrunk.done);
    CHECK_EQ(rect_text(shrunk.picture), std::string("16,12,12x8"));

    // And the whole thing comes back as bytes. Two steps out, two steps back, and the layer is the layer
    // that went in - whole-layer equality, because a scale reaches all of a picture and a sampled point
    // would miss the edge of it.
    CHECK(doc.undo());
    CHECK(doc.undo());
    CHECK_EQ(doc.layer(1).pixels.checksum_hex(), before_layer);
    CHECK(doc.redo());
    CHECK(doc.redo());
    CHECK(doc.layer(1).pixels.checksum_hex() != before_layer);
    CHECK_EQ(rect_text(doc.layer(1).pixels.content_bounds()), std::string("16,12,12x8"));
}

// A layer that fills the canvas has nowhere to grow. The refusal carries the largest factor that does fit,
// and that factor is quoted because it works: "it did not fit" without a number is a second job.
void test_a_scale_that_does_not_fit_names_the_largest_one_that_does() {
    Document doc;
    doc.reset(stack({{"sky", Image::filled(Extent{12, 8}, 200, 200, 200, 255)}}), std::nullopt,
              "x.noise");
    const std::string whole = doc.layer(0).pixels.checksum_hex();
    const int steps = doc.history().undo_depth;

    const LayerScale refused = doc.scale_layer(0, 1.5);
    CHECK(!refused.done);
    CHECK(refused.max_factor >= 1.0);
    CHECK(refused.max_factor < 1.1);
    CHECK_EQ(doc.layer(0).pixels.checksum_hex(), whole);  // a refusal moves no pixel
    CHECK_EQ(doc.history().undo_depth, steps);            // and costs no step
    CHECK(!doc.can_undo());

    CHECK(doc.scale_layer(0, refused.max_factor).done);
}

// A layer holding nothing, an index out of the stack and a factor that is no size are all refused without
// touching the document; and twice the size it already is is a copy that records nothing.
void test_a_scale_that_means_nothing_is_refused_and_costs_nothing() {
    Document doc;
    doc.reset(stack({{"sky", block(Extent{24, 16}, Rect{8, 4, 8, 8}, 40)},
                     {"blank", Image::filled(Extent{24, 16}, 0, 0, 0, 0)}}),
              std::nullopt, "x.noise");
    const int steps = doc.history().undo_depth;
    const bool dirty_before = doc.dirty();

    CHECK(!doc.scale_layer(1, 2.0).done);  // nothing to scale about: every pixel of it is transparent
    CHECK(!doc.scale_layer(2, 1.5).done);  // out of the stack
    CHECK(!doc.scale_layer(-1, 1.5).done);
    CHECK(!doc.scale_layer(0, 0.0).done);  // no size at all
    CHECK(!doc.scale_layer(0, -3.0).done);
    CHECK(!doc.scale_layer(0, std::numeric_limits<double>::quiet_NaN()).done);
    CHECK(!doc.scale_layer(0, std::numeric_limits<double>::infinity()).done);
    CHECK_EQ(doc.history().undo_depth, steps);
    CHECK_EQ(doc.dirty(), dirty_before);

    CHECK(doc.scale_layer(0, 2.0).done);
    const std::uint64_t revision = doc.revision();
    const int after_grow = doc.history().undo_depth;
    // The same size again lands exactly where it stood, so there is nothing to undo - and no fit question
    // either, because a copy of what already fits fits.
    CHECK(doc.scale_layer(0, 1.0).done);
    CHECK_EQ(doc.revision(), revision);
    CHECK_EQ(doc.history().undo_depth, after_grow);
}

// A layer's mask is a claim about the same pixels as its picture, so both go through the same map: scaling
// one without the other would hide parts that were shown and show parts that were hidden. A mask that
// covers everything says the same thing at any size, and one that reaches a canvas edge bounds how much
// the picture may grow.
void test_a_scale_takes_the_coverage_with_the_picture() {
    Document doc;
    doc.reset(stack({{"sky", block(Extent{48, 32}, Rect{16, 12, 12, 8}, 160)}}), std::nullopt,
              "x.noise");
    Mask middle(Extent{48, 32});
    middle.op(Rect{12, 8, 20, 16}, MaskOp::replace);  // a window narrower than the picture on the left
    CHECK(doc.set_layer_mask(0, middle));
    const std::string mask_before(doc.layer(0).mask->plane().begin(),
                                  doc.layer(0).mask->plane().end());
    const std::string pixels_before = doc.layer(0).pixels.checksum_hex();
    const int steps = doc.history().undo_depth;

    const LayerScale grown = doc.scale_layer(0, 1.5);
    CHECK(grown.done);
    // The coverage grew about the picture's middle, so its left edge moved left and its right edge right.
    CHECK(doc.layer(0).mask->bounds().x < 12);
    CHECK(doc.layer(0).mask->bounds().right() > 32);
    // Both planes of one layer under one map is one thing a person did: one step, not two.
    CHECK_EQ(doc.history().undo_depth, steps + 1);

    CHECK(doc.undo());
    CHECK_EQ(doc.layer(0).pixels.checksum_hex(), pixels_before);
    CHECK_EQ(std::string(doc.layer(0).mask->plane().begin(), doc.layer(0).mask->plane().end()),
             mask_before);
    CHECK_EQ(doc.layer(0).mask->bounds().x, 12);
    CHECK_EQ(doc.layer(0).mask->bounds().right(), 32);

    // The window is what limits the growth here, not the picture: the mask has less room than the block it
    // hides, and the refusal says the number its own edges allow.
    const double mask_limited = doc.scale_layer(0, 4.0).max_factor;
    CHECK(!doc.scale_layer(0, 4.0).done);
    CHECK(mask_limited > 2.0);
    CHECK(mask_limited < 2.1);

    // A mask that hides nothing anywhere is the same mask at any size: filtering a flat plane would spend
    // a pass putting ringing where there was none, so it is left byte for byte as it was.
    Mask whole(Extent{48, 32});
    whole.op(Rect{0, 0, 48, 32}, MaskOp::replace);
    CHECK(doc.set_layer_mask(0, whole));
    const std::string flat(doc.layer(0).mask->plane().begin(), doc.layer(0).mask->plane().end());
    CHECK(doc.scale_layer(0, 1.25).done);
    CHECK_EQ(std::string(doc.layer(0).mask->plane().begin(), doc.layer(0).mask->plane().end()), flat);
}

void test_a_layer_takes_a_name_and_gives_the_old_one_back() {
    Document doc;
    doc.reset(stack({{"sky", Image::filled(Extent{4, 4}, 10, 20, 30, 255)},
                     {"bird", Image::filled(Extent{4, 4}, 200, 200, 200, 128)}}),
              std::nullopt, "x.noise");
    const std::string whole = doc.checksum();
    const std::string shown = doc.composite().checksum_hex();
    const std::string ids = layer_ids(doc);
    const std::map<std::uint64_t, std::uint64_t> held = content_revisions(doc);
    const std::uint64_t revision = doc.revision();

    CHECK(!doc.rename_layer(2, "out of the stack"));
    CHECK(!doc.rename_layer(-1, "below the bottom"));
    // Nothing to point at by name, and a name nobody could read to the end: both are refused rather than
    // quietly stored as something else.
    CHECK(!doc.rename_layer(0, "   \t \n"));
    CHECK(!doc.rename_layer(0, std::string(kLayerNameMaxChars + 1, 'x')));

    // The space around a name is not part of it.
    CHECK(doc.rename_layer(1, "  the bird  "));
    CHECK_EQ(doc.layer(1).name, std::string("the bird"));
    CHECK(doc.dirty());
    CHECK_EQ(layer_ids(doc), ids);  // the same layers renamed, not new ones wearing the name
    CHECK_EQ(doc.composite().checksum_hex(), shown);
    CHECK_EQ(doc.revision(), revision);      // nothing for a renderer to re-upload
    CHECK(content_revisions(doc) == held);   // and no card has to resample a picture
    CHECK(doc.checksum() != whole);          // the name is part of what the document is
    CHECK_EQ(doc.history().undo_depth, 1);

    CHECK(doc.undo());
    CHECK_EQ(doc.layer(1).name, std::string("bird"));
    CHECK_EQ(doc.checksum(), whole);
    // Undoing a name is as invisible to a renderer as typing one was: a step that painted no rectangle and
    // moved no layer re-uploads no texture and resamples no card.
    CHECK_EQ(doc.revision(), revision);
    CHECK(content_revisions(doc) == held);
    CHECK(doc.redo());
    CHECK_EQ(doc.layer(1).name, std::string("the bird"));
    CHECK_EQ(doc.revision(), revision);
    CHECK(content_revisions(doc) == held);

    // A rename rides with the layer through a step about the stack: the survivor keeps its own name, and
    // undo returns the renamed layer renamed rather than back to what it was called before.
    CHECK(doc.rename_layer(0, "the sky"));
    const std::string renamed = doc.checksum();
    CHECK(renamed != whole);
    CHECK(doc.merge_down(1));
    CHECK_EQ(doc.layers().size(), 1u);
    CHECK_EQ(doc.layer(0).name, std::string("the sky"));
    CHECK(doc.undo());
    CHECK_EQ(layer_names(doc), std::string("the sky,the bird"));
    CHECK_EQ(doc.checksum(), renamed);
}

// Both fences at once: the selection limits what a write may change, the layer mask limits what the
// canvas shows of what was changed. A stroke under a masked-out area is real - it is on the layer -
// and simply is not visible until the mask moves.
void test_a_selection_and_a_mask_fence_different_things() {
    Document doc;
    doc.reset(stack({{"Ink", Image::filled(Extent{10, 4}, 0, 0, 0, 0)}}), std::nullopt, "x.noise");

    Mask selection(Extent{10, 4});
    selection.op(Rect{0, 0, 8, 4}, MaskOp::replace);
    CHECK(doc.set_selection(selection));
    Mask mask(Extent{10, 4});
    mask.op(Rect{0, 0, 6, 4}, MaskOp::replace);
    CHECK(doc.set_layer_mask(0, mask));

    // Outside the selection: the write door is the selection, so nothing lands anywhere.
    const std::string empty = doc.layer(0).pixels.checksum_hex();
    const int marked = doc.history().undo_depth;  // the mark itself is a step
    paint_on(doc, Rect{8, 1, 2, 2}, 180);
    CHECK_EQ(doc.layer(0).pixels.checksum_hex(), empty);
    CHECK_EQ(doc.history().undo_depth, marked);

    // Inside both: the layer changes and the canvas shows it.
    paint_on(doc, Rect{1, 1, 2, 2}, 180);
    CHECK(doc.layer(0).pixels.checksum_hex() != empty);
    CHECK_EQ(doc.composite().at(1, 1)[3], 255);

    // Inside the selection but under the mask: the write is allowed and the canvas keeps it out of
    // sight, which is the whole point of a non-destructive mask.
    paint_on(doc, Rect{7, 1, 1, 2}, 180);
    CHECK_EQ(doc.composite().at(7, 1)[3], 0);

    // With the selection down the same pixels are writable, and still hidden - so the layer, not
    // the composite, is what changed.
    CHECK(doc.set_selection(std::nullopt));
    paint_on(doc, Rect{9, 1, 1, 2}, 180);
    CHECK_EQ(doc.composite().at(9, 1)[3], 0);

    // Taking the mask off shows every byte that was painted under it.
    CHECK(doc.set_layer_mask(0, std::nullopt));
    CHECK_EQ(doc.composite().at(7, 1)[3], 255);
    CHECK_EQ(doc.composite().at(9, 1)[3], 255);
}

// The marked area is a document property that is saved, so it is undoable - and the step that puts it
// back moves coverage and nothing else. A build that bumped the stack's revision for a mark would
// re-upload a canvas nobody repainted and resample every layer thumbnail to prove nothing happened.
void test_marking_the_area_is_one_step_that_moves_no_pixel() {
    const Extent size{12, 6};
    Document doc;
    doc.reset(stack({{"Sky", Image::filled(size, 9, 9, 9, 255)}}), std::nullopt, "x.noise");
    const std::string canvas = doc.composite().checksum_hex();

    Mask corner(size);
    corner.op(Rect{2, 1, 5, 4}, MaskOp::replace);
    CHECK(doc.set_selection(corner));
    CHECK_EQ(doc.history().undo_depth, 1);
    CHECK(doc.dirty());
    CHECK_EQ(doc.composite().checksum_hex(), canvas);

    paint_on(doc, Rect{2, 1, 3, 2}, 200);  // inside the mark, so it lands - and is its own step
    CHECK_EQ(doc.history().undo_depth, 2);
    CHECK(doc.undo());                      // pixels first: the mark was not part of that step
    CHECK_EQ(doc.history().undo_depth, 1);
    REQUIRE(doc.selection().has_value());
    CHECK_EQ(doc.selection()->covered(), corner.covered());

    const std::uint64_t revision = doc.revision();
    const std::map<std::uint64_t, std::uint64_t> cards = content_revisions(doc);
    CHECK(doc.undo());
    CHECK(!doc.selection().has_value());
    CHECK_EQ(doc.composite().checksum_hex(), canvas);
    CHECK_EQ(doc.revision(), revision);
    CHECK(content_revisions(doc) == cards);

    // Forward again is the same operation, and the coverage that comes back is byte for byte the one
    // that was put down - partial coverage included, because a mark is not a box.
    Mask soft(size);
    soft.op(Rect{2, 1, 5, 4}, MaskOp::replace);
    soft.row(1)[3] = 128;
    CHECK(doc.set_selection(soft));
    CHECK(doc.undo());
    CHECK(!doc.selection().has_value());
    CHECK(doc.redo());
    REQUIRE(doc.selection().has_value());
    for (int y = 0; y < size.h; ++y) {
        for (int x = 0; x < size.w; ++x) {
            CHECK_EQ(static_cast<int>(doc.selection()->coverage(x, y)),
                     static_cast<int>(soft.coverage(x, y)));
        }
    }
    CHECK_EQ(doc.revision(), revision);
    CHECK(content_revisions(doc) == cards);
}

// A marquee or a `cover` stroke is one gesture and one undo step, however many events it arrived as,
// and the coat is the stroke's own - not one coat per frame. The middle event lies inside the union of
// the other two, so an implementation that painted onto what it had already written comes back darker
// there than the single coat below.
void test_a_marked_gesture_is_one_step_painted_in_one_coat() {
    const Extent size{12, 6};
    Document doc;
    doc.reset(stack({{"Sky", Image::filled(size, 9, 9, 9, 255)}}), std::nullopt, "x.noise");
    Mask started(size);
    started.op(Rect{0, 0, 3, size.h}, MaskOp::replace);
    CHECK(doc.set_selection(started));
    const int steps = doc.history().undo_depth;
    const std::string canvas = doc.composite().checksum_hex();

    CHECK(doc.begin_marked());
    CHECK(doc.in_marked());
    for (const Rect event : {Rect{2, 0, 5, 2}, Rect{2, 2, 5, 4}, Rect{4, 1, 3, 2}}) {
        doc.repaint_marked(event, [](Mask& mask, Rect area) {
            mask.paint(area, coat(area, 128), MaskInk::reveal);
        });
        // Live on the canvas, and still no step: the gesture commits once, at release.
        CHECK_EQ(doc.history().undo_depth, steps);
    }
    doc.commit_marked();
    CHECK(!doc.in_marked());
    CHECK_EQ(doc.history().undo_depth, steps + 1);
    CHECK_EQ(doc.composite().checksum_hex(), canvas);

    Mask one_coat(started);
    one_coat.paint(Rect{2, 0, 5, size.h}, coat(Rect{2, 0, 5, size.h}, 128), MaskInk::reveal);
    REQUIRE(doc.selection().has_value());
    for (int y = 0; y < size.h; ++y) {
        for (int x = 0; x < size.w; ++x) {
            CHECK_EQ(static_cast<int>(doc.selection()->coverage(x, y)),
                     static_cast<int>(one_coat.coverage(x, y)));
        }
    }

    // And the step puts back the coverage the press found, which was `started` - including the column
    // the gesture never touched, which is what makes the coat reversible rather than merely darker.
    CHECK(doc.undo());
    REQUIRE(doc.selection().has_value());
    for (int y = 0; y < size.h; ++y) {
        for (int x = 0; x < size.w; ++x) {
            CHECK_EQ(static_cast<int>(doc.selection()->coverage(x, y)),
                     static_cast<int>(started.coverage(x, y)));
        }
    }
    CHECK_EQ(doc.history().undo_depth, steps);
}

// A gesture that moved no coverage - a coat of no ink over what is already there - is not an edit:
// no step, no dirty mark. Otherwise tapping the canvas with `cover` would spend the undo budget
// restoring coverage nobody changed.
void test_a_marked_gesture_that_moved_nothing_costs_nothing() {
    const Extent size{10, 4};
    Document doc;
    doc.reset(stack({{"Sky", Image::filled(size, 1, 1, 1, 255)}}), std::nullopt, "x.noise");
    Mask started(size);
    started.op(Rect{1, 0, 4, size.h}, MaskOp::replace);
    CHECK(doc.set_selection(started));
    doc.mark_saved();
    const int steps = doc.history().undo_depth;

    CHECK(doc.begin_marked());
    doc.repaint_marked(Rect{0, 0, size.w, size.h}, [](Mask& mask, Rect area) {
        mask.paint(area, coat(area, 0), MaskInk::reveal);
    });
    CHECK(!doc.dirty());
    doc.commit_marked();
    CHECK_EQ(doc.history().undo_depth, steps);
    REQUIRE(doc.selection().has_value());
    CHECK_EQ(doc.selection()->covered(), started.covered());

    // Writing what is already there inside a gesture is the same nothing, through the other door.
    CHECK(doc.begin_marked());
    CHECK(doc.set_selection(started));
    doc.commit_marked();
    CHECK_EQ(doc.history().undo_depth, steps);
    CHECK(!doc.dirty());
}

// A gesture that is called off - the frame that stops caring about the drag - gives back the coverage
// the press found, including the difference between "no marked area" and "a plane covering nothing".
void test_cancel_marked_gives_back_the_coverage_the_press_found() {
    const Extent size{8, 4};
    Document doc;
    doc.reset(stack({{"Sky", Image::filled(size, 3, 3, 3, 255)}}), std::nullopt, "x.noise");
    const int steps = doc.history().undo_depth;

    CHECK(doc.begin_marked());
    Mask box(size);
    box.op(Rect{2, 1, 4, 2}, MaskOp::replace);
    CHECK(doc.set_selection(box));
    CHECK(doc.selection().has_value());
    doc.cancel_marked();
    CHECK(!doc.in_marked());
    CHECK(!doc.selection().has_value());  // there was nothing to find, so nothing comes back
    CHECK_EQ(doc.history().undo_depth, steps);

    // The same, from a document that did have a marked area: it comes back byte for byte, the
    // half-covered cell with the rest of it.
    Mask started(size);
    started.op(Rect{0, 0, 2, size.h}, MaskOp::replace);
    started.row(2)[1] = 96;
    CHECK(doc.set_selection(started));
    const int marked = doc.history().undo_depth;
    CHECK(doc.begin_marked());
    doc.repaint_marked(Rect{3, 0, 5, size.h}, [](Mask& mask, Rect area) {
        mask.op(area, MaskOp::subtract);
    });
    CHECK_EQ(doc.selection()->covered(), static_cast<std::size_t>(2 * size.h));
    doc.cancel_marked();
    REQUIRE(doc.selection().has_value());
    for (int y = 0; y < size.h; ++y) {
        for (int x = 0; x < size.w; ++x) {
            CHECK_EQ(static_cast<int>(doc.selection()->coverage(x, y)),
                     static_cast<int>(started.coverage(x, y)));
        }
    }
    CHECK_EQ(doc.history().undo_depth, marked);
}

// One press writes one thing: the bytes a gesture held aside belong to the plane it was pressed on,
// so a marked gesture and a layer gesture cannot share a frame - and the refusal says which one it
// already holds rather than quietly writing into both.
void test_a_marked_gesture_and_a_layer_gesture_are_different_presses() {
    const Extent size{8, 4};
    Document doc;
    doc.reset(stack({{"Sky", Image::filled(size, 4, 4, 4, 255)}}), std::nullopt, "x.noise");
    CHECK(doc.begin_provisional(Rect{0, 0, 4, 4}, LayerPlane::pixels));
    CHECK(!doc.begin_marked());
    CHECK(!doc.in_marked());
    doc.discard_provisional();

    CHECK(doc.begin_marked());
    CHECK(!doc.begin_provisional(Rect{0, 0, 4, 4}, LayerPlane::pixels));
    doc.commit_marked();
    // A gesture that never wrote is not an edit, and the refused attempt added nothing to it.
    CHECK_EQ(doc.history().undo_depth, 0);
}

}  // namespace

int main() {
    test_the_incremental_composite_equals_one_built_from_scratch();
    test_the_stack_blends_bottom_first();
    test_a_hidden_layer_contributes_nothing();
    test_layer_opacity_scales_the_blend();
    test_a_lone_layers_composite_is_the_blend_paths_answer_for_every_mode();
    test_a_lone_layer_is_shown_whole_only_at_full_strength();
    test_setting_a_mode_moves_the_picture_and_touches_nothing_else();
    test_layer_state_changes_reach_the_renderer();
    test_layer_state_refuses_what_was_asked_for();
    test_repairing_a_rectangle_leaves_the_rest_of_the_canvas_alone();
    test_the_stack_keeps_a_usable_selection();
    test_names_are_generated_from_the_stack_size();
    test_the_document_checksum_follows_the_stack();
    test_the_document_checksum_sees_a_layer_mask();
    test_a_wholly_shown_mask_hides_nothing_and_a_wholly_hidden_one_hides_everything();
    test_a_wholly_shown_mask_costs_the_composite_nothing([] {
        static const auto epoch = std::chrono::steady_clock::now();
        return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - epoch)
            .count();
    });
    test_no_selection_and_an_empty_selection_are_different_things();
    test_a_selection_holds_every_write_inside_it();
    test_an_edit_carrying_its_own_coverage_reaches_past_the_selection();
    test_a_partly_covered_selection_paints_partly();
    test_a_gesture_clips_to_the_selection_and_undoes_exactly();
    test_painting_a_mask_is_one_undo_step_that_hides_and_shows();
    test_the_selection_fences_a_mask_stroke_too();
    test_a_mask_gesture_is_fenced_by_the_selection();
    test_a_mask_stroke_that_changed_nothing_costs_nothing();
    test_painting_a_layer_with_no_mask_is_refused();
    test_a_gesture_holds_one_plane();
    test_taking_the_mask_off_ends_the_gesture_and_is_itself_undoable();
    test_a_selection_of_another_size_is_refused();
    test_a_selection_change_is_a_document_change();
    test_a_layer_mask_hides_without_editing_the_layer();
    test_a_layer_mask_of_another_size_is_refused();
    test_a_layer_mask_change_is_a_document_change();
    test_a_selection_and_a_mask_fence_different_things();
    test_a_layers_picture_scales_about_the_middle_of_what_it_holds();
    test_a_scale_that_does_not_fit_names_the_largest_one_that_does();
    test_a_scale_that_means_nothing_is_refused_and_costs_nothing();
    test_a_scale_takes_the_coverage_with_the_picture();
    test_a_layer_takes_a_name_and_gives_the_old_one_back();
    test_what_a_layer_holds_and_what_the_composite_uses_are_two_counters();
    test_a_layer_moves_as_one_step_that_undoes_itself();
    test_merging_down_shows_what_the_canvas_already_showed();
    test_flattening_leaves_one_layer_that_shows_what_the_stack_showed();
    test_marking_the_area_is_one_step_that_moves_no_pixel();
    test_a_marked_gesture_is_one_step_painted_in_one_coat();
    test_a_marked_gesture_that_moved_nothing_costs_nothing();
    test_cancel_marked_gives_back_the_coverage_the_press_found();
    test_a_marked_gesture_and_a_layer_gesture_are_different_presses();
    return noise::test::summary("document");
}
