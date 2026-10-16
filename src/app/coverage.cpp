// SPDX-License-Identifier: Apache-2.0
#include "app/coverage.h"

namespace noise::app {

const std::vector<CoverageTargetInfo> kCoverageTargets{
    {CoverageTarget::marked, "marked", "the marked area"},
    {CoverageTarget::mask, "mask", "the active layer's mask"},
};

std::optional<CoverageTarget> coverage_target_from_name(const std::string& name) {
    for (const CoverageTargetInfo& info : kCoverageTargets) {
        if (name == info.id) {
            return info.target;
        }
    }
    return std::nullopt;
}

const char* coverage_target_name(CoverageTarget target) {
    for (const CoverageTargetInfo& info : kCoverageTargets) {
        if (info.target == target) {
            return info.id;
        }
    }
    return kCoverageTargets.front().id;
}

std::string coverage_target_names() {
    std::string names;
    for (const CoverageTargetInfo& info : kCoverageTargets) {
        if (!names.empty()) {
            names += ", ";
        }
        names += info.id;
    }
    return names;
}

}  // namespace noise::app
