// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "core/geom.h"

#include <vector>

namespace noise::app {

// Marching ants: the dashes that walk round a selection's boundary. Everything here is in one unit -
// screen pixels measured along the boundary - and is free of ImGui and of the document's pixels, so
// the pattern is a value a test can hold rather than something only a screenshot can show.

// One light or dark band is this many screen pixels; a light-dark pair is twice that, so the pattern
// repeats every 6 px. Measured on the screen, never in the document: the ants have to stay legible
// when the user zooms, and a dash that scales with the image turns into a wall of ink at 8x.
inline constexpr float kAntDashPx = 3.0f;
// One and a third cycles a second. The first value here was 32 px/s, four cycles, which reads as a
// blink rather than a walk.
inline constexpr float kAntMarchPxPerSecond = 8.0f;

// The overlay's state. `phases` is a request: draw the pattern frozen at these offsets instead of
// animating, one band per phase, so a probe can photograph what time hides. `time` is the clock
// reading the frame drew at, published so the phase a capture shows is a value a report can name.
struct AntOverlay {
    std::vector<float> phases{};
    double time = 0.0;
};

// Where the ants are at a clock reading, wrapped into one period so the number stays small however
// long the session runs - accumulated in float it would drift.
float ant_phase(double time);

// The phase a frame uses: the frozen one a debug client asked for, otherwise the clock's. The overlay
// draws with this and `app.state` reports it, which is what lets a screenshot be reproduced.
float ant_drawn_phase(const AntOverlay& ants);

// The side of a boundary run the line is drawn on, in document coordinates: the shared edge between
// the covered pixel and the uncovered one, so the ants fall between them rather than through the
// middle. At the edge of the document there is no uncovered pixel and the line would sit on the
// canvas's own border where it cannot be read, so it moves one pixel inside.
double ant_line(MaskSide side, int fixed, int across);

// How far round the selection a boundary point sits, in document units: clockwise from the top-left
// corner of its bounding box - right along the top, down the right side, left along the bottom, up
// the left side. Every run reads its phase from here, so the pattern goes round the selection as one
// line rather than four sides each sliding along their own axis at a rate that happens to disagree
// with the next.
//
// A run asks for its own side's formula, evaluated along its own axis, which keeps the distance
// linear: |ds/daxis| is 1 everywhere, so a dash is the same length on every side at every zoom.
// Picking the formula by which box side a point lies nearest does not do - the first pixel of the top
// edge is equally near the left side, and the two formulas differ by the height of the box, which
// turns the pattern into dashes of two, three, four and five pixels.
//
// This measures the box, not the contour: a rectangle is exact, and an irregular outline is
// continuous along each stretch and steps where the outline changes which way it faces. Tracing the
// contour would mean following holes and rejoining them where a hole meets the outline, to fix a step
// no wider than one dash on shapes nobody selects.
struct AntRing {
    Rect box{};

    double along(MaskSide side, double axis) const;
};

// One dash: the half-open stretch [from, to) in screen pixels along the run's axis.
struct AntDash {
    float from = 0.0f;
    float to = 0.0f;
    bool light = false;
};

// The dashes of one straight stretch of boundary. `axis_from`/`axis_to` are its ends along its own
// axis in screen pixels (`axis_to` > `axis_from`), `s_from`/`s_to` the distances round the boundary of
// those same two ends in the same unit - their difference over the span is the direction this stretch
// travels, and it is what makes the pattern continue into the stretches that meet at a corner.
//
// The dashes tile the stretch: consecutive, no gaps, colours alternating, each one at most a dash
// plus a pixel long. Boundaries are solved for - the points whose distance round the boundary,
// advanced by the phase, is a multiple of the dash length - rather than stepped towards, so they
// cannot drift along a run or fall at different places on two stretches that meet.
std::vector<AntDash> ant_dashes(float axis_from, float axis_to, float s_from, float s_to,
                                float phase);

}  // namespace noise::app
