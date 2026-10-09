// SPDX-License-Identifier: Apache-2.0
#include "core/command.h"

#include "core/log.h"
#include "core/shortcut.h"

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

bool CommandTable::add(const std::string& id, const std::string& title,
                       std::vector<std::string> shortcuts, CommandHandler handler) {
    if (contains(id)) {
        log::error("command id registered twice: " + id);
        return false;
    }
    for (std::string& shortcut : shortcuts) {
        const std::optional<std::string> canonical = normalize_shortcut(shortcut);
        if (!canonical) {
            log::error("command " + id + " has a malformed shortcut: " + shortcut);
            return false;
        }
        const std::string holder = command_for_shortcut(*canonical);
        if (!holder.empty()) {
            log::error("shortcut " + *canonical + " is bound twice: " + holder + " and " + id);
            return false;
        }
        shortcut = *canonical;
    }
    commands_.push_back(Registered{id, title, std::move(shortcuts), std::move(handler)});
    return true;
}

std::string CommandTable::command_for_shortcut(std::string_view shortcut) const {
    const std::optional<std::string> canonical = normalize_shortcut(shortcut);
    if (!canonical) {
        return {};
    }
    for (const Registered& c : commands_) {
        if (std::find(c.shortcuts.begin(), c.shortcuts.end(), *canonical) != c.shortcuts.end()) {
            return c.id;
        }
    }
    return {};
}

bool CommandTable::contains(const std::string& id) const {
    return std::any_of(commands_.begin(), commands_.end(),
                       [&](const Registered& c) { return c.id == id; });
}

std::vector<CommandInfo> CommandTable::list() const {
    std::vector<CommandInfo> entries;
    entries.reserve(commands_.size());
    for (const Registered& c : commands_) {
        entries.push_back(CommandInfo{c.id, c.title, c.shortcuts});
    }
    std::sort(entries.begin(), entries.end(),
              [](const CommandInfo& a, const CommandInfo& b) { return a.id < b.id; });
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
