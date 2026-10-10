// SPDX-License-Identifier: Apache-2.0
#include "test.h"

#include "app/ants.h"

#include <cmath>
#include <cstdio>
#include <vector>

using namespace noise;
using noise::app::AntDash;
using noise::app::AntOverlay;
using noise::app::AntRing;

namespace {

bool check(bool ok, const char* expression, const char* file, int line, const std::string& detail) {
    noise::test::expect(ok, expression, file, line, detail);
    return ok;
}

constexpr float kDash = noise::app::kAntDashPx;
constexpr float kPeriod = 2.0f * noise::app::kAntDashPx;

// Enough of the dashes to see what went wrong without filling a terminal with a thousand of them.
std::string text(const std::vector<AntDash>& dashes) {
    std::string out;
    for (const AntDash& dash : dashes) {
        if (out.size() > 120) {
            out += "...";
            break;
        }
        out += dash.light ? "L[" : "D[";
        out += std::to_string(dash.from).substr(0, 6);
        out += ",";
        out += std::to_string(dash.to).substr(0, 6);
        out += "] ";
    }
    return out;
}

// The phase is the clock times the speed, wrapped into one period, and it never goes backwards
// inside a period - which is the difference between a walk and a flicker.
void test_the_phase_is_the_clock_times_the_speed_wrapped_into_one_period() {
    for (double time : {0.0, 0.1, 0.25, 0.5, 0.7499, 0.75, 1.0, 3.0, 60.0, 100000.0}) {
        const float phase = noise::app::ant_phase(time);
        const double want = std::fmod(time * noise::app::kAntMarchPxPerSecond, kPeriod);
        if (check(phase >= 0.0f && phase < kPeriod, "phase in range", __FILE__, __LINE__,
                  std::to_string(phase))) {
            check(std::fabs(phase - want) < 0.001, "phase == time * speed (mod period)", __FILE__,
                  __LINE__, "t=" + std::to_string(time) + " got " + std::to_string(phase) +
                                " want " + std::to_string(want));
        }
    }
    // Advancing the clock advances the phase by the same amount, until it wraps.
    float previous = noise::app::ant_phase(0.0);
    for (double time = 0.001; time < 0.75; time += 0.001) {
        const float phase = noise::app::ant_phase(time);
        CHECK(phase >= previous);
        previous = phase;
    }
    // A frame of frozen phases draws the first of them, which is what makes a screenshot mean
    // something: the report and the overlay have to agree on what a capture shows.
    AntOverlay ants;
    ants.time = 0.4;
    CHECK_EQ(noise::app::ant_drawn_phase(ants), noise::app::ant_phase(0.4));
    ants.phases = {1.5f, 4.5f};
    CHECK_EQ(noise::app::ant_drawn_phase(ants), 1.5f);
}

// The pattern the user sees is a line of dashes, so the pieces have to tile the run: no gaps, no
// overlaps, colours alternating, each piece about a dash long. A mutation that steps the dashes in the
// wrong direction along the axis, or that decides colour per run instead of per position, breaks one
// of these and nothing else.
void test_the_dashes_tile_the_run_with_alternating_colour() {
    for (const float slope : {1.0f, -1.0f}) {
        for (const float phase : {0.0f, 1.0f, 2.0f, 3.0f}) {
            for (const float length : {1.0f, 2.5f, 40.0f, 147.0f, 1000.0f}) {
                const float axis_from = 20.0f;
                const auto dashes = noise::app::ant_dashes(
                    axis_from, axis_from + length, 0.0f, slope * length, phase);
                float cursor = axis_from;
                bool previous_light = false;
                bool first = true;
                for (const AntDash& dash : dashes) {
                    check(std::fabs(dash.from - cursor) < 0.001f, "dashes are contiguous", __FILE__,
                          __LINE__, text(dashes));
                    check(dash.to > dash.from, "dash has length", __FILE__, __LINE__, text(dashes));
                    check(dash.to <= axis_from + length + 0.001f, "dash stays inside the run",
                          __FILE__, __LINE__, text(dashes));
                    // Which colour comes first depends on which way round the ring this side runs;
                    // that neighbouring dashes differ is the invariant.
                    check(first || dash.light != previous_light, "colours alternate", __FILE__,
                          __LINE__, text(dashes));
                    previous_light = dash.light;
                    first = false;
                    cursor = dash.to;
                }
                // A run longer than a period is fully covered; a short one may be a single partial
                // dash, which is the whole of what there is to draw.
                if (length > kPeriod) {
                    check(std::fabs(cursor - (axis_from + length)) < 0.001f, "the run is fully drawn",
                          __FILE__, __LINE__, text(dashes));
                }
            }
        }
    }
}

// A dash is a screen measure: over a long stretch, half the pixels are light and every piece is at
// most a dash plus the pixel a rounded end can add. Doubling the dash, or deriving it from the
// document instead of the screen, fails this - and at 8x zoom that is the difference between ants and
// a bar of ink.
void test_the_dash_length_belongs_to_the_screen() {
    const float length = 1200.0f;
    const auto dashes = noise::app::ant_dashes(0.0f, length, 0.0f, length, 0.0f);
    float light = 0.0f;
    for (const AntDash& dash : dashes) {
        const float size = dash.to - dash.from;
        check(size <= kDash + 1.0f, "no dash longer than a dash plus a rounded pixel", __FILE__,
              __LINE__, text(dashes));
        light += dash.light ? size : 0.0f;
    }
    const float fraction = light / length;
    check(fraction > 0.45f && fraction < 0.55f, "half the boundary is light", __FILE__, __LINE__,
          std::to_string(fraction));
}

// Advancing the phase by one pixel moves every dash boundary by one pixel. If they moved by
// different amounts - or some not at all - the pattern shimmers in place, which is exactly the defect
// this overlay used to have.
void test_the_pattern_translates_with_the_phase() {
    const float axis_from = 0.0f;
    const float axis_to = 500.0f;
    const auto at = [&](float phase) {
        std::vector<float> cuts;
        for (const AntDash& dash :
             noise::app::ant_dashes(axis_from, axis_to, 0.0f, axis_to - axis_from, phase)) {
            cuts.push_back(dash.to);
        }
        return cuts;
    };
    const std::vector<float> base = at(0.0f);
    for (const float step : {1.0f, 2.0f}) {
        const std::vector<float> moved = at(step);
        check(moved.size() >= base.size() - 2 && moved.size() <= base.size() + 2,
              "the same number of dashes", __FILE__, __LINE__,
              std::to_string(base.size()) + " vs " + std::to_string(moved.size()));
        for (std::size_t i = 0; i + 1 < base.size() && i + 1 < moved.size(); ++i) {
            const float want = base[i] - step;
            check(std::fabs(moved[i] - want) <= 1.0f, "every boundary moved by the phase", __FILE__,
                  __LINE__, "index " + std::to_string(i) + " got " + std::to_string(moved[i]) +
                                " want " + std::to_string(want));
        }
    }
}

// The ring is what makes four sides read as one line: it increases by exactly the run's length as the
// run goes, and where two sides meet at a corner the numbers agree. A ring that picked its formula by
// which box side a point lay nearest gave the top edge a slope of 1.67 - dashes of two, three, four
// and five pixels - and this is the test that says so.
void test_the_ring_walks_the_box_as_one_loop() {
    const AntRing ring{Rect{30, 20, 60, 40}};
    struct Side {
        MaskSide side;
        double from;
        double to;
    };
    const std::vector<Side> sides{
        {MaskSide::up, 30.0, 90.0},     {MaskSide::right, 20.0, 60.0},
        {MaskSide::down, 90.0, 30.0},   {MaskSide::left, 60.0, 20.0},
    };
    double previous_end = ring.along(MaskSide::up, 30.0);
    for (const Side& side : sides) {
        const double start = ring.along(side.side, side.from);
        const double end = ring.along(side.side, side.to);
        // Continuous with the previous side's end: the corner is one point with one distance.
        check(std::fabs(start - previous_end) < 1e-9, "sides meet at the corners", __FILE__, __LINE__,
              std::to_string(start) + " vs " + std::to_string(previous_end));
        // Linear with slope exactly 1 in document units, so a dash is the same length on every side.
        check(std::fabs(std::fabs(end - start) - std::fabs(side.to - side.from)) < 1e-9,
              "one unit of ring per unit of run", __FILE__, __LINE__,
              std::to_string(end - start) + " vs " + std::to_string(side.to - side.from));
        previous_end = end;
    }
    // The loop closes: back at the starting corner the distance is the perimeter.
    const double perimeter = 2.0 * (60.0 + 40.0);
    check(std::fabs(previous_end - perimeter) < 1e-9, "the loop is the perimeter", __FILE__, __LINE__,
          std::to_string(previous_end));
}

// Two runs that meet at a corner have to come out of `ant_dashes` as one pattern: the last boundary of
// one and the first of the next sit a dash apart, not a whole side out of step.
void test_two_sides_meeting_at_a_corner_continue_each_other() {
    const AntRing ring{Rect{0, 0, 40, 20}};
    const float scale = 3.0f;
    struct Run {
        MaskSide side;
        double from;
        double to;
    };
    const std::vector<Run> runs{{MaskSide::up, 0.0, 40.0}, {MaskSide::right, 0.0, 20.0},
                                {MaskSide::down, 40.0, 0.0}, {MaskSide::left, 20.0, 0.0}};
    std::vector<float> boundaries;
    for (const Run& run : runs) {
        const float s_from = static_cast<float>(ring.along(run.side, run.from)) * scale;
        const float s_to = static_cast<float>(ring.along(run.side, run.to)) * scale;
        const auto dashes = noise::app::ant_dashes(0.0f, static_cast<float>(std::fabs(run.to - run.from)) * scale,
                                                   s_from, s_to, 0.0f);
        check(!dashes.empty(), "every side gets dashes", __FILE__, __LINE__,
              std::string{run.side == MaskSide::up    ? "up"
                          : run.side == MaskSide::right ? "right"
                          : run.side == MaskSide::down  ? "down"
                                                        : "left"} +
                  " " + text(dashes));
        for (const AntDash& dash : dashes) {
            boundaries.push_back(dash.to);
        }
    }
    // Across the whole outline the boundaries are one dash apart, give or take the rounding of a
    // device pixel - never a gap the size of a side.
    for (std::size_t i = 1; i < boundaries.size(); ++i) {
        const float gap = boundaries[i] - boundaries[i - 1];
        if (gap <= 0.0f) {
            continue;  // a run ending mid-dash repeats the boundary of the previous side
        }
        check(gap <= kDash + 1.0f, "no gap in the outline", __FILE__, __LINE__,
              "gap " + std::to_string(gap) + " at " + std::to_string(i));
    }
}

// At the edge of the document there is no uncovered pixel to sit between, and the line would fall on
// the canvas's own border, where it cannot be read. Inside by one instead.
void test_a_boundary_at_the_document_edge_is_drawn_inside() {
    CHECK_EQ(noise::app::ant_line(MaskSide::up, 0, 100), 1.0);
    CHECK_EQ(noise::app::ant_line(MaskSide::left, 0, 100), 1.0);
    CHECK_EQ(noise::app::ant_line(MaskSide::down, 99, 100), 99.0);
    CHECK_EQ(noise::app::ant_line(MaskSide::right, 99, 100), 99.0);
    // Away from the edge, between the covered pixel and the uncovered one.
    CHECK_EQ(noise::app::ant_line(MaskSide::up, 20, 100), 20.0);
    CHECK_EQ(noise::app::ant_line(MaskSide::down, 20, 100), 21.0);
    CHECK_EQ(noise::app::ant_line(MaskSide::left, 20, 100), 20.0);
    CHECK_EQ(noise::app::ant_line(MaskSide::right, 20, 100), 21.0);
}

}  // namespace

int main() {
    test_the_phase_is_the_clock_times_the_speed_wrapped_into_one_period();
    test_the_dashes_tile_the_run_with_alternating_colour();
    test_the_dash_length_belongs_to_the_screen();
    test_the_pattern_translates_with_the_phase();
    test_the_ring_walks_the_box_as_one_loop();
    test_two_sides_meeting_at_a_corner_continue_each_other();
    test_a_boundary_at_the_document_edge_is_drawn_inside();
    return noise::test::summary("ants");
}
