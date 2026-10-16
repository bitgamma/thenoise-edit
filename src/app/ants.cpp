// SPDX-License-Identifier: Apache-2.0
#include "app/ants.h"

#include <algorithm>
#include <cmath>

namespace noise::app {
namespace {

// `fmod` with a non-negative result: the pattern test compares an offset against the dash length, and
// a negative remainder would mirror the pattern on either side of the anchor.
float wrapped(float value, float m) {
    const float rest = std::fmod(value, m);
    return rest < 0.0f ? rest + m : rest;
}

}  // namespace

float ant_phase(double time) {
    const double period = 2.0 * static_cast<double>(kAntDashPx);
    const double advanced = std::fmod(time * static_cast<double>(kAntMarchPxPerSecond), period);
    return static_cast<float>(advanced < 0.0 ? advanced + period : advanced);
}

float ant_drawn_phase(const AntOverlay& ants) {
    return ants.phases.empty() ? ant_phase(ants.time) : ants.phases.front();
}

double ant_line(MaskSide side, int fixed, int across) {
    // Coverage ends at `fixed` on the up and left sides and one pixel further on the down and right
    // ones. Whichever end of the document that puts the line on, pull it inside.
    switch (side) {
    case MaskSide::up:
        return fixed == 0 ? 1.0 : static_cast<double>(fixed);
    case MaskSide::left:
        return fixed == 0 ? 1.0 : static_cast<double>(fixed);
    case MaskSide::down:
        return fixed + 1 == across ? fixed : fixed + 1.0;
    case MaskSide::right:
        return fixed + 1 == across ? fixed : fixed + 1.0;
    }
    return static_cast<double>(fixed);
}

double AntRing::along(MaskSide side, double axis) const {
    switch (side) {
    case MaskSide::up:
        return axis - box.x;
    case MaskSide::right:
        return box.w + (axis - box.y);
    case MaskSide::down:
        return box.w + box.h + (box.right() - axis);
    case MaskSide::left:
        return 2.0 * box.w + box.h + (box.bottom() - axis);
    }
    return 0.0;
}

std::vector<AntDash> ant_dashes(float axis_from, float axis_to, float s_from, float s_to,
                                float phase) {
    std::vector<AntDash> dashes;
    const float period = 2.0f * kAntDashPx;
    const float span = axis_to - axis_from;
    if (span <= 0.0f || s_from == s_to) {
        return dashes;
    }
    // Screen pixels of boundary travelled per screen pixel of run: +1 or -1, its sign being which way
    // round the selection this stretch goes.
    const float per_screen = (s_to - s_from) / span;
    const auto s_at = [s_from, per_screen, axis_from](float axis) {
        return s_from + (axis - axis_from) * per_screen;
    };
    if (!std::isfinite(span) || !std::isfinite(s_from) || !std::isfinite(s_to) ||
        !std::isfinite(phase) || !std::isfinite(per_screen)) {
        return dashes;
    }
    // The multiples are walked in the direction of the run's own axis, which for a side the ring
    // traverses backwards means walking them downwards: iterate a backwards side upwards and the first
    // multiple lands past the run's far end, every boundary clamps onto it, and the side comes out as
    // one un-dashed stretch.
    const float step = per_screen > 0.0f ? 1.0f : -1.0f;
    const float first = step > 0.0f ? std::ceil((s_from + phase) / kAntDashPx)
                                    : std::floor((s_from + phase) / kAntDashPx);
    const float past_end = step > 0.0f ? std::ceil((s_to + phase) / kAntDashPx)
                                       : std::floor((s_to + phase) / kAntDashPx);
    // A counted walk: the run is |s_to - s_from| of boundary, so it holds that many dash widths of
    // cuts, plus one at each end. Bounded this way, no argument spins the loop.
    const long long budget = static_cast<long long>(std::fabs(s_to - s_from) / kAntDashPx) + 4;
    float cut = axis_from;
    for (long long index = 0; index < budget; ++index) {
        const float multiple = first + step * static_cast<float>(index);
        if ((multiple - past_end) * step > 0.0f) {
            break;
        }
        const float boundary = multiple * kAntDashPx - phase;
        const float end = std::clamp(axis_from + (boundary - s_from) / per_screen, axis_from, axis_to);
        if (end > cut) {
            // The colour of the middle, not of the end: a boundary point belongs to whichever band
            // the wrap puts it in, and testing it picks a colour at random for the dash next to it.
            dashes.push_back(
                AntDash{cut, end, wrapped(s_at((cut + end) * 0.5f) + phase, period) < kAntDashPx});
        }
        cut = end;
    }
    return dashes;
}

const char* coverage_view_name(CoverageView view) {
    switch (view) {
        case CoverageView::ants:
            return "ants";
        case CoverageView::overlay:
            return "overlay";
        case CoverageView::both:
            return "both";
    }
    return "both";
}

std::optional<CoverageView> coverage_view_from_name(std::string_view name) {
    for (const CoverageView view : {CoverageView::ants, CoverageView::overlay,
                                    CoverageView::both}) {
        if (name == coverage_view_name(view)) {
            return view;
        }
    }
    return std::nullopt;
}

std::string coverage_view_names() {
    std::string names;
    for (const CoverageView view : {CoverageView::ants, CoverageView::overlay,
                                    CoverageView::both}) {
        if (!names.empty()) {
            names += ", ";
        }
        names += coverage_view_name(view);
    }
    return names;
}

}  // namespace noise::app
