// SPDX-License-Identifier: Apache-2.0
#include "core/mask.h"

#include <algorithm>
#include <optional>

namespace noise {
namespace {

constexpr std::uint8_t kOut = 0;
constexpr std::uint8_t kIn = 255;

// How far a value travels `coverage` of the way to its target. Rounding is away from zero and never
// overshoots, so a partial mask cannot move a pixel further than the write did.
int step_towards(int delta, std::uint8_t coverage) {
    const int magnitude = delta < 0 ? -delta : delta;
    const int moved = (magnitude * static_cast<int>(coverage) + 127) / 255;
    return delta < 0 ? -moved : moved;
}

Rect bounds_of(const std::vector<std::uint8_t>& plane, Extent size) {
    int x0 = size.w;
    int y0 = size.h;
    int x1 = 0;
    int y1 = 0;
    for (std::size_t index = 0; index < plane.size(); ++index) {
        if (plane[index] == kOut) {
            continue;
        }
        const int x = static_cast<int>(index % static_cast<std::size_t>(size.w));
        const int y = static_cast<int>(index / static_cast<std::size_t>(size.w));
        x0 = std::min(x0, x);
        y0 = std::min(y0, y);
        x1 = std::max(x1, x + 1);
        y1 = std::max(y1, y + 1);
    }
    if (x1 <= x0 || y1 <= y0) {
        return Rect{};
    }
    return Rect{x0, y0, x1 - x0, y1 - y0};
}

// The covered pixels that touch an uncovered one, as runs along each row. Out-of-bounds counts as
// uncovered, so a selection that fills the document still has an outline: the boundary the user sees
// is where coverage stops, wherever that is.
std::vector<MaskRun> outline_of(const std::vector<std::uint8_t>& plane, Extent size) {
    auto covered = [&](int x, int y) {
        if (x < 0 || y < 0 || x >= size.w || y >= size.h) {
            return false;
        }
        return plane[static_cast<std::size_t>(y) * static_cast<std::size_t>(size.w) +
                     static_cast<std::size_t>(x)] != kOut;
    };
    std::vector<MaskRun> runs;
    int run_y = -1;
    int run_x0 = 0;
    int run_x1 = 0;
    auto flush = [&runs, &run_y, &run_x0, &run_x1] {
        if (run_y >= 0) {
            runs.push_back(MaskRun{run_y, run_x0, run_x1});
            run_y = -1;
        }
    };
    for (int y = 0; y < size.h; ++y) {
        for (int x = 0; x < size.w; ++x) {
            if (!covered(x, y)) {
                flush();
                continue;
            }
            const bool touches_out = !covered(x - 1, y) || !covered(x + 1, y) ||
                                     !covered(x, y - 1) || !covered(x, y + 1);
            if (!touches_out) {
                flush();
                continue;
            }
            if (run_y == y && run_x1 == x) {
                run_x1 = x + 1;
                continue;
            }
            flush();
            run_y = y;
            run_x0 = x;
            run_x1 = x + 1;
        }
        flush();
    }
    return runs;
}

MaskFacts facts_of(const std::vector<std::uint8_t>& plane, Extent size) {
    MaskFacts facts;
    for (const std::uint8_t cell : plane) {
        facts.covered += cell != kOut ? 1u : 0u;
    }
    facts.bounds = bounds_of(plane, size);
    facts.outline = outline_of(plane, size);
    return facts;
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
             kOut) {}

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
        return kOut;
    }
    return plane_[static_cast<std::size_t>(y) * static_cast<std::size_t>(size_.w) +
                 static_cast<std::size_t>(x)];
}

void Mask::fill(std::uint8_t coverage) {
    std::fill(plane_.begin(), plane_.end(), coverage);
    invalidate();
}

void Mask::op(Rect rect, MaskOp operation) {
    const Rect area = intersect(rect, Rect{0, 0, size_.w, size_.h});
    if (area.empty()) {
        return;  // nothing written, so nothing derived from the plane went stale
    }
    invalidate();
    for (int y = area.y; y < area.bottom(); ++y) {
        std::uint8_t* row =
            plane_.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(size_.w);
        for (int x = area.x; x < area.right(); ++x) {
            std::uint8_t& cell = row[x];
            switch (operation) {
                case MaskOp::replace:
                    cell = kIn;
                    break;
                case MaskOp::add:
                    cell = static_cast<std::uint8_t>(std::max<int>(cell, kIn));
                    break;
                case MaskOp::subtract:
                    cell = kOut;
                    break;
            }
        }
    }
}

void Mask::invert() {
    for (std::uint8_t& cell : plane_) {
        cell = static_cast<std::uint8_t>(kIn - cell);
    }
    invalidate();
}

const MaskFacts& Mask::facts() const {
    if (!facts_) {
        facts_ = facts_of(plane_, size_);
    }
    return *facts_;
}

bool Mask::operator==(const Mask& other) const {
    return size_ == other.size_ && plane_ == other.plane_;
}

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
            if (coverage == kIn) {
                continue;
            }
            const std::uint8_t* from = before.at(x - before_at.x, y - before_at.y);
            std::uint8_t* to = after.at(x, y);
            // Coverage is a fraction of 255 and premultiplied colour is a fraction of 255 squared,
            // so the largest value below is 65025 * 255 - inside int32, with the rounding error far
            // under one LSB of the byte that comes out.
            const int below_alpha = static_cast<int>(from[3]) * 255;
            const int written_alpha = static_cast<int>(to[3]) * 255;
            const int mixed_alpha = below_alpha + step_towards(written_alpha - below_alpha, coverage);
            // What is left with no coverage is the absent pixel, and that byte pattern only: a hue
            // nobody can see is a second way to say "nothing here", and the composite would diff.
            if (mixed_alpha <= 0) {
                for (int channel = 0; channel < kChannels; ++channel) {
                    to[channel] = 0;
                }
                continue;
            }
            for (int channel = 0; channel < 3; ++channel) {
                const int below = static_cast<int>(from[channel]) * static_cast<int>(from[3]);
                const int written = static_cast<int>(to[channel]) * static_cast<int>(to[3]);
                const int mixed = below + step_towards(written - below, coverage);
                to[channel] = static_cast<std::uint8_t>((mixed * 255 + 127) / mixed_alpha);
            }
            to[3] = static_cast<std::uint8_t>((mixed_alpha + 127) / 255);
        }
    }
}

}  // namespace noise
