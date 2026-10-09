// SPDX-License-Identifier: Apache-2.0
#include "test.h"

#include "app/commands.h"
#include "app/shortcuts.h"
#include "core/command.h"
#include "core/log.h"
#include "core/shortcut.h"
#include "dbg/wire.h"

#include <nlohmann/json.hpp>
#include <sokol_app.h>

#include <set>
#include <string>

using namespace noise;
using nlohmann::json;

namespace {

void check(const std::string& text, const char* expected) {
    const std::optional<std::string> got = normalize_shortcut(text);
    if (expected == nullptr) {
        CHECK(!got.has_value());
        return;
    }
    CHECK(got.has_value());
    CHECK_EQ(got.value_or(""), std::string(expected));
}

void test_canonical_text() {
    check("F", "F");
    check("ctrl+f", "Ctrl+F");
    check("CTRL+SHIFT+Z", "Ctrl+Shift+Z");
    check("space", "Space");
    check("[", "[");
    check("Ctrl++", "Ctrl++");   // `+` is a key too
    check("pageup", "PageUp");

    check("", nullptr);
    check("Ctrl+", nullptr);
    check("Ctrl+foo", nullptr);
    check("Super+Z", nullptr);
    check("Shift+Ctrl+Z", nullptr);   // modifiers have one order
    check("Ctrl+Ctrl+Z", nullptr);
    check("F F", nullptr);

    // The vocabulary is its own contract: every name it publishes normalizes to itself.
    for (const std::string& name : named_key_names()) {
        CHECK_EQ(normalize_shortcut(name).value_or(""), name);
    }
}

void test_the_table_owns_its_bindings() {
    CommandTable table;
    CHECK(table.add("view.fit", "fit", {"F"}, {}, [](const json&) { return command_ok(json::object()); }));
    CHECK(table.add("doc.save", "save", {"ctrl+s"}, {},
                    [](const json&) { return command_ok(json::object()); }));

    // Lookup is by meaning, not spelling: the host produces canonical text, and a driver
    // typing something else gets the same command or none, never a near miss.
    CHECK_EQ(table.command_for_shortcut("f"), std::string("view.fit"));
    CHECK_EQ(table.command_for_shortcut("Ctrl+S"), std::string("doc.save"));
    CHECK(table.command_for_shortcut("Ctrl+F").empty());
    CHECK(table.command_for_shortcut("nonsense").empty());

    // A second claim on one key, and a key that does not exist, are refused rather than
    // quietly shadowed or left inert.
    CHECK(!table.add("other.fit", "impostor", {"f"}, {},
                     [](const json&) { return command_ok(json::object()); }));
    CHECK(!table.add("bad.key", "typo", {"Cntl+Z"}, {},
                     [](const json&) { return command_ok(json::object()); }));
    CHECK_EQ(table.command_for_shortcut("F"), std::string("view.fit"));
    CHECK(!table.contains("other.fit"));
    CHECK(!table.contains("bad.key"));

    // `list()` is the wire's view of the table, and it is what a driver reads to learn
    // the keymap, so the shortcuts have to travel with it.
    const std::vector<CommandInfo> listed = table.list();
    CHECK_EQ(listed.size(), std::size_t(2));
    const CommandInfo& fit = listed[0].id == "view.fit" ? listed[0] : listed[1];
    CHECK_EQ(fit.shortcuts.size(), std::size_t(1));
    CHECK_EQ(fit.shortcuts[0], std::string("F"));
}

// Everything the platform can report for one key press, with every modifier combination.
// Scanning codes rather than names keeps this independent of the vocabulary: if the host
// cannot produce a key a command is bound to, that shows up as absent here.
std::set<std::string> producible_shortcuts() {
    std::set<std::string> out;
    for (int code = 32; code <= 400; ++code) {
        for (unsigned mods = 0; mods < 8; ++mods) {
            sapp_event ev{};
            ev.type = SAPP_EVENTTYPE_KEY_DOWN;
            ev.key_code = static_cast<sapp_keycode>(code);
            ev.modifiers = mods & 2u ? SAPP_MODIFIER_CTRL : 0;
            ev.modifiers |= mods & 4u ? SAPP_MODIFIER_ALT : 0;
            ev.modifiers |= mods & 1u ? SAPP_MODIFIER_SHIFT : 0;
            if (const std::optional<std::string> text = app::shortcut_of(ev)) {
                out.insert(*text);
            }
        }
    }
    return out;
}

sapp_event key(sapp_keycode code, unsigned modifiers = 0, bool repeat = false) {
    sapp_event ev{};
    ev.type = SAPP_EVENTTYPE_KEY_DOWN;
    ev.key_code = code;
    ev.modifiers = modifiers;
    ev.key_repeat = repeat;
    return ev;
}

void test_key_events_become_canonical_text() {
    const std::optional<std::string> f = app::shortcut_of(key(SAPP_KEYCODE_F));
    CHECK_EQ(f.value_or(""), std::string("F"));
    const std::optional<std::string> ctrl_s =
        app::shortcut_of(key(SAPP_KEYCODE_S, SAPP_MODIFIER_CTRL));
    CHECK_EQ(ctrl_s.value_or(""), std::string("Ctrl+S"));
    CHECK_EQ(app::shortcut_of(key(SAPP_KEYCODE_SPACE)).value_or(""), std::string("Space"));
    CHECK_EQ(app::shortcut_of(key(SAPP_KEYCODE_ESCAPE)).value_or(""), std::string("Escape"));
    CHECK_EQ(app::shortcut_of(key(SAPP_KEYCODE_F5)).value_or(""), std::string("F5"));
    CHECK_EQ(app::shortcut_of(key(SAPP_KEYCODE_Z, SAPP_MODIFIER_CTRL | SAPP_MODIFIER_SHIFT))
                 .value_or(""),
             std::string("Ctrl+Shift+Z"));

    CHECK(!app::shortcut_of(key(SAPP_KEYCODE_F, 0, true)).has_value());  // held key repeat
    CHECK(!app::shortcut_of([] {
        sapp_event ev{};
        ev.type = SAPP_EVENTTYPE_KEY_UP;
        ev.key_code = SAPP_KEYCODE_F;
        return ev;
    }()).has_value());
    // Bare modifiers and unmapped codes name no key, so they bind nothing.
    CHECK(!app::shortcut_of(key(SAPP_KEYCODE_LEFT_SHIFT)).has_value());
    CHECK(!app::shortcut_of(key(SAPP_KEYCODE_WORLD_1)).has_value());
}

// The binding table and the platform translation must agree, or a shortcut reads as bound
// and never fires. This is the whole reason both live in one table.
void test_every_registered_shortcut_is_reachable_by_a_key_press() {
    const std::set<std::string> producible = producible_shortcuts();
    CHECK(!producible.empty());

    AppState state;
    CommandTable table;
    dbg::DebugWire wire{table};
    const app::HostHooks hooks{nullptr, nullptr};
    app::register_commands(state, table, wire, hooks);

    std::size_t bound = 0;
    for (const CommandInfo& info : table.list()) {
        for (const std::string& shortcut : info.shortcuts) {
            ++bound;
            CHECK(producible.count(shortcut) == 1);
        }
    }
    CHECK(bound > 0);
}

}

int main() {
    log::set_level(log::Level::Error);
    test_canonical_text();
    test_the_table_owns_its_bindings();
    test_key_events_become_canonical_text();
    test_every_registered_shortcut_is_reachable_by_a_key_press();
    return test::summary("shortcut");
}
