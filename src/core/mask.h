// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "core/geom.h"
#include "core/image.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace noise {

// What a mask operation does to the coverage it lands on. Every mask-writing command names one of
// these rather than inventing its own arithmetic, so "add to the selection" and "add to the mask"
// are the same operation.
enum class MaskOp {
    replace,   // the region's coverage becomes this operation's own
    add,       // the union
    subtract,  // what is left of the region's coverage
};

struct MaskOpInfo {
    MaskOp op;
    const char* name;
};

// The rows the commands, the panels and the errors all read: a mask operation is added here and
// nowhere else, and a request naming one the table does not have is refused by name.
inline constexpr MaskOpInfo kMaskOps[]{
    {MaskOp::replace, "replace"},
    {MaskOp::add, "add"},
    {MaskOp::subtract, "subtract"},
};

std::optional<MaskOp> mask_op_from_name(const std::string& name);
const char* mask_op_name(MaskOp op);
// "replace, add, subtract" - from the table, so an error cannot list a name that no longer exists.
std::string mask_op_names();

// What a coverage plane adds up to. `outline` follows the rule stated on `Mask::outline`; `bounds` is
// tight, which is what a region compiler needs from it.
struct MaskFacts {
    std::size_t covered = 0;
    Rect bounds{};
    std::vector<MaskRun> outline{};
};

// Coverage of one pixel: 0 is out, 255 is in, between is a partial claim on it. Straight rather
// than premultiplied - a mask holds no colour - which is what lets one plane serve as a selection,
// as a layer mask, and as anything carried between the two.
//
// A mask covers pixels of one document, and its size is part of its meaning: coverage outside it is
// 0, so a mask is never silently resized.
class Mask {
public:
    Mask() = default;
    explicit Mask(Extent size);

    // `plane` must hold exactly size.w * size.h bytes; a plane of another size is a loader bug and
    // is refused rather than cropped or padded.
    static std::optional<Mask> from_plane(Extent size, std::vector<std::uint8_t> plane);

    Extent size() const { return size_; }
    // False for a mask with no pixels, which is what a document holds before it has had one. Such a
    // mask covers nothing *and* restricts nothing; a mask that covers nothing but has a size
    // restricts everything, which is a real state (subtract the whole canvas from itself).
    bool sized() const { return size_.w > 0 && size_.h > 0; }

    std::uint8_t coverage(int x, int y) const;
    const std::vector<std::uint8_t>& plane() const { return plane_; }

    void fill(std::uint8_t coverage);
    // Clipped to the mask's own size: a region reaching past the canvas is not an error, it is a
    // rect the caller did not clip, and coverage outside the canvas cannot exist anyway.
    void op(Rect rect, MaskOp op);
    void invert();

    // What a plane amounts to, computed once per content and held by the mask that owns it: a
    // per-frame overlay and a wire report read the same values without either scanning again. Every
    // mutator drops the cache, so it cannot outlive the coverage it describes.
    const MaskFacts& facts() const;

    // Pixels with any coverage at all, and the tight box holding them (empty when there are none).
    std::size_t covered() const { return facts().covered; }
    Rect bounds() const { return facts().bounds; }
    // The boundary, as horizontal runs: the covered pixels that touch an uncovered one, out-of-bounds
    // counting as uncovered. One pixel wide and one rule, so a rectangle, a painted blob and a
    // selection that fills the canvas all get an outline without any of them being special-cased.
    const std::vector<MaskRun>& outline() const { return facts().outline; }

    bool operator==(const Mask& other) const;

private:
    void invalidate() const { facts_.reset(); }

    Extent size_{};
    std::vector<std::uint8_t> plane_;
    mutable std::optional<MaskFacts> facts_;
};

// Pulls what a write produced back towards what was there, in proportion to the mask: the result at
// a pixel is `coverage / 255` of the way from `before` to `after`.
//
// The mix is done on premultiplied values, which is not a detail: source-over is linear in the
// amount painted only once the colour is multiplied by its own alpha, so mixing straight-alpha
// channels would move a pixel's colour as well as its coverage (painting a low-alpha brush into a
// half-covered pixel would darken it instead of thinning it). Mixed this way the result is what
// "paint with the coverage folded into the amount" produces - measured over the whole byte cube, the
// alpha channel comes back identical and the colour channels within 2 LSB, the two units being the
// quantisation of the write the door is handed and of the unpremultiply, not slack. At coverage 0
// and 255 the bytes are exact either way, which is what a rectangular selection relies on.
//
// `at` is in document pixels and must lie inside both images: `before` holds the bytes found at
// `before_at`, which is where a caller's snapshot sits relative to the document. A mask with no
// pixels restricts nothing, so the whole call is skipped.
void restrict_to(const Mask& mask, Rect at, const Image& before, Rect before_at, Image& after);

}  // namespace noise
