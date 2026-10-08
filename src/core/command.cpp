// SPDX-License-Identifier: Apache-2.0
#include "core/command.h"

#include "core/log.h"

#include <algorithm>

namespace noise {

CommandResult command_ok(nlohmann::json value) {
    CommandResult result;
    result.ok = true;
    result.value = std::move(value);
    return result;
}

CommandResult command_error(std::string message) {
    CommandResult result;
    result.ok = false;
    result.error = std::move(message);
    return result;
}

bool CommandTable::add(const std::string& id, const std::string& title, CommandHandler handler) {
    if (contains(id)) {
        log::error("command id registered twice: " + id);
        return false;
    }
    commands_.push_back(Registered{id, title, std::move(handler)});
    return true;
}

bool CommandTable::contains(const std::string& id) const {
    return std::any_of(commands_.begin(), commands_.end(),
                       [&](const Registered& c) { return c.id == id; });
}

std::vector<CommandTable::Entry> CommandTable::list() const {
    std::vector<Entry> entries;
    entries.reserve(commands_.size());
    for (const Registered& c : commands_) {
        entries.emplace_back(c.id, c.title);
    }
    std::sort(entries.begin(), entries.end(),
              [](const Entry& a, const Entry& b) { return a.first < b.first; });
    return entries;
}

CommandResult CommandTable::invoke(const std::string& id, const nlohmann::json& args) const {
    const auto found = std::find_if(commands_.begin(), commands_.end(),
                                    [&](const Registered& c) { return c.id == id; });
    if (found == commands_.end()) {
        std::string known;
        for (const Registered& c : commands_) {
            if (!known.empty()) {
                known += ", ";
            }
            known += c.id;
        }
        return command_error("unknown command '" + id + "'; registered: " + known);
    }
    return found->handler(args);
}

}
