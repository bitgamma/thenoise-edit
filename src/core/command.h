// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <functional>
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>
#include <vector>

namespace noise {

// Every action the app can perform is registered here exactly once. The UI, the
// keyboard shortcuts and the debug wire all dispatch through this table, so a
// capability that is missing here is a capability the agent cannot verify either.
struct CommandResult {
    bool ok = false;
    nlohmann::json value = nlohmann::json::object();
    std::string error;
};

using CommandHandler = std::function<CommandResult(const nlohmann::json& args)>;

struct CommandInfo {
    std::string id;
    std::string title;
    std::vector<std::string> shortcuts;
};

class CommandTable {
public:
    // `shortcuts` is this command's key bindings in canonical text (core/shortcut.h). Each
    // one is normalized here, and a malformed shortcut or one already bound to another
    // command refuses the registration outright: the table is the only home of key
    // bindings, so a broken binding is a build-time bug, not a runtime surprise.
    bool add(const std::string& id, const std::string& title,
             std::vector<std::string> shortcuts, CommandHandler handler);
    bool contains(const std::string& id) const;
    std::vector<CommandInfo> list() const;
    // Id of the command bound to this shortcut, empty when nothing is.
    std::string command_for_shortcut(std::string_view shortcut) const;
    CommandResult invoke(const std::string& id, const nlohmann::json& args) const;

private:
    struct Registered {
        std::string id;
        std::string title;
        std::vector<std::string> shortcuts;
        CommandHandler handler;
    };

    std::vector<Registered> commands_;
};

CommandResult command_ok(nlohmann::json value);
CommandResult command_error(std::string message);

}
