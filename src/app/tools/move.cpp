// SPDX-License-Identifier: Apache-2.0
#include "app/tools/move.h"

#include "core/log.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace noise::app {

namespace {

// The smallest rectangle holding every pixel with coverage. A layer is document-sized whether or not
// it is filled, and without this a drag would call a whole canvas dirty - and so hold a whole canvas
// aside for undo - every time the cursor moved one pixel.
Rect content_bounds(const Image& image) {
    const Extent size = image.size();
    int left = size.w;
    int top = size.h;
    int right = 0;
    int bottom = 0;
    for (int y = 0; y < size.h; ++y) {
        for (int x = 0; x < size.w; ++x) {
            if (image.at(x, y)[3] != 0) {
                left = std::min(left, x);
                top = std::min(top, y);
                // Maxima, not last-seen: a scan that ends on a narrow row would otherwise report a
                // box too small to cover the widest row above it, and the pixels outside it would
                // never be repainted.
                right = std::max(right, x + 1);
                bottom = std::max(bottom, y + 1);
            }
        }
    }
    return Rect{left, top, right - left, bottom - top};
}

Rect shift(Rect rect, int dx, int dy) {
    return Rect{rect.x + dx, rect.y + dy, rect.w, rect.h};
}

// The snapshot moved by (dx, dy), written across `region`: a pixel the shift leaves outside the
// snapshot is transparent, because a vacated part of a layer holds nothing rather than what happened
// to be there before the gesture.
void paint_shifted(Image& onto, const Image& from, Rect region, int dx, int dy) {
    const Extent size = from.size();
    for (int y = region.y; y < region.bottom(); ++y) {
        const int source_y = y - dy;
        const int first = std::max(region.x, dx);
        const int last = std::min(region.right(), size.w + dx);
        std::uint8_t* row = onto.at(0, y);
        if (source_y < 0 || source_y >= size.h) {
            std::memset(row + std::size_t(region.x) * kChannels, 0,
                        std::size_t(region.w) * kChannels);
            continue;
        }
        if (first > region.x) {
            std::memset(row + std::size_t(region.x) * kChannels, 0,
                        std::size_t(first - region.x) * kChannels);
        }
        if (last > first) {
            std::memcpy(row + std::size_t(first) * kChannels,
                        from.at(first - dx, source_y), std::size_t(last - first) * kChannels);
        }
        if (last < region.right()) {
            std::memset(row + std::size_t(last) * kChannels, 0,
                        std::size_t(region.right() - last) * kChannels);
        }
    }
}

}  // namespace

void move_press(AppState& state, Vec2 document_point) {
    if (state.doc.empty() || state.move.active) {
        return;
    }
    state.move = PointerMove{};
    state.move.active = true;
    state.move.origin = document_point;
    state.move.snapshot = state.doc.layer(std::size_t(state.doc.active())).pixels;
    state.move.content = content_bounds(state.move.snapshot);
    if (state.move.content.empty()) {
        log::info("move: the active layer holds nothing to move");
    }
}

void move_drag(AppState& state, Vec2 document_point) {
    if (!state.move.active || state.move.content.empty()) {
        return;
    }
    const int dx = static_cast<int>(std::lround(document_point.x - state.move.origin.x));
    const int dy = static_cast<int>(std::lround(document_point.y - state.move.origin.y));
    if (dx == state.move.dx && dy == state.move.dy) {
        // The cursor moved, the layer did not: a sub-pixel twitch must not repaint, must not
        // dirty the document, and must not grow the undo step.
        return;
    }
    const Rect canvas{0, 0, state.doc.width(), state.doc.height()};
    const Rect dirty = intersect(shift(state.move.content, state.move.dx, state.move.dy), canvas);
    const Rect next = intersect(shift(state.move.content, dx, dy), canvas);
    const Rect area = unite(dirty, next);
    if (area.empty()) {
        // Off the edge both ways: nothing on the canvas changes, and a move that has left the
        // document entirely is not something the pointer can bring back - it is undo that does.
        state.move.dx = dx;
        state.move.dy = dy;
        return;
    }
    state.doc.begin_provisional(area, LayerPlane::pixels);
    state.move.dx = dx;
    state.move.dy = dy;
    state.move.steps += 1;
    state.doc.repaint_provisional(area, [&state](Image& image, Rect region) {
        paint_shifted(image, state.move.snapshot, region, state.move.dx, state.move.dy);
    });
}

void move_release(AppState& state) {
    if (!state.move.active) {
        return;
    }
    state.doc.commit_provisional();
    state.move = PointerMove{};
}

}  // namespace noise::app
