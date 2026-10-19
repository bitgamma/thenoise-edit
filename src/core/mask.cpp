// SPDX-License-Identifier: Apache-2.0
#include "core/mask.h"

#include <algorithm>
#include <cstring>
#include <optional>

namespace noise {
namespace {

// How far a value travels `coverage` of the way to its target. Rounding is away from zero and never
// overshoots, so a partial mask cannot move a pixel further than the write did.
int step_towards(int delta, std::uint8_t coverage) {
    const int magnitude = delta < 0 ? -delta : delta;
    const int moved = (magnitude * static_cast<int>(coverage) + 127) / 255;
    return delta < 0 ? -moved : moved;
}

// Every boundary edge of one side, as runs along that side's own axis. Out-of-bounds counts as
// uncovered, so a selection that fills the document still has a boundary: the edge the user sees is
// where coverage stops, wherever that is.
std::vector<MaskRun> runs_of_side(const std::vector<std::uint8_t>& plane, Extent size,
                                  MaskSide side, Rect window) {
    const bool horizontal = side == MaskSide::up || side == MaskSide::down;
    // The positions visited are covered pixels, so a window that holds all of them misses no edge:
    // the caller hands it the mask's bounds. Neighbours are still read outside it, through `covered`.
    const int across = horizontal ? size.h : size.w;
    const int along = horizontal ? size.w : size.h;
    // How far, and in which direction, a covered pixel's neighbour lies on the side being scanned.
    const int dx = side == MaskSide::left ? -1 : side == MaskSide::right ? 1 : 0;
    const int dy = side == MaskSide::up ? -1 : side == MaskSide::down ? 1 : 0;
    auto covered = [&](int x, int y) {
        if (x < 0 || y < 0 || x >= size.w || y >= size.h) {
            return false;
        }
        return plane[static_cast<std::size_t>(y) * static_cast<std::size_t>(size.w) +
                     static_cast<std::size_t>(x)] != kMaskOut;
    };
    std::vector<MaskRun> runs;
    int run_fixed = -1;
    int run_from = 0;
    int run_to = 0;
    auto flush = [&runs, &run_fixed, &run_from, &run_to, side] {
        if (run_fixed >= 0) {
            runs.push_back(MaskRun{side, run_fixed, run_from, run_to});
            run_fixed = -1;
        }
    };
    const int fixed_from = horizontal ? window.y : window.x;
    const int fixed_to = horizontal ? window.bottom() : window.right();
    const int cell_from = horizontal ? window.x : window.y;
    const int cell_to = horizontal ? window.right() : window.bottom();
    for (int fixed = fixed_from; fixed < fixed_to && fixed < across; ++fixed) {
        for (int cell = cell_from; cell < cell_to && cell < along; ++cell) {
            const int x = horizontal ? cell : fixed;
            const int y = horizontal ? fixed : cell;
            if (!covered(x, y) || covered(x + dx, y + dy)) {
                flush();
                continue;
            }
            // Consecutive edge cells on one line are contiguous: the scan only leaves the run when a
            // cell stops being an edge, and that flushes it.
            if (run_fixed == fixed) {
                run_to = cell + 1;
                continue;
            }
            flush();
            run_fixed = fixed;
            run_from = cell;
            run_to = cell + 1;
        }
        flush();
    }
    return runs;
}



}  // namespace

std::optional<MaskOp> mask_op_from_name(const std::string& name) {
    for (const MaskOpInfo& info : kMaskOps) {
        if (name == info.name) {
            return info.op;
        }
    }
    return std::nullopt;
}

const char* mask_op_name(MaskOp op) {
    for (const MaskOpInfo& info : kMaskOps) {
        if (info.op == op) {
            return info.name;
        }
    }
    // Unreachable for a value from the enum: an op nobody named has no name to print, and guessing
    // one would let a bad value travel as if it were valid.
    return "?";
}

std::string mask_op_names() {
    std::string names;
    for (const MaskOpInfo& info : kMaskOps) {
        if (!names.empty()) {
            names += ", ";
        }
        names += info.name;
    }
    return names;
}

Mask::Mask(Extent size)
    : size_(size),
      plane_(sized() ? static_cast<std::size_t>(size.w) * static_cast<std::size_t>(size.h) : 0u,
             kMaskOut) {}

std::optional<Mask> Mask::from_plane(Extent size, std::vector<std::uint8_t> plane) {
    const std::size_t want = static_cast<std::size_t>(size.w) * static_cast<std::size_t>(size.h);
    if (size.w <= 0 || size.h <= 0 || plane.size() != want) {
        return std::nullopt;
    }
    Mask mask;
    mask.size_ = size;
    mask.plane_ = std::move(plane);
    return mask;
}

std::uint8_t Mask::coverage(int x, int y) const {
    if (x < 0 || y < 0 || x >= size_.w || y >= size_.h) {
        return kMaskOut;
    }
    return plane_[static_cast<std::size_t>(y) * static_cast<std::size_t>(size_.w) +
                 static_cast<std::size_t>(x)];
}

Mask Mask::region(Rect area) const {
    Mask out;
    if (area.w <= 0 || area.h <= 0 || !area.inside(size_)) {
        return out;
    }
    out.size_ = Extent{area.w, area.h};
    out.plane_.resize(static_cast<std::size_t>(area.w) * static_cast<std::size_t>(area.h));
    for (int y = 0; y < area.h; ++y) {
        std::copy_n(row(area.y + y).begin() + static_cast<std::ptrdiff_t>(area.x),
                    static_cast<std::size_t>(area.w), out.row(y).begin());
    }
    return out;  // its own counts: a region of a plane is not the plane
}

std::span<std::uint8_t> Mask::row(int y) {
    touch_rows(y, y);
    return std::span<std::uint8_t>(
        plane_.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(size_.w),
        static_cast<std::size_t>(size_.w));
}

std::span<const std::uint8_t> Mask::row(int y) const {
    return std::span<const std::uint8_t>(
        plane_.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(size_.w),
        static_cast<std::size_t>(size_.w));
}

void Mask::fill(std::uint8_t coverage) {
    std::fill(plane_.begin(), plane_.end(), coverage);
    invalidate();
}

namespace {

// The one rule a mask operation applies to a pixel. `want` is what the operation claims for the
// cell in the same units the plane is kept in, so a rectangle that claims everything and the edge of
// an ellipse that claims a third of a pixel cannot drift apart.
std::uint8_t combined(std::uint8_t had, std::uint8_t want, MaskOp operation) {
    switch (operation) {
        case MaskOp::replace:
            return want;
        case MaskOp::add:
            return static_cast<std::uint8_t>(std::max<int>(had, want));
        case MaskOp::subtract:
            return static_cast<std::uint8_t>(had > want ? had - want : 0);
    }
    return had;
}

}  // namespace

void Mask::op(Rect rect, MaskOp operation) {
    const Rect area = intersect(rect, Rect{0, 0, size_.w, size_.h});
    if (area.empty()) {
        return;  // nothing written, so nothing derived from the plane went stale
    }
    touch_rows(area.y, area.bottom() - 1);
    for (int y = area.y; y < area.bottom(); ++y) {
        std::uint8_t* row =
            plane_.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(size_.w);
        for (int x = area.x; x < area.right(); ++x) {
            row[x] = combined(row[x], kMaskIn, operation);
        }
    }
}

bool Mask::op(Rect area, const std::vector<std::uint8_t>& patch, MaskOp operation) {
    if (area.w <= 0 || area.h <= 0) {
        return patch.empty();
    }
    const std::size_t cells = static_cast<std::size_t>(area.w) * static_cast<std::size_t>(area.h);
    if (patch.size() != cells) {
        return false;
    }
    const Rect written = intersect(area, Rect{0, 0, size_.w, size_.h});
    if (written.empty()) {
        return true;  // nothing written, so nothing derived from the plane went stale
    }
    touch_rows(written.y, written.bottom() - 1);
    for (int y = written.y; y < written.bottom(); ++y) {
        std::uint8_t* row =
            plane_.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(size_.w);
        const std::uint8_t* source =
            patch.data() + static_cast<std::size_t>(y - area.y) * static_cast<std::size_t>(area.w);
        for (int x = written.x; x < written.right(); ++x) {
            row[x] = combined(row[x], source[x - area.x], operation);
        }
    }
    return true;
}

bool Mask::paint(Rect area, const std::vector<std::uint8_t>& ink, MaskInk ink_mode) {
    if (area.w <= 0 || area.h <= 0) {
        return ink.empty();
    }
    const std::size_t cells = static_cast<std::size_t>(area.w) * static_cast<std::size_t>(area.h);
    if (ink.size() != cells) {
        return false;
    }
    const Rect written = intersect(area, Rect{0, 0, size_.w, size_.h});
    if (written.empty()) {
        return true;  // nothing written, so nothing derived from the plane went stale
    }
    touch_rows(written.y, written.bottom() - 1);
    const std::uint8_t target = ink_mode == MaskInk::reveal ? kMaskIn : kMaskOut;
    for (int y = written.y; y < written.bottom(); ++y) {
        std::uint8_t* row =
            plane_.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(size_.w);
        const std::uint8_t* source =
            ink.data() + static_cast<std::size_t>(y - area.y) * static_cast<std::size_t>(area.w);
        for (int x = written.x; x < written.right(); ++x) {
            const std::uint8_t had = row[x];
            row[x] = static_cast<std::uint8_t>(
                static_cast<int>(had) + step_towards(static_cast<int>(target) - static_cast<int>(had),
                                                     source[x - area.x]));
        }
    }
    return true;
}

void Mask::restrict_to(const Mask& selection, Rect at, const Mask& before, Rect before_at) {
    if (!selection.sized() || at.empty() || plane_.empty()) {
        return;
    }
    // The caller's guarantee, checked rather than trusted: outside the overlap there is no original
    // byte to fall back to.
    const Rect area = intersect(at, intersect(before_at, Rect{0, 0, size_.w, size_.h}));
    if (area.empty()) {
        return;
    }
    touch_rows(area.y, area.bottom() - 1);
    for (int y = area.y; y < area.bottom(); ++y) {
        std::uint8_t* row =
            plane_.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(size_.w);
        for (int x = area.x; x < area.right(); ++x) {
            const std::uint8_t coverage = selection.coverage(x, y);
            if (coverage == kMaskIn) {
                continue;
            }
            const std::uint8_t* below = before.plane_.data() +
                                        static_cast<std::size_t>(y - before_at.y) *
                                            static_cast<std::size_t>(before.size_.w) +
                                        static_cast<std::size_t>(x - before_at.x);
            row[x] = static_cast<std::uint8_t>(
                static_cast<int>(*below) +
                step_towards(static_cast<int>(row[x]) - static_cast<int>(*below), coverage));
        }
    }
}

void Mask::invert() {
    for (std::uint8_t& cell : plane_) {
        cell = static_cast<std::uint8_t>(kMaskIn - cell);
    }
    invalidate();
}

void Mask::invalidate() const {
    // Nothing is known about any row until the plane is scanned again, and the boundary is a scan of
    // its own.
    rows_.clear();
    row_dirty_.clear();
    boundary_.reset();
}

void Mask::touch_rows(int from, int to) const {
    boundary_.reset();
    if (rows_.size() != static_cast<std::size_t>(sized() ? size_.h : 0)) {
        invalidate();  // nothing computed yet, or the plane is not the size the rows describe
        return;
    }
    for (int y = std::max(from, 0); y <= std::min(to, size_.h - 1); ++y) {
        row_dirty_[static_cast<std::size_t>(y)] = true;
    }
}

void Mask::refresh() const {
    const std::size_t height = sized() ? static_cast<std::size_t>(size_.h) : 0u;
    if (rows_.size() != height) {
        rows_.assign(height, RowFacts{});
        row_dirty_.assign(height, true);
    }
    const std::size_t width = sized() ? static_cast<std::size_t>(size_.w) : 0u;
    for (std::size_t y = 0; y < height; ++y) {
        if (!row_dirty_[y]) {
            continue;
        }
        RowFacts& counts = rows_[y];
        counts = RowFacts{};
        const std::uint8_t* cells = plane_.data() + y * width;
        for (int x = 0; x < size_.w; ++x) {
            const std::uint8_t cell = cells[x];
            if (cell == kMaskOut) {
                continue;
            }
            if (counts.any == 0) {
                counts.from = x;
            }
            ++counts.any;
            counts.full += cell == kMaskIn ? 1u : 0u;
            counts.sum += cell;
            counts.to = x + 1;
        }
        row_dirty_[y] = false;
    }

    MaskFacts facts;
    int from_row = -1;
    int to_row = -1;
    int x_from = 0;
    int x_to = 0;
    for (std::size_t y = 0; y < height; ++y) {
        const RowFacts& counts = rows_[y];
        facts.covered += counts.any;
        facts.area += static_cast<double>(counts.sum);
        if (counts.any == 0) {
            continue;
        }
        if (from_row < 0) {
            from_row = static_cast<int>(y);
            x_from = counts.from;
            x_to = counts.to;
        } else {
            x_from = std::min(x_from, counts.from);
            x_to = std::max(x_to, counts.to);
        }
        to_row = static_cast<int>(y);
    }
    facts.area /= static_cast<double>(kMaskIn);
    facts.bounds = from_row < 0 ? Rect{}
                                : Rect{x_from, from_row, x_to - x_from, to_row - from_row + 1};
    facts_ = facts;
}

const MaskFacts& Mask::facts() const {
    refresh();
    return facts_;
}

bool Mask::covers_all(Rect region) const {
    const Rect area = intersect(region, Rect{0, 0, size_.w, size_.h});
    if (area.empty() || !sized()) {
        return true;  // nothing to cover, or a mask that makes no claim: either way, identity
    }
    refresh();
    for (int y = area.y; y < area.bottom(); ++y) {
        if (rows_[static_cast<std::size_t>(y)].full != static_cast<std::uint32_t>(size_.w)) {
            return false;
        }
    }
    return true;
}

bool Mask::covers_nothing(Rect region) const {
    const Rect area = intersect(region, Rect{0, 0, size_.w, size_.h});
    if (area.empty() || !sized()) {
        return false;
    }
    refresh();
    for (int y = area.y; y < area.bottom(); ++y) {
        if (rows_[static_cast<std::size_t>(y)].any != 0u) {
            return false;
        }
    }
    return true;
}

const std::vector<MaskRun>& Mask::boundary() const {
    // Its own cache, and its own scan: the boundary walks the plane for edges, which the counts above
    // deliberately do not. An overlay asks for this; a panel showing counts does not, and must not pay
    // for it.
    if (!boundary_) {
        std::vector<MaskRun> runs;
        refresh();  // the bounds say where there is anything to find
        if (!facts_.bounds.empty()) {
            for (const MaskSide side : kMaskSides) {
                const std::vector<MaskRun> side_runs =
                    runs_of_side(plane_, size_, side, facts_.bounds);
                runs.insert(runs.end(), side_runs.begin(), side_runs.end());
            }
        }
        boundary_ = std::move(runs);
    }
    return *boundary_;
}

bool Mask::operator==(const Mask& other) const {
    return size_ == other.size_ && plane_ == other.plane_;
}

namespace {

// `to` becomes `coverage / 255` of the way from `from` to `to`, in premultiplied space, which is
// where "some fraction of this write happened" is a linear statement about both colour and amount.
// Coverage is a fraction of 255 and premultiplied colour a fraction of 255 squared, so the largest
// value below is 65025 * 255 - inside int32, with the rounding error far under one LSB of the byte
// that comes out. What is left with no coverage is the absent pixel and that byte pattern only: a
// hue nobody can see is a second way to say "nothing here", and the composite would diff.
void mix_towards(const std::uint8_t* from, std::uint8_t* to, std::uint8_t coverage) {
    const int below_alpha = static_cast<int>(from[3]) * 255;
    const int written_alpha = static_cast<int>(to[3]) * 255;
    const int mixed_alpha = below_alpha + step_towards(written_alpha - below_alpha, coverage);
    if (mixed_alpha <= 0) {
        for (int channel = 0; channel < kChannels; ++channel) {
            to[channel] = 0;
        }
        return;
    }
    for (int channel = 0; channel < 3; ++channel) {
        const int below = static_cast<int>(from[channel]) * static_cast<int>(from[3]);
        const int written = static_cast<int>(to[channel]) * static_cast<int>(to[3]);
        const int mixed = below + step_towards(written - below, coverage);
        to[channel] = static_cast<std::uint8_t>((mixed * 255 + 127) / mixed_alpha);
    }
    to[3] = static_cast<std::uint8_t>((mixed_alpha + 127) / 255);
}

}  // namespace

void restrict_to(const Mask& mask, Rect at, const Image& before, Rect before_at, Image& after) {
    if (!mask.sized() || at.empty()) {
        return;
    }
    // The caller's guarantee, checked rather than trusted: writing outside either image would be a
    // heap write, and past `before_at` there would be no original byte to fall back to.
    const Rect area = intersect(at, intersect(before_at, Rect{0, 0, after.size().w, after.size().h}));
    for (int y = area.y; y < area.bottom(); ++y) {
        for (int x = area.x; x < area.right(); ++x) {
            const std::uint8_t coverage = mask.coverage(x, y);
            if (coverage == kMaskIn) {
                continue;
            }
            mix_towards(before.at(x - before_at.x, y - before_at.y), after.at(x, y), coverage);
        }
    }
}

void scale_by_coverage(const Mask& mask, Rect at, const Image& layer, Image& out) {
    if (out.size() != layer.size()) {
        out = Image::filled(layer.size(), 0, 0, 0, 0);
    }
    if (at.empty()) {
        return;
    }
    const Rect area = intersect(at, Rect{0, 0, layer.size().w, layer.size().h});
    const std::uint8_t absent[kChannels] = {0, 0, 0, 0};
    // A mask with no pixels makes no claim, which for a mask means the same as it does for a
    // selection: nothing is hidden. Leaving the layer's bytes alone is the identity, and the
    // alternative - a plane that covers nothing because it has no size - would blank the layer.
    const std::uint8_t whole = kMaskIn;
    for (int y = area.y; y < area.bottom(); ++y) {
        for (int x = area.x; x < area.right(); ++x) {
            std::uint8_t* to = out.at(x, y);
            std::memcpy(to, layer.at(x, y), kChannels);   // then scaled, in place
            mix_towards(absent, to, mask.sized() ? mask.coverage(x, y) : whole);
        }
    }
}

Mask coverage_of(const Image& picture) {
    const Extent size = picture.size();
    const std::size_t w = static_cast<std::size_t>(size.w);
    std::vector<std::uint8_t> plane(w * static_cast<std::size_t>(size.h));
    for (int y = 0; y < size.h; ++y) {
        for (int x = 0; x < size.w; ++x) {
            plane[static_cast<std::size_t>(y) * w + static_cast<std::size_t>(x)] =
                picture.at(x, y)[3];
        }
    }
    std::optional<Mask> coverage = Mask::from_plane(picture.size(), std::move(plane));
    // The plane was sized from the image it came from, so the only way this refuses is an image whose
    // own size and bytes disagree - a broken image, not a broken call.
    return coverage.has_value() ? *coverage : Mask{};
}

}  // namespace noise
