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
    std::vector<std::string> args;  // the argument names this command reads, and the only ones it takes
};

class CommandTable {
public:
    // `shortcuts` is this command's key bindings in canonical text (core/shortcut.h). Each
    // one is normalized here, and a malformed shortcut or one already bound to another
    // command refuses the registration outright: the table is the only home of key
    // bindings, so a broken binding is a build-time bug, not a runtime surprise.
    //
    // `args` is the complete set of top-level argument names the handler reads. `invoke` refuses
    // anything outside it, so a call can never be silently ignored: an argument the caller believed
    // in is either read or named in an error. A command that reads fields from a table of its own
    // (the brush fields) builds its list from that table rather than restating it.
    bool add(const std::string& id, const std::string& title,
             std::vector<std::string> shortcuts, std::vector<std::string> args,
             CommandHandler handler);
    bool contains(const std::string& id) const;
    std::vector<CommandInfo> list() const;
    // Id of the command bound to this shortcut, empty when nothing is.
    std::string command_for_shortcut(std::string_view shortcut) const;
    // The bindings of one command in canonical text, empty if it has none or no such command. So a
    // panel can print the key beside a control without restating a binding it could get wrong.
    std::vector<std::string> shortcuts_for(std::string_view id) const;
    CommandResult invoke(const std::string& id, const nlohmann::json& args) const;

private:
    struct Registered {
        std::string id;
        std::string title;
        std::vector<std::string> shortcuts;
        std::vector<std::string> args;
        CommandHandler handler;
    };

    std::vector<Registered> commands_;
};

CommandResult command_ok(nlohmann::json value);
CommandResult command_error(std::string message);

}
