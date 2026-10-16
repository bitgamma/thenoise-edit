// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <optional>
#include <string>
#include <vector>

namespace noise::app {

// Which coverage the pointer writes. Coverage is one thing — `core::Mask` — but it has two homes, and
// what differs is only how long it lives: the marked area is put down when the work is done, the active
// layer's mask is saved with the document. The switch says which one the editor is holding, so the shape
// language, the brush and the maths are shared instead of forked per lifetime.
enum class CoverageTarget {
    marked,
    mask,
};

struct CoverageTargetInfo {
    CoverageTarget target;
    const char* id;
    const char* title;
};

// The rows the command table, the panel and every refusal read: a target that is not here cannot be
// asked for, and one added here is offerable everywhere without a name compared anywhere.
extern const std::vector<CoverageTargetInfo> kCoverageTargets;

std::optional<CoverageTarget> coverage_target_from_name(const std::string& name);
const char* coverage_target_name(CoverageTarget target);
// "marked, mask" — what a refusal lists.
std::string coverage_target_names();
// "mark the area, or paint the active layer's mask" — what the panel says.
std::string coverage_target_titles();

}  // namespace noise::app
