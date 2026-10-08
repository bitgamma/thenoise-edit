// SPDX-License-Identifier: Apache-2.0
#include "test.h"

#include "core/command.h"
#include "core/log.h"

#include <nlohmann/json.hpp>

#include <stdexcept>
#include <string>

using namespace noise;
using nlohmann::json;

namespace {

void test_dispatch_and_listing() {
    CommandTable table;
    CHECK(table.add("alpha.one", "first", [](const json& args) {
        return command_ok(json{{"echo", args}});
    }));
    CHECK(table.add("alpha.two", "second", [](const json&) {
        return command_ok(json{{"n", 7}});
    }));

    const json args = {{"x", 1}, {"nested", {true, nullptr, "s"}}};
    const CommandResult result = table.invoke("alpha.one", args);
    CHECK(result.ok);
    CHECK(result.error.empty());
    CHECK(result.value["echo"] == args);
    CHECK_EQ(table.invoke("alpha.two", json::object()).value["n"].get<int>(), 7);

    const auto entries = table.list();
    CHECK_EQ(entries.size(), std::size_t(2));
    CHECK_EQ(entries[0].first, std::string("alpha.one"));
    CHECK_EQ(entries[1].second, std::string("second"));
}

void test_unknown_command_names_itself_and_alternatives() {
    CommandTable table;
    table.add("view.zoom", "zoom", [](const json&) { return command_ok(json::object()); });
    table.add("view.pan", "pan", [](const json&) { return command_ok(json::object()); });

    const CommandResult result = table.invoke("view.zoomm", json::object());
    CHECK(!result.ok);
    CHECK(result.error.find("view.zoomm") != std::string::npos);
    CHECK(result.error.find("view.pan") != std::string::npos);
}

void test_duplicate_id_is_refused_and_first_wins() {
    CommandTable table;
    CHECK(table.add("dup", "original", [](const json&) { return command_ok(json{{"who", "first"}}); }));
    CHECK(!table.add("dup", "impostor", [](const json&) { return command_ok(json{{"who", "second"}}); }));
    const CommandResult result = table.invoke("dup", json::object());
    CHECK(result.ok);
    CHECK_EQ(result.value["who"].get<std::string>(), std::string("first"));
    CHECK_EQ(table.list().size(), std::size_t(1));
}

void test_handler_error_is_propagated_verbatim() {
    CommandTable table;
    table.add("fails", "always fails", [](const json&) {
        return command_error("missing field 'region'");
    });
    const CommandResult result = table.invoke("fails", json::object());
    CHECK(!result.ok);
    CHECK_EQ(result.error, std::string("missing field 'region'"));
    CHECK(result.value.is_object());
}

void test_contains() {
    CommandTable table;
    table.add("known", "", [](const json&) { return command_ok(json::object()); });
    CHECK(table.contains("known"));
    CHECK(!table.contains("unknown"));
}

}

int main() {
    log::set_level(log::Level::Error);
    test_dispatch_and_listing();
    test_unknown_command_names_itself_and_alternatives();
    test_duplicate_id_is_refused_and_first_wins();
    test_handler_error_is_propagated_verbatim();
    test_contains();
    return test::summary("command");
}
