// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <functional>
#include <nlohmann/json.hpp>
#include <string>
#include <utility>
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

class CommandTable {
public:
    using Entry = std::pair<std::string, std::string>;  // id, title

    bool add(const std::string& id, const std::string& title, CommandHandler handler);
    bool contains(const std::string& id) const;
    std::vector<Entry> list() const;
    CommandResult invoke(const std::string& id, const nlohmann::json& args) const;

private:
    struct Registered {
        std::string id;
        std::string title;
        CommandHandler handler;
    };

    std::vector<Registered> commands_;
};

CommandResult command_ok(nlohmann::json value);
CommandResult command_error(std::string message);

}
