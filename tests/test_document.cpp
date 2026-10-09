// SPDX-License-Identifier: Apache-2.0
#include "test.h"

#include "core/document.h"

#include <cstdint>
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

void paint_on(Document& doc, Rect rect, std::uint8_t grey) {
    doc.paint(rect, [rect, grey](Image& image) {
        const std::uint8_t rgba[4] = {grey, grey, grey, 255};
        image.fill_rect(rect, rgba);
    });
}

// The composite is repaired over the rectangles that changed, so the load-bearing property is
// that it ends up identical to one built from scratch. A partial repair that missed a region, or
// blended a layer twice, shows up here and nowhere else.
void test_the_incremental_composite_equals_one_built_from_scratch() {
    Document live;
    live.reset(stack({{"Paper", Image::filled(Extent{24, 18}, 20, 20, 20, 255)}}), "x.noise");

    const auto check_against_scratch = [&live](const char* where) {
        std::vector<Layer> copy;
        for (const Layer& layer : live.layers()) {
            Layer clone;
            clone.name = layer.name;
            clone.pixels = layer.pixels;
            clone.visible = layer.visible;
            clone.opacity = layer.opacity;
            clone.mode = layer.mode;
            copy.push_back(std::move(clone));
        }
        Document fresh;
        fresh.reset(std::move(copy), "x.noise");
        const std::string built = live.composite().checksum_hex();
        const std::string scratch = fresh.composite().checksum_hex();
        if (built == scratch) {
            ++noise::test::checks;
            return;
        }
        noise::test::report("incremental composite == composite from scratch", __FILE__,
                            __LINE__, std::string(where) + ": got " + built + " want " + scratch);
    };

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
        one.reset(std::move(stack), "x.noise");

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
    doc.reset(stack({{"Lone", pixels}}), "x.noise");
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
              "x.noise");
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
    doc.reset(stack({{"Bottom", bottom}, {"Top", top}}), "x.noise");
    const std::string with_top = doc.composite().checksum_hex();

    CHECK(doc.set_layer_visible(1, false));
    Document without;
    without.reset(stack({{"Bottom", bottom}}), "x.noise");
    CHECK_EQ(doc.composite().checksum_hex(), without.composite().checksum_hex());
    CHECK_NE(with_top, doc.composite().checksum_hex());
    CHECK_EQ(doc.layer(1).pixels.checksum_hex(), top.checksum_hex());

    // The single-layer shortcut has to respect it too: one hidden layer is a transparent canvas,
    // not that layer handed straight to the renderer.
    Document single;
    single.reset(stack({{"Only", top}}), "x.noise");
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
              "x.noise");
    const std::string shown = doc.composite().checksum_hex();
    CHECK(doc.set_layer_opacity(1, 0.0));
    Document only_bottom;
    only_bottom.reset(stack({{"Bottom", Image::filled(size, 255, 0, 0, 255)}}), "x.noise");
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
              "x.noise");
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
    doc.reset(stack({{"Only", Image::filled(Extent{4, 4}, 1, 1, 1, 255)}}), "x.noise");
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
    doc.reset(stack({{"Bottom", bottom}, {"Top", top}}), "x.noise");
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
              "x.noise");
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
              "x.noise");
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
    doc.reset(stack({{"Kept", Image::filled(Extent{4, 4}, 0, 0, 0, 0)}}), "x.noise");
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
    one.reset(stack({{"A", Image::filled(Extent{8, 8}, 1, 1, 1, 255)}}), "x.noise");
    Document two;
    two.reset(stack({{"A", Image::filled(Extent{8, 8}, 1, 1, 1, 255)},
                     {"B", Image::filled(Extent{8, 8}, 0, 0, 0, 0)}}),
              "x.noise");
    CHECK_NE(one.checksum(), two.checksum());
    // Same two layers, opposite order: the flattened result differs, and so must the checksum.
    Document three;
    three.reset(stack({{"B", Image::filled(Extent{8, 8}, 0, 0, 0, 0)},
                       {"A", Image::filled(Extent{8, 8}, 1, 1, 1, 255)}}),
                "x.noise");
    CHECK_NE(two.checksum(), three.checksum());
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
    return noise::test::summary("document");
}
