// SPDX-License-Identifier: Apache-2.0
#include "test.h"

#include "core/command.h"
#include "core/log.h"

#include <nlohmann/json.hpp>

#include <stdexcept>
#include <string>
#include <vector>

using namespace noise;
using nlohmann::json;

namespace {

CommandResult echo(const json& args) { return command_ok(json{{"echo", args}}); }

CommandResult ok_empty(const json&) { return command_ok(json::object()); }

void test_dispatch_and_listing() {
    CommandTable table;
    CHECK(table.add("alpha.one", "first", {}, {"x", "nested"}, &echo));
    CHECK(table.add("alpha.two", "second", {}, {}, [](const json&) {
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
    CHECK_EQ(entries[0].id, std::string("alpha.one"));
    CHECK_EQ(entries[1].title, std::string("second"));
    CHECK(entries[0].shortcuts.empty());
    // A caller discovers what a command takes from the listing; an argument that is only accepted
    // in practice is not discoverable at all.
    CHECK((entries[0].args == std::vector<std::string>{"x", "nested"}));
    CHECK(entries[1].args.empty());
}

// An argument the command does not read is refused, naming the argument, the command and what
// would have worked. A call that quietly ignores a field the caller believed in is the worst kind
// of success: it returns ok and leaves a program - or a probe - measuring something else.
void test_an_undeclared_argument_is_refused_by_name() {
    CommandTable table;
    CHECK(table.add("paint.thing", "paints", {}, {"size", "color"}, &echo));
    CHECK(table.add("bare.thing", "takes nothing", {}, {}, &ok_empty));

    const CommandResult mistyped = table.invoke("paint.thing", {{"size", 4}, {"colour", "red"}});
    CHECK(!mistyped.ok);
    CHECK(mistyped.error.find("'colour'") != std::string::npos);
    CHECK(mistyped.error.find("paint.thing") != std::string::npos);
    CHECK(mistyped.error.find("'size'") != std::string::npos);  // says what would have worked
    CHECK(mistyped.value.empty());                              // the handler never ran

    const CommandResult two_bad = table.invoke(
        "paint.thing", {{"colour", "red"}, {"oacity", 1.0}, {"size", 2}});
    CHECK(!two_bad.ok);
    CHECK(two_bad.error.find("are not arguments") != std::string::npos);
    CHECK(two_bad.error.find("'colour'") != std::string::npos);
    CHECK(two_bad.error.find("'oacity'") != std::string::npos);

    const CommandResult on_a_bare_command = table.invoke("bare.thing", {{"index", 2}});
    CHECK(!on_a_bare_command.ok);
    CHECK(on_a_bare_command.error.find("it takes none") != std::string::npos);

    // Declared arguments, and no arguments at all, still go through. A null argument object is the
    // same as an empty one: `command.invoke` and the UI both pass one or the other.
    CHECK(table.invoke("paint.thing", {{"size", 4}}).ok);
    CHECK(table.invoke("bare.thing", json::object()).ok);
    CHECK(table.invoke("bare.thing", nullptr).ok);

    // Arguments that are not an object are refused rather than quietly read as an empty one.
    const CommandResult scalar = table.invoke("bare.thing", 7);
    CHECK(!scalar.ok);
    CHECK(scalar.error.find("object") != std::string::npos);
    const CommandResult array = table.invoke("bare.thing", json::array({1, 2}));
    CHECK(!array.ok);
}

// The refusal is only honest if a registration cannot declare a list that lies about itself.
void test_a_broken_argument_list_refuses_the_registration() {
    CommandTable table;
    CHECK(!table.add("bad.empty_name", "", {}, {""}, &ok_empty));
    CHECK(!table.add("bad.twice", "", {}, {"size", "size"}, &ok_empty));
    CHECK(table.add("bad.ok", "", {}, {"size", "color"}, &ok_empty));
    CHECK_EQ(table.list().size(), std::size_t(1));
}

void test_unknown_command_names_itself_and_alternatives() {
    CommandTable table;
    table.add("view.zoom", "zoom", {}, {"factor", "anchor"}, &ok_empty);
    table.add("view.pan", "pan", {}, {"dx", "dy"}, &ok_empty);

    const CommandResult result = table.invoke("view.zoomm", json::object());
    CHECK(!result.ok);
    CHECK(result.error.find("view.zoomm") != std::string::npos);
    CHECK(result.error.find("view.pan") != std::string::npos);
}

void test_duplicate_id_is_refused_and_first_wins() {
    CommandTable table;
    CHECK(table.add("dup", "original", {}, {},
                    [](const json&) { return command_ok(json{{"who", "first"}}); }));
    CHECK(!table.add("dup", "impostor", {}, {},
                     [](const json&) { return command_ok(json{{"who", "second"}}); }));
    const CommandResult result = table.invoke("dup", json::object());
    CHECK(result.ok);
    CHECK_EQ(result.value["who"].get<std::string>(), std::string("first"));
    CHECK_EQ(table.list().size(), std::size_t(1));
}

void test_handler_error_is_propagated_verbatim() {
    CommandTable table;
    table.add("fails", "always fails", {}, {}, [](const json&) {
        return command_error("missing field 'region'");
    });
    const CommandResult result = table.invoke("fails", json::object());
    CHECK(!result.ok);
    CHECK_EQ(result.error, std::string("missing field 'region'"));
    CHECK(result.value.is_object());
}

void test_contains() {
    CommandTable table;
    table.add("known", "", {}, {}, &ok_empty);
    CHECK(table.contains("known"));
    CHECK(!table.contains("unknown"));
}

}  // namespace

int main() {
    log::set_level(log::Level::Error);
    test_dispatch_and_listing();
    test_an_undeclared_argument_is_refused_by_name();
    test_a_broken_argument_list_refuses_the_registration();
    test_unknown_command_names_itself_and_alternatives();
    test_duplicate_id_is_refused_and_first_wins();
    test_handler_error_is_propagated_verbatim();
    test_contains();
    return test::summary("command");
}
