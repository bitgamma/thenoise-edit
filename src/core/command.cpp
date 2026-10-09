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

namespace {

std::string quoted(const std::vector<std::string>& names) {
    std::string out;
    for (const std::string& name : names) {
        if (!out.empty()) {
            out += ", ";
        }
        out += "'" + name + "'";
    }
    return out;
}

}  // namespace

bool CommandTable::add(const std::string& id, const std::string& title,
                       std::vector<std::string> shortcuts, std::vector<std::string> args,
                       CommandHandler handler) {
    if (std::any_of(args.begin(), args.end(),
                   [](const std::string& name) { return name.empty(); })) {
        log::error("command " + id + " declares an empty argument name");
        return false;
    }
    std::vector<std::string> sorted = args;
    std::sort(sorted.begin(), sorted.end());
    if (const auto twice = std::adjacent_find(sorted.begin(), sorted.end()); twice != sorted.end()) {
        log::error("command " + id + " declares an argument twice: '" + *twice + "'");
        return false;
    }
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
    commands_.push_back(Registered{id, title, std::move(shortcuts), std::move(args),
                                   std::move(handler)});
    return true;
}

std::vector<std::string> CommandTable::shortcuts_for(std::string_view id) const {
    for (const Registered& command : commands_) {
        if (command.id == id) {
            return command.shortcuts;
        }
    }
    return {};
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
        entries.push_back(CommandInfo{c.id, c.title, c.shortcuts, c.args});
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
    if (!args.is_null() && !args.is_object()) {
        return command_error(id + " expects its arguments as an object, got " +
                             std::string(args.type_name()));
    }
    if (args.is_object()) {
        std::vector<std::string> unexpected;
        for (const auto& field : args.items()) {
            if (std::find(found->args.begin(), found->args.end(), field.key()) ==
                found->args.end()) {
                unexpected.push_back(field.key());
            }
        }
        if (!unexpected.empty()) {
            // Named in full, and so is what would have worked: an argument that does nothing is the
            // worst kind of wrong, because the call succeeds and the caller keeps believing it.
            std::vector<std::string> takes = found->args;
            std::sort(takes.begin(), takes.end());
            return command_error(quoted(unexpected) +
                                 (unexpected.size() == 1 ? " is not an argument of "
                                                          : " are not arguments of") +
                                 id + "; it takes " +
                                 (takes.empty() ? std::string("none") : quoted(takes)));
        }
    }
    return found->handler(args);
}

}
