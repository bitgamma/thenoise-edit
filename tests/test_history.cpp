// SPDX-License-Identifier: Apache-2.0
#include "test.h"

#include "core/document.h"

#include <cstdint>
#include <string>
#include <vector>

using namespace noise;

namespace {

Image make_pattern(Extent size) {
    Image image = Image::filled(size, 0, 0, 0, 0);
    for (int y = 0; y < size.h; ++y) {
        for (int x = 0; x < size.w; ++x) {
            std::uint8_t* pixel = image.at(x, y);
            pixel[0] = static_cast<std::uint8_t>((x * 3 + y) & 0xff);
            pixel[1] = static_cast<std::uint8_t>((x + y * 7) & 0xff);
            pixel[2] = static_cast<std::uint8_t>((x * y) & 0xff);
            pixel[3] = static_cast<std::uint8_t>((x % 3 == 0) ? 0 : 255);
        }
    }
    return image;
}

void paint_rect(Image& image, Rect rect, std::uint8_t grey) {
    const std::uint8_t rgba[4] = {grey, grey, grey, 255};
    image.fill_rect(rect, rgba);
}

// A mutation the way a tool makes one: fill a rectangle and declare which one it was.
void fill(Document& doc, Rect rect, std::uint8_t grey) {
    doc.paint(rect, [rect, grey](Image& image) { paint_rect(image, rect, grey); });
}

std::size_t differing_pixels(const Image& a, const Image& b) {
    std::size_t changed = 0;
    for (std::size_t i = 0; i < a.pixels().size(); ++i) {
        changed += a.pixels()[i] != b.pixels()[i] ? 1u : 0u;
    }
    return changed;
}

void test_undo_after_many_mutations_restores_every_byte() {
    Document doc;
    doc.reset(make_pattern(Extent{64, 48}), "pattern.png");
    const std::string original = doc.checksum();

    int grey = 8;
    for (int i = 0; i < 12; ++i) {
        fill(doc, Rect{i * 3, (i * 5) % 40, 9 + i, 7 + (i % 6)}, static_cast<std::uint8_t>(grey += 13));
        CHECK(doc.can_undo());
    }
    const std::string painted = doc.checksum();
    CHECK_NE(painted, original);
    CHECK_EQ(doc.history().undo_depth, 12);
    CHECK_EQ(doc.history().redo_depth, 0);

    for (int i = 11; i >= 0; --i) {
        CHECK(doc.undo());
        CHECK_EQ(doc.history().undo_depth, i);
        CHECK_EQ(doc.history().redo_depth, 12 - i);
    }
    CHECK_EQ(doc.checksum(), original);
    CHECK(!doc.can_undo());

    // Redo runs the same steps forward and lands on the same bytes, not an approximation.
    for (int i = 0; i < 12; ++i) {
        CHECK(doc.redo());
    }
    CHECK_EQ(doc.checksum(), painted);
    CHECK(!doc.can_redo());
}

void test_undo_touches_only_the_rectangle_it_recorded() {
    Document doc;
    doc.reset(make_pattern(Extent{40, 30}), "pattern.png");
    const std::string untouched = doc.checksum();

    fill(doc, Rect{5, 4, 12, 9}, 200);
    const std::string after_first_step = doc.checksum();
    fill(doc, Rect{20, 2, 6, 6}, 30);
    CHECK(doc.undo());

    // Putting one rectangle back must leave every other pixel exactly as it was, so the
    // result has to equal a document that only ever ran the first step.
    Document reference;
    reference.reset(make_pattern(Extent{40, 30}), "pattern.png");
    fill(reference, Rect{5, 4, 12, 9}, 200);
    CHECK_EQ(differing_pixels(reference.pixels(), doc.pixels()), 0u);
    CHECK_EQ(doc.checksum(), after_first_step);
    CHECK_NE(doc.checksum(), untouched);
}

void test_a_mutation_after_an_undo_forks_the_timeline() {
    Document doc;
    doc.reset(make_pattern(Extent{24, 24}), "pattern.png");
    fill(doc, Rect{0, 0, 4, 4}, 10);
    fill(doc, Rect{8, 8, 4, 4}, 20);
    CHECK_EQ(doc.history().redo_depth, 0);
    CHECK(doc.undo());
    CHECK_EQ(doc.history().redo_depth, 1);
    fill(doc, Rect{16, 0, 4, 4}, 30);
    CHECK_EQ(doc.history().redo_depth, 0);
}

void test_opening_a_document_starts_a_new_timeline() {
    Document doc;
    doc.reset(make_pattern(Extent{16, 16}), "a.png");
    fill(doc, Rect{0, 0, 4, 4}, 40);
    CHECK(doc.can_undo());
    doc.reset(make_pattern(Extent{4, 4}), "b.png");
    CHECK(!doc.can_undo());
    CHECK(!doc.can_redo());
    CHECK_EQ(doc.history().bytes, 0u);
    CHECK(!doc.undo());
    CHECK(!doc.redo());
}

void test_a_change_outside_the_pixels_is_not_a_step() {
    Document doc;
    doc.reset(make_pattern(Extent{12, 12}), "pattern.png");
    const std::string before = doc.checksum();
    const std::uint64_t revision = doc.revision();
    doc.paint(Rect{40, 40, 8, 8},
              [](Image& image) { paint_rect(image, Rect{0, 0, 12, 12}, 0); });
    CHECK_EQ(doc.checksum(), before);
    CHECK_EQ(doc.revision(), revision);
    CHECK(!doc.can_undo());
}

void test_the_revision_moves_with_every_undoable_change() {
    Document doc;
    doc.reset(make_pattern(Extent{12, 12}), "pattern.png");
    const std::uint64_t opened = doc.revision();
    fill(doc, Rect{1, 1, 3, 3}, 90);
    CHECK_EQ(doc.revision(), opened + 1);
    CHECK(doc.undo());
    CHECK_EQ(doc.revision(), opened + 2);
    CHECK(doc.redo());
    CHECK_EQ(doc.revision(), opened + 3);
}

void test_the_budget_drops_whole_steps_oldest_first() {
    const std::size_t entry_bytes = 8u * 8u * 4u;
    History history(entry_bytes * 2u + entry_bytes / 2u);  // room for two, never three
    Image image = Image::filled(Extent{8, 8}, 0, 0, 0, 0);
    CHECK_EQ(history.stats().bytes, 0u);

    for (const std::uint8_t grey : {std::uint8_t{10}, std::uint8_t{20}, std::uint8_t{30}}) {
        history.record(Rect{0, 0, 8, 8}, std::vector<std::uint8_t>(entry_bytes, grey));
        paint_rect(image, Rect{0, 0, 8, 8}, grey);
    }
    CHECK_EQ(history.stats().undo_depth, 2);
    CHECK_EQ(history.stats().bytes, entry_bytes * 2u);

    // The steps that survive are the newest two: dropping the oldest is a shorter history,
    // dropping the newest would be a lost edit.
    CHECK(history.undo(image).has_value());
    CHECK(history.undo(image).has_value());
    CHECK(!history.undo(image).has_value());
    // The oldest step is gone, so undo stops at the before-image of the second one rather
    // than running all the way back to an empty image.
    CHECK_EQ(static_cast<int>(image.at(0, 0)[0]), 20);
}

}  // namespace

int main() {
    test_undo_after_many_mutations_restores_every_byte();
    test_undo_touches_only_the_rectangle_it_recorded();
    test_a_mutation_after_an_undo_forks_the_timeline();
    test_opening_a_document_starts_a_new_timeline();
    test_a_change_outside_the_pixels_is_not_a_step();
    test_the_revision_moves_with_every_undoable_change();
    test_the_budget_drops_whole_steps_oldest_first();
    return noise::test::summary("history");
}
