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

// The ends of the coverage scale: no claim on the pixel, and the whole of it. Between is a partial
// claim, which is what an anti-aliased edge produces and what `restrict_to` paints with.
inline constexpr std::uint8_t kMaskOut = 0;
inline constexpr std::uint8_t kMaskIn = 255;

// What a coverage plane adds up to; `bounds` is tight, which is what a region compiler needs.
struct MaskFacts {
    std::size_t covered = 0;
    // How much of the canvas the mask claims, in pixels: the sum of every cell over 255. Equal to
    // `covered` exactly when nothing is partly covered, which is what makes the pair say whether a
    // shape has an anti-aliased edge.
    double area = 0.0;
    Rect bounds{};
    // Every boundary edge, each in exactly one run: grouped along its own length, so a horizontal
    // side appears as runs along x and a vertical side as runs along y, and no edge is in both.
    std::vector<MaskRun> boundary{};
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

    // Coverage over `area`, row-major, combined with what is there by the same rule `op(rect, op)`
    // uses on full coverage: `replace` makes a cell the patch's value, `add` takes the brighter of
    // the two, `subtract` takes the patch's claim away. This is the seam a rasterizer writes
    // through - a shape whose edge covers part of a pixel says how much, in the same units the
    // plane is already kept in. Cells of `area` outside the mask are dropped. A patch whose length
    // is not `area.w * area.h` writes nothing and returns false.
    bool op(Rect area, const std::vector<std::uint8_t>& patch, MaskOp op);
    void invert();

    // What a plane amounts to, computed once per content and held by the mask that owns it: a
    // per-frame overlay and a wire report read the same values without either scanning again. Every
    // mutator drops the cache, so it cannot outlive the coverage it describes.
    const MaskFacts& facts() const;

    // Pixels with any coverage at all, and the tight box holding them (empty when there are none).
    std::size_t covered() const { return facts().covered; }
    double area() const { return facts().area; }
    Rect bounds() const { return facts().bounds; }
    // The boundary: every edge between a covered pixel and an uncovered one - out-of-bounds counting
    // as uncovered - as runs facing that side. One rule, so a rectangle, a painted blob, a selection
    // filling the canvas and a selection with a hole in it all get an outline without any of them
    // being special-cased, and each edge is reported once: grouping boundary *pixels* instead would
    // report the pixels of a vertical edge once per row, which an overlay stroking along the row
    // paints as a solid line.
    const std::vector<MaskRun>& boundary() const { return facts().boundary; }

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

// `layer` seen through this mask, written into `out` at the same document coordinates, so a caller
// can blend `out` where it would have blended `layer`: every pixel keeps its colour and keeps
// `coverage / 255` of its amount, and a pixel the mask does not reach comes back as the absent
// pixel. `out` is resized to `layer` when it is not already, and only `at` is written.
//
// This is `restrict_to` with nothing underneath, so it inherits that rule and its error bounds:
// premultiplied, colour-preserving, within 1 LSB of alpha and 2 of colour of scaled arithmetic.
// Masking straight-alpha channels instead would turn a half-masked red pixel into a darker red one.
void scale_by_coverage(const Mask& mask, Rect at, const Image& layer, Image& out);

}  // namespace noise
