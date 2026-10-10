// SPDX-License-Identifier: Apache-2.0
#include "test.h"

#include "core/image.h"
#include "core/resample.h"

#include <cstdint>
#include <cstring>
#include <vector>

using namespace noise;

namespace {

// Colour, partial alpha and one absent pixel, all in a pattern with no period a wrong filter could
// land back on by accident.
Image content(Extent size) {
    Image image = Image::filled(size, 0, 0, 0, 0);
    for (int y = 0; y < size.h; ++y) {
        for (int x = 0; x < size.w; ++x) {
            std::uint8_t* p = image.at(x, y);
            p[0] = static_cast<std::uint8_t>((x * 5 + y * 3) & 0xff);
            p[1] = static_cast<std::uint8_t>(200 - ((x * y) % 180));
            p[2] = static_cast<std::uint8_t>((x ^ y) * 2 & 0xff);
            p[3] = static_cast<std::uint8_t>(80 + ((x + 2 * y) % 176));
        }
    }
    std::memcpy(image.at(size.w / 3, size.h / 3), kEmptyPixel, kChannels);
    return image;
}

// A step edge: the left half opaque white, the right half opaque black. Where the step lands after a
// resample is the whole geometry question, and ringing cannot hide a wrong answer.
Image step(int width, int height) {
    Image image = Image::filled(Extent{width, height}, 0, 0, 0, 255);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width / 2; ++x) {
            std::uint8_t* p = image.at(x, y);
            p[0] = p[1] = p[2] = 255;
        }
    }
    return image;
}

std::size_t differing_bytes(const Image& a, const Image& b) {
    if (a.size() != b.size()) {
        return a.bytes() + b.bytes();
    }
    std::size_t differing = 0;
    for (std::size_t i = 0; i < a.bytes(); ++i) {
        if (a.data()[i] != b.data()[i]) {
            ++differing;
        }
    }
    return differing;
}

void test_a_resample_to_the_size_it_has_is_a_copy() {
    const Extent size{37, 23};
    const Image source = content(size);
    CHECK_EQ(differing_bytes(source, resample_lanczos3(source, size)), std::size_t(0));

    // Larger and smaller, in both orders, on a size that is not a factor of anything.
    const Image up = resample_lanczos3(source, Extent{61, 41});
    CHECK(up.size() == Extent{61, 41});
    const Image back = resample_lanczos3(up, size);
    CHECK(back.size() == size);
    CHECK_EQ(differing_bytes(back, resample_lanczos3(back, size)), std::size_t(0));
}

void test_the_output_is_the_size_asked_for() {
    const Image source = content(Extent{20, 12});
    for (const Extent want : {Extent{1, 1}, Extent{20, 12}, Extent{19, 12}, Extent{21, 13},
                              Extent{7, 3}, Extent{512, 512}}) {
        CHECK(resample_lanczos3(source, want).size() == want);
    }
    // Nothing to resample is nothing, in any shape asked for.
    CHECK(resample_lanczos3(Image{}, Extent{4, 4}).empty());
    CHECK(resample_lanczos3(source, Extent{0, 4}).empty());
}

void test_a_reduction_averages_what_it_covers_instead_of_sampling_it() {
    // Detail at the source's own sampling limit, reduced four to one: four of those stripes go into
    // every output pixel, so the honest answer is their mean. A filter that kept its taps three
    // pixels wide would sample the stripes instead of averaging them and come back with a grey that
    // has nothing to do with the picture - the definition of aliasing, and the case the answer coming
    // back from the engine at a snapped size is full of.
    const Extent size{128, 8};
    Image stripes = Image::filled(size, 0, 0, 0, 255);
    for (int y = 0; y < size.h; ++y) {
        for (int x = 0; x < size.w; x += 2) {
            std::uint8_t* p = stripes.at(x, y);
            p[0] = p[1] = p[2] = 255;
        }
    }
    // 30 and not 32 on purpose: with an integer factor every output sample sits symmetrically about
    // the stripes, and a filter of any width then averages them to the right answer by symmetry. At a
    // non-integer factor the only thing that gives 128 is a window as wide as the pixel.
    const Image reduced = resample_lanczos3(stripes, Extent{30, 8});
    for (int y = 0; y < 8; ++y) {
        for (int x = 4; x < 26; ++x) {
            const int value = reduced.at(x, y)[0];
            CHECK(value > 122 && value < 134);
        }
    }
}

void test_a_flat_picture_stays_flat() {
    // The filter cannot move a picture that has nowhere to move: every weight is normalised, so a
    // constant stays that constant exactly - and an answer that came back at a snapped size depends
    // on flat areas of the frame still being flat.
    const Image opaque = Image::filled(Extent{9, 5}, 200, 40, 90, 255);
    for (const Extent want : {Extent{17, 11}, Extent{4, 3}, Extent{9, 5}}) {
        const Image out = resample_lanczos3(opaque, want);
        std::size_t off = 0;
        for (int y = 0; y < want.h; ++y) {
            for (int x = 0; x < want.w; ++x) {
                const std::uint8_t* p = out.at(x, y);
                if (p[0] != 200 || p[1] != 40 || p[2] != 90 || p[3] != 255) {
                    ++off;
                }
            }
        }
        CHECK_EQ(off, std::size_t(0));
    }

    // A translucent flat colour goes through premultiplication and back, so the ends of the round
    // trip are within the quantisation of the unpremultiply - one step, not a drift.
    const Image sheer = Image::filled(Extent{9, 5}, 255, 0, 0, 128);
    const Image out = resample_lanczos3(sheer, Extent{13, 7});
    for (int y = 0; y < 7; ++y) {
        for (int x = 0; x < 13; ++x) {
            const std::uint8_t* p = out.at(x, y);
            CHECK(p[0] >= 254);
            CHECK_EQ(p[1], 0);
            CHECK_EQ(p[2], 0);
            CHECK(p[3] >= 127 && p[3] <= 129);
        }
    }
}

void test_transparency_does_not_bleed_black_into_an_edge() {
    // One opaque pixel above an empty one, doubled: the pixel between them is half covered. Mixed
    // straight, the colour would come back at half strength - a dark fringe on every soft edge a
    // generated answer has. Mixed premultiplied, it is the same colour at half the amount.
    const Image pair = Image::from_rgba(Extent{1, 2}, {255, 0, 0, 255, 0, 0, 0, 0});
    const Image out = resample_lanczos3(pair, Extent{1, 4});
    // The two samples the transition falls over, nearer one end than the other: the amount changes
    // across them and the hue does not change at all.
    for (const int y : {1, 2}) {
        const std::uint8_t* between = out.at(0, y);
        CHECK(between[0] >= 254);
        CHECK_EQ(between[1], 0);
        CHECK_EQ(between[2], 0);
        CHECK(between[3] > 0 && between[3] < 255);
    }
    CHECK(out.at(0, 1)[3] > out.at(0, 2)[3]);
}

void test_the_edge_lands_where_the_geometry_says() {
    // Halved: the step was at half of 16, so it is still at half of 8 - columns 0 and 1 lit, 2 and
    // 3 not. A resample that ignored the field of view would put it elsewhere.
    const Image half = resample_lanczos3(step(16, 4), Extent{8, 4});
    for (int y = 0; y < 4; ++y) {
        for (int x = 0; x < 4; ++x) {
            CHECK(half.at(x, y)[0] > 200);
            CHECK(half.at(7 - x, y)[0] < 60);
        }
    }

    // Doubled: the same step, in the middle of 32 - and no grey further from the middle than one
    // tap of the filter.
    const Image twice = resample_lanczos3(step(16, 2), Extent{32, 2});
    for (int x = 0; x < 12; ++x) {
        CHECK(twice.at(x, 0)[0] > 250);
        CHECK(twice.at(31 - x, 0)[0] < 5);
    }
    // The boundary itself sits at the middle of the doubled image, one tap either side of it.
    CHECK(twice.at(15, 0)[0] > 128);
    CHECK(twice.at(17, 0)[0] < 127);
}

void test_a_pixel_left_with_no_alpha_has_no_colour_either() {
    // The far tail of an edge, where the filter brings back a rounding-level amount of alpha: the
    // colour it carries has nothing to be divided by, and a division there paints with noise. The
    // rule is the one the rest of the app keeps - "nothing here" has exactly one byte pattern.
    // A low alpha rather than a full one: the tail of the filter then leaves pixels with a trace of
    // premultiplied colour and an amount that rounds away, which is exactly the division that must
    // not happen.
    std::vector<std::uint8_t> bytes{255, 128, 40, 8};
    for (int i = 0; i < 7; ++i) {
        bytes.insert(bytes.end(), {0, 0, 0, 0});
    }
    const Image row = Image::from_rgba(Extent{1, 8}, bytes);
    const Image out = resample_lanczos3(row, Extent{1, 64});

    std::size_t absent = 0;
    std::size_t ghosts = 0;
    for (int y = 0; y < 64; ++y) {
        const std::uint8_t* p = out.at(0, y);
        if (p[3] != 0) {
            continue;
        }
        ++absent;
        if (p[0] != 0 || p[1] != 0 || p[2] != 0) {
            ++ghosts;
        }
    }
    CHECK(absent > 0);  // the tail is really there, or the check proved nothing
    CHECK_EQ(ghosts, std::size_t(0));
}

void test_colour_under_no_alpha_is_not_carried() {
    // Premultiplication discards a colour that cannot be seen, so the illegal pixel - some colour
    // under a zero alpha - comes back as the one pattern "nothing here" has. Pinned because the
    // promise is only useful if the behaviour is the documented one.
    const Image illegal = Image::from_rgba(Extent{1, 1}, {255, 0, 0, 0});
    const Image out = resample_lanczos3(illegal, Extent{1, 1});
    CHECK_EQ(std::memcmp(out.at(0, 0), kEmptyPixel, kChannels), 0);
}

}  // namespace

int main() {
    test_a_resample_to_the_size_it_has_is_a_copy();
    test_the_output_is_the_size_asked_for();
    test_a_flat_picture_stays_flat();
    test_transparency_does_not_bleed_black_into_an_edge();
    test_the_edge_lands_where_the_geometry_says();
    test_a_reduction_averages_what_it_covers_instead_of_sampling_it();
    test_a_pixel_left_with_no_alpha_has_no_colour_either();
    test_colour_under_no_alpha_is_not_carried();
    return noise::test::summary(__FILE__);
}
