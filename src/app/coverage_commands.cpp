// SPDX-License-Identifier: Apache-2.0
#include "app/coverage.h"

#include "app/command_args.h"
#include "app/command_groups.h"

#include "core/layer.h"

#include <nlohmann/json.hpp>

namespace noise::app {
namespace {

json targets_json() {
    json out = json::array();
    for (const CoverageTargetInfo& info : kCoverageTargets) {
        out.push_back({{"id", info.id}, {"title", info.title}});
    }
    return out;
}

}  // namespace

void register_coverage_commands(AppState& state, CommandTable& table) {
    // The switch between the two lifetimes of one coverage. With no argument it is a read, and it
    // answers with what the target it names can actually do, so the panel draws the truth — a mask half
    // that is dimmed because this layer has no mask — instead of guessing from its own memory.
    table.add("coverage.target",
              "Read or set which coverage the pointer writes ('marked' or 'mask')", {}, {"to"},
              [&state](const json& args) -> CommandResult {
                  if (args.contains("to")) {
                      if (!args.at("to").is_string()) {
                          return fail("coverage.target",
                                      "'to' must be one of " + coverage_target_names());
                      }
                      const std::optional<CoverageTarget> target =
                          coverage_target_from_name(args.at("to").get<std::string>());
                      if (!target) {
                          return fail("coverage.target", "'to' names no coverage target: '" +
                                                             args.at("to").get<std::string>() +
                                                             "' (" + coverage_target_names() + ")");
                      }
                      state.coverage_target = *target;
                  }
                  const bool has_document = !state.doc.empty();
                  const Layer& layer = has_document ? state.active_layer() : Layer{};
                  return command_ok(json{{"target", coverage_target_name(state.coverage_target)},
                                         {"targets", targets_json()},
                                         {"writable", state.coverage_writable()},
                                         {"layer", layer.name},
                                         {"has_mask", layer.mask.has_value()}});
              });
}

}  // namespace noise::app
