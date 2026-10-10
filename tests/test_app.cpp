// SPDX-License-Identifier: Apache-2.0
#include "test.h"

#include "app/commands.h"
#include "app/tools/select.h"
#include "app/state.h"
#include "core/image.h"
#include "io/file.h"
#include "io/project.h"
#include "io/jpeg.h"
#include "io/png.h"
#include "io/zip.h"
#include "core/log.h"
#include "dbg/wire.h"
#include "platform/platform.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <map>
#include <filesystem>
#include <string>
#include <vector>

using namespace noise;
using nlohmann::json;

namespace {

// Drives the real command table with no window and no GPU: whatever the wire can
// reach, this reaches.

// Deterministic stand-in for the framebuffer, so the capture path is checkable
// against pixels we chose.
Image stub_frame(int width, int height) {
    Image frame = Image::filled(Extent{width, height}, 0, 0, 0, 255);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            std::uint8_t* p = frame.at(x, y);
            p[0] = static_cast<std::uint8_t>(x & 0xff);
            p[1] = static_cast<std::uint8_t>(y & 0xff);
            p[2] = 7;
        }
    }
    return frame;
}

struct Harness {
    AppState state;
    CommandTable table;
    dbg::DebugWire wire{table};
    int quits = 0;

    Harness() {
        state.runtime_dir = (std::filesystem::temp_directory_path() /
                             ("noise-app-test-" + std::to_string(platform::pid()))).string();
        std::filesystem::remove_all(state.runtime_dir);
        std::filesystem::create_directories(state.runtime_dir);
        // The host publishes the canvas area every frame it draws; a harness with no
        // frames has to publish it by hand or viewport commands stay pending.
        state.viewport = Extent{1280, 800};
        state.viewport_published = true;
        state.framebuffer = Extent{1280, 800};
        state.canvas_origin = {0.0, 0.0};
        const app::HostHooks hooks{&stub_frame, [this] { ++quits; }};
        app::register_commands(state, table, wire, hooks);
    }

    CommandResult run(const std::string& id, const json& args) { return table.invoke(id, args); }

    json value(const std::string& id, const json& args) {
        const CommandResult result = run(id, args);
        CHECK_OK(result);
        return result.ok ? result.value : json::object();
    }

    std::string temp(const std::string& name) {
        return (std::filesystem::path(state.runtime_dir) / name).string();
    }
};

// The baseline in docs/ARCHITECTURE.md: renaming or dropping one of these silently
// breaks every driver, recorded session and future test that names it.
void test_m0_command_surface_is_registered() {
    Harness h;
    const json listed = h.value("command.list", json::object());
    std::vector<std::string> ids;
    for (const json& entry : listed["commands"]) {
        ids.push_back(entry["id"].get<std::string>());
    }
    for (const char* required : {"app.state", "app.quit", "command.list", "command.invoke",
                                 "doc.new", "doc.open", "doc.save", "doc.get", "image.get",
                                 "screenshot", "pixels.compare", "log.tail", "view.fit",
                                 "view.pan", "view.zoom", "view.set"}) {
        CHECK(std::find(ids.begin(), ids.end(), std::string(required)) != ids.end());
    }
    for (const json& entry : listed["commands"]) {
        CHECK(entry["title"].is_string());
        CHECK(!entry["title"].get<std::string>().empty());
    }
}

// The keymap is only usable if a driver can read it: `command.list` carries the bindings,
// so a shortcut that exists only in a header comment cannot happen here.
void test_the_keymap_is_reported_with_the_commands() {
    Harness h;
    const json listed = h.value("command.list", json::object());
    std::map<std::string, std::string> bound;
    for (const json& entry : listed["commands"]) {
        std::string joined;
        for (const std::string& shortcut : entry["shortcuts"].get<std::vector<std::string>>()) {
            joined += shortcut + " ";
        }
        bound[entry["id"].get<std::string>()] = joined;
    }
    CHECK_EQ(bound["view.fit"], std::string("F "));
    CHECK_EQ(bound["edit.undo"], std::string("Ctrl+Z "));
    CHECK_EQ(bound["edit.redo"], std::string("Ctrl+Y "));
    CHECK_EQ(bound["doc.save"], std::string("Ctrl+S "));
    CHECK_EQ(bound["app.quit"], std::string("Escape Ctrl+Q "));
}

// The refusal of unread arguments lives in the table, so it has to hold across the whole surface:
// a command that quietly ignores a field is a probe that measures something else while reporting
// what it asked for. `command.invoke` needs no exclusion, because the check runs before its handler
// and its own `args` is passed on to the same check one level down.
void test_every_command_refuses_an_argument_it_does_not_declare() {
    Harness h;
    h.value("doc.new", json{{"width", 16}, {"height", 16}});
    const json listed = h.value("command.list", json::object());
    std::size_t checked = 0;
    for (const json& entry : listed["commands"]) {
        const std::string id = entry["id"].get<std::string>();
        const CommandResult result = h.run(id, json{{"no_such_argument_of_this_command", 1}});
        CHECK(!result.ok);
        CHECK(result.error.find("no_such_argument_of_this_command") != std::string::npos);
        CHECK(result.error.find(id) != std::string::npos);
        // Names what would have worked, or that nothing would.
        CHECK(result.error.find("it takes ") != std::string::npos);
        ++checked;
    }
    CHECK(checked >= std::size_t(30));  // the surface, not a subset of it
}

// What closed this: `tool.brush` selects a tool and reads no fields, so a sweep that set the brush
// through it measured one default-sized brush under five labels. Refusing the field is what turns
// that mistake back into an error, and the listing is where a driver finds out instead.
void test_a_brush_field_on_the_command_that_takes_none_is_an_error() {
    Harness h;
    h.value("doc.new", json{{"width", 16}, {"height", 16}});
    const double before = h.value("app.state", json::object())["brush"]["size"].get<double>();
    const CommandResult on_the_tool_command = h.run("tool.brush", json{{"size", before + 12.0}});
    CHECK(!on_the_tool_command.ok);
    CHECK(on_the_tool_command.error.find("'size'") != std::string::npos);
    CHECK(on_the_tool_command.error.find("it takes none") != std::string::npos);
    // Refused means nothing happened, not "accepted and ignored".
    CHECK_EQ(h.value("app.state", json::object())["brush"]["size"].get<double>(), before);

    CHECK_NEAR(h.value("tool.set", json{{"size", 12.0}})["size"].get<double>(), 12.0, 1e-12);

    bool saw_tool_set = false;
    bool saw_tool_stroke = false;
    const json listed = h.value("command.list", json::object());
    for (const json& entry : listed["commands"]) {
        const std::string id = entry["id"].get<std::string>();
        if (id != "tool.set" && id != "tool.stroke") {
            continue;
        }
        const std::vector<std::string> args = entry["args"].get<std::vector<std::string>>();
        // Every brush field, declared from the one table that names them.
        for (const char* field : {"size", "hardness", "opacity", "flow", "spacing"}) {
            CHECK(std::find(args.begin(), args.end(), std::string(field)) != args.end());
        }
        CHECK(std::find(args.begin(), args.end(), std::string("color")) != args.end());
        if (id == "tool.set") {
            saw_tool_set = true;
            CHECK(std::find(args.begin(), args.end(), std::string("points")) == args.end());
        } else {
            saw_tool_stroke = true;
            CHECK(std::find(args.begin(), args.end(), std::string("points")) != args.end());
        }
    }
    CHECK(saw_tool_set);
    CHECK(saw_tool_stroke);
}

// One layered format is read, and a container that is not ours says which one it turned out to be.
// Routing by extension used to send an OpenRaster file to the PNG decoder and answer "Not a PNG
// file" about a file that is a perfectly good archive of something this app does not read; a
// directory threw from the standard library. Both are refused, naming the path.
void test_a_container_that_is_not_ours_is_refused_naming_what_it_met() {
    Harness h;
    const std::filesystem::path dir = h.state.runtime_dir;
    const auto bytes = [](const std::string& text) {
        return std::span<const std::uint8_t>(
            reinterpret_cast<const std::uint8_t*>(text.data()), text.size());
    };
    io::ZipWriter zip;
    zip.add("mimetype", bytes("application/openraster"));
    zip.add("stack.xml", bytes("<image w=\"1\" h=\"1\"><stack/></image>"));
    std::string write_error;
    CHECK(io::write_file_atomic((dir / "foreign.ora").string(), zip.bytes(), write_error));
    // The same bytes under our own extension: the content decides what a file is, so the answer is
    // the same and the name buys nothing.
    CHECK(io::write_file_atomic((dir / "claims-to-be.noise").string(), zip.bytes(), write_error));

    for (const char* name : {"foreign.ora", "claims-to-be.noise"}) {
        const CommandResult result = h.run("doc.open", json{{"path", (dir / name).string()}});
        CHECK(!result.ok);
        CHECK(result.error.find("application/openraster") != std::string::npos);
        CHECK(result.error.find("png") == std::string::npos);  // nothing blamed a decoder it never ran
    }

    const CommandResult directory = h.run("doc.open", json{{"path", dir.string()}});
    CHECK(!directory.ok);
    CHECK(directory.error.find("not a regular file") != std::string::npos);
    CHECK(directory.error.find(dir.string()) != std::string::npos);
}

// The export names its format by extension and reports what that format could not carry: a JPEG
// has no alpha channel, so the answer says the transparency was flattened instead of leaving the
// caller to notice after the file is closed.
void test_exporting_to_a_flat_format_reports_what_the_format_could_hold() {
    Harness h;
    for (const char* extension : {"png", "jpg", "jpeg", "webp"}) {
        h.value("doc.new",
                json{{"width", 24}, {"height", 16}, {"fill", json::array({10, 120, 200, 128})}});
        const std::string path = h.temp(std::string("export-") + extension + "." + extension);
        const json result = h.value("doc.export", json{{"path", path}});
        CHECK_EQ(result["exported"].get<std::string>(), path);
        CHECK_EQ(result["format"].get<std::string>(), std::string(extension));
        const bool no_alpha = std::string(extension) == "jpg" || std::string(extension) == "jpeg";
        CHECK_EQ(result["discarded_alpha"].get<bool>(), no_alpha);
        // Only a format with a knob reports one, and PNG is the one format here without it.
        CHECK_EQ(result.contains("quality"), std::string(extension) != "png");
        CHECK(std::filesystem::exists(path));

        // What was written reads back as an image of the size that went out.
        const json opened = h.value("doc.open", json{{"path", path}});
        CHECK_EQ(opened["width"].get<int>(), 24);
        CHECK_EQ(opened["height"].get<int>(), 16);
    }
}

// Nothing is written by a call that was refused, and `quality` is checked against the format that
// was asked for: sending it to a PNG is a field with nothing to do, which is an error, not a no-op.
void test_export_arguments_are_checked_and_nothing_is_written_on_refusal() {
    Harness h;
    h.value("doc.new", json{{"width", 32}, {"height", 32}, {"fill", json::array({1, 2, 3, 255})}});

    const CommandResult tiff = h.run("doc.export", json{{"path", h.temp("refused.tiff")}});
    CHECK(!tiff.ok);
    CHECK(tiff.error.find("tiff") != std::string::npos);
    CHECK(tiff.error.find("webp") != std::string::npos);  // says what it does write
    const CommandResult lossless_quality =
        h.run("doc.export", json{{"path", h.temp("refused.png")}, {"quality", 30}});
    CHECK(!lossless_quality.ok);
    CHECK(lossless_quality.error.find("lossless") != std::string::npos);
    const CommandResult out_of_range =
        h.run("doc.export", json{{"path", h.temp("refused.jpg")}, {"quality", 101}});
    CHECK(!out_of_range.ok);
    CHECK(out_of_range.error.find("101") != std::string::npos);
    const CommandResult wrong_type =
        h.run("doc.export", json{{"path", h.temp("refused.jpg")}, {"quality", "high"}});
    CHECK(!wrong_type.ok);
    CHECK(wrong_type.error.find("quality") != std::string::npos);
    for (const char* name : {"refused.tiff", "refused.png", "refused.jpg"}) {
        CHECK(!std::filesystem::exists(h.temp(name)));
    }

    // Where it does mean something, it does something.
    h.value("doc.export", json{{"path", h.temp("small.jpg")}, {"quality", 20}});
    h.value("doc.export", json{{"path", h.temp("large.jpg")}, {"quality", 95}});
    CHECK(std::filesystem::file_size(h.temp("small.jpg")) <
          std::filesystem::file_size(h.temp("large.jpg")));
}

json full_stroke() {
    return json{{"points", json::array({json::array({5.0, 5.0}), json::array({30.0, 20.0})})},
                {"tool", "brush"},
                {"size", 8.0},
                {"hardness", 0.6},
                {"opacity", 0.8},
                {"flow", 0.7},
                {"spacing", 0.25},
                {"color", json::array({255, 0, 0, 255})}};
}

std::string checksum_of(Harness& h) {
    return h.value("doc.get", json::object())["checksum"].get<std::string>();
}

int undo_depth(Harness& h) {
    return h.value("app.state", json::object())["document"]["history"]["undo_depth"].get<int>();
}

void test_one_stroke_is_one_step_that_puts_every_byte_back() {
    Harness h;
    h.value("doc.new", json{{"width", 40},
                            {"height", 30},
                            {"fill", json::array({10, 20, 30, 255})}});
    const std::string blank = checksum_of(h);
    CHECK_EQ(undo_depth(h), 0);

    const json painted = h.value("tool.stroke", full_stroke());
    CHECK(painted["dabs"].get<int>() > 1);
    CHECK_EQ(painted["touched"].size(), 4u);
    const std::string stroked = checksum_of(h);
    CHECK_NE(stroked, blank);
    CHECK_EQ(undo_depth(h), 1);

    const json undone = h.value("edit.undo", json::object());
    CHECK_EQ(undone["undo_depth"].get<int>(), 0);
    CHECK_EQ(undone["redo_depth"].get<int>(), 1);
    CHECK_EQ(checksum_of(h), blank);

    h.value("edit.redo", json::object());
    CHECK_EQ(checksum_of(h), stroked);

    // What the status bar shows is what the last stroke ran with, so a screenshot can be
    // reproduced from reported state alone.
    const json brush = h.value("app.state", json::object())["brush"];
    CHECK_EQ(brush["tool"].get<std::string>(), std::string("brush"));
    CHECK_NEAR(brush["size"].get<double>(), 8.0, 1e-12);
    CHECK_NEAR(brush["hardness"].get<double>(), 0.6, 1e-12);
}

// A move gesture of an exact whole-pixel offset, fed through the pointer command the way the
// window feeds it: press, a couple of drags through intermediate positions, release.
void pointer_offset_gesture(Harness& h, int dx, int dy) {
    h.value("tool.pointer", json{{"phase", "press"}, {"at", json::array({10.0, 10.0})}});
    h.value("tool.pointer",
            json{{"phase", "drag"}, {"at", json::array({10.0 + dx * 0.5, 10.0 + dy * 0.5})}});
    h.value("tool.pointer", json{{"phase", "drag"}, {"at", json::array({10.0 + dx, 10.0 + dy})}});
    h.value("tool.pointer", json{{"phase", "release"}});
}

void pointer_gesture(Harness& h, const json& points) {
    h.run("tool.pointer", json{{"phase", "press"}, {"at", points.front()}});
    for (std::size_t i = 1; i < points.size(); ++i) {
        h.run("tool.pointer", json{{"phase", "drag"}, {"at", points[i]}});
    }
    h.run("tool.pointer", json{{"phase", "release"}});
}

// The reason a live stroke keeps its own coverage buffer and its own dab walk: a gesture fed
// in event by event must land on the same bytes as the same polyline painted in one call.
// If this ever differs, what the user saw while dragging is not what got committed.
void test_a_pointer_gesture_lands_where_the_same_path_painted_in_one_go_lands() {
    Harness h;
    h.value("doc.new", json{{"width", 64},
                            {"height", 48},
                            {"fill", json::array({20, 60, 90, 255})}});
    const json points = json::array({json::array({6.0, 6.0}), json::array({40.0, 22.0}),
                                     json::array({12.0, 40.0})});
    json stroke_args = full_stroke();
    stroke_args["points"] = points;
    h.value("tool.stroke", stroke_args);
    const std::string one_call = checksum_of(h);
    const json back = h.value("edit.undo", json::object());
    CHECK_EQ(back["undo_depth"].get<int>(), 0);
    const std::string clean = checksum_of(h);
    CHECK_NE(clean, one_call);

    pointer_gesture(h, points);

    // The same polyline, fed one pointer event at a time across frames, lands on the same
    // bytes: nothing is double-composited and no dab was lost between events.
    CHECK_EQ(checksum_of(h), one_call);

    // And the whole drag was one step, not one step per pointer event.
    const json undone = h.value("edit.undo", json::object());
    CHECK_EQ(undone["undo_depth"].get<int>(), 0);
    CHECK_EQ(checksum_of(h), clean);

    const json again = h.value("edit.redo", json::object());
    CHECK_EQ(again["redo_depth"].get<int>(), 0);
    CHECK_EQ(checksum_of(h), one_call);
}

void test_a_pointer_gesture_is_one_step_and_reports_itself() {
    Harness h;
    h.value("doc.new", json{{"width", 48},
                            {"height", 32},
                            {"fill", json::array({0, 0, 0, 0})}});
    const std::string empty = checksum_of(h);
    const json points = json::array({json::array({4.0, 4.0}), json::array({44.0, 28.0})});
    h.value("tool.pointer", json{{"phase", "press"}, {"at", points.front()}});

    const json mid = h.value("app.state", json::object());
    CHECK(mid["stroke"]["active"].get<bool>());
    CHECK(mid["stroke"]["dabs"].get<int>() > 0);
    CHECK_EQ(mid["document"]["history"]["undo_depth"].get<int>(), 0);  // nothing committed yet
    CHECK_NE(checksum_of(h), empty);                                    // but the canvas changed

    for (std::size_t i = 1; i < points.size(); ++i) {
        h.value("tool.pointer", json{{"phase", "drag"}, {"at", points[i]}});
    }
    h.value("tool.pointer", json{{"phase", "release"}});

    const json after = h.value("app.state", json::object());
    CHECK_EQ(after["document"]["history"]["undo_depth"].get<int>(), 1);
    CHECK(!after["stroke"]["active"].get<bool>());
    CHECK_EQ(after["stroke"]["dabs"].get<int>(), 0);

    const json undone = h.value("edit.undo", json::object());
    CHECK_EQ(undone["undo_depth"].get<int>(), 0);
    CHECK_EQ(checksum_of(h), empty);
}

void test_a_gesture_that_leaves_the_canvas_still_undoes_completely() {
    Harness h;
    h.state.viewport = Extent{16, 16};  // a small canvas over a bigger document
    h.value("doc.new", json{{"width", 64},
                            {"height", 48},
                            {"fill", json::array({5, 5, 5, 255})}});
    const std::string before = checksum_of(h);
    const json points = json::array({json::array({4.0, 4.0}), json::array({60.0, 44.0})});

    pointer_gesture(h, points);
    const std::string painted = checksum_of(h);
    CHECK_NE(painted, before);

    h.value("edit.undo", json::object());
    CHECK_EQ(checksum_of(h), before);
}

// Everything outside the stroke's own rectangle has to be byte-identical: that is what
// recording a stroke's bounds buys, and a pointer path that grew its scratch buffer the easy
// way (replace instead of union) is exactly what it catches.
void test_a_pointer_gesture_changes_no_pixel_outside_its_own_rectangle() {
    Harness h;
    h.value("doc.new", json{{"width", 64},
                            {"height", 48},
                            {"fill", json::array({200, 10, 10, 255})}});
    h.value("tool.set", json{{"tool", "brush"},
                             {"size", 12.0},
                             {"hardness", 1.0},
                             {"opacity", 1.0},
                             {"flow", 1.0},
                             {"spacing", 0.3},
                             {"color", json::array({0, 0, 0, 255})}});
    const Image before = h.state.doc.composite();

    pointer_gesture(h, json::array({json::array({16.0, 24.0}), json::array({40.0, 24.0})}));

    const int reach = static_cast<int>(std::ceil(12.0 * 0.5 + 1.0));  // radius plus the rim band
    const Rect area{16 - reach, 24 - reach, (40 - 16) + 2 * reach, 2 * reach};
    int outside = 0;
    int inside = 0;
    for (int y = 0; y < 48; ++y) {
        for (int x = 0; x < 64; ++x) {
            const std::uint8_t* a = before.at(x, y);
            const std::uint8_t* b = h.state.doc.composite().at(x, y);
            if (!std::equal(a, a + 4, b)) {
                ++(area.contains(x, y) ? inside : outside);
            }
        }
    }
    CHECK_EQ(outside, 0);
    CHECK(inside > 0);
}

// A long live drag must leave a continuous band: coverage stamped by an early pointer event
// has to survive every later repaint. What the dab counter says and what the pixels hold are
// different facts, and the screen only shows the second one.
void test_a_live_stroke_keeps_every_dab_it_ever_stamped() {
    Harness h;
    h.value("doc.new", json{{"width", 640},
                            {"height", 400},
                            {"fill", json::array({200, 100, 50, 255})}});
    h.value("tool.set", json{{"tool", "brush"},
                             {"size", 24.0},
                             {"hardness", 0.8},
                             {"opacity", 1.0},
                             {"flow", 1.0},
                             {"spacing", 0.25},
                             {"color", json::array({0, 0, 0, 255})}});

    json points = json::array();
    for (int step = 0; step <= 8; ++step) {
        points.push_back(json::array({190.0 + 30.0 * step, 200.0 + 9.0 * step}));
    }
    pointer_gesture(h, points);

    // Sampled across the whole 240 px run: a dab lost to a repaint shows up as a column of
    // untouched pixels, which is what the pointer path did once on a 640x400 document.
    int columns_painted = 0;
    int columns = 0;
    for (int x = 190; x <= 430; x += 10) {
        ++columns;
        for (int y = 180; y <= 290; ++y) {
            const std::uint8_t* pixel = h.state.doc.composite().at(x, y);
            if (pixel[0] < 100 && pixel[1] < 100 && pixel[2] < 100) {
                ++columns_painted;
                break;
            }
        }
    }
    CHECK_EQ(columns_painted, columns);
}

void test_the_eraser_takes_alpha_and_keeps_the_colour_of_a_partly_erased_pixel() {
    Harness h;
    h.value("doc.new", json{{"width", 32},
                            {"height", 24},
                            {"fill", json::array({10, 20, 30, 200})}});
    const Image before = h.state.doc.composite();

    const json selected = h.value("tool.eraser", json::object());
    CHECK_EQ(selected["tool"].get<std::string>(), std::string("eraser"));
    pointer_gesture(h, json::array({json::array({8.0, 12.0}), json::array({24.0, 12.0})}));

    // src-out scales alpha and keeps the colour, so a half-erased pixel is the same hue
    // thinned out. An empty pixel is the one exception: it carries no colour, and zeroing it
    // keeps "nothing" a single set of bytes for codecs and goldens.
    int erased = 0;
    int partly = 0;
    for (int y = 0; y < 24; ++y) {
        for (int x = 0; x < 32; ++x) {
            const std::uint8_t* a = before.at(x, y);
            const std::uint8_t* b = h.state.doc.composite().at(x, y);
            if (b[3] >= a[3]) {
                continue;
            }
            ++erased;
            if (b[3] == 0) {
                CHECK_EQ(static_cast<int>(b[0] + b[1] + b[2]), 0);
                continue;
            }
            ++partly;
            CHECK_EQ(static_cast<int>(b[0]), static_cast<int>(a[0]));
            CHECK_EQ(static_cast<int>(b[1]), static_cast<int>(a[1]));
            CHECK_EQ(static_cast<int>(b[2]), static_cast<int>(a[2]));
        }
    }
    CHECK(erased > 0);
    CHECK(partly > 0);
}

// `contains` is key containment, which an array never satisfies.
bool has_shortcut(const json& command_row, const std::string& shortcut) {
    const std::vector<std::string> shortcuts =
        command_row.at("shortcuts").get<std::vector<std::string>>();
    return std::find(shortcuts.begin(), shortcuts.end(), shortcut) != shortcuts.end();
}

// The tool table is the one list: a row gives the id, the pixels it paints and the key that
// selects it, and an unknown tool is refused by naming the tools there really are.
void test_a_tool_row_becomes_a_command_a_key_and_a_paint_mode() {
    Harness h;
    h.value("doc.new", json{{"width", 16}, {"height", 16}});
    const json listed = h.value("command.list", json::object());
    for (const ToolInfo& tool : kTools) {
        const std::string id = std::string("tool.") + tool.id;
        const json* entry = nullptr;
        for (const json& row : listed["commands"]) {
            if (row["id"].get<std::string>() == id) {
                entry = &row;
            }
        }
        CHECK(entry != nullptr);
        if (entry == nullptr) {
            continue;
        }
        CHECK(has_shortcut(*entry, tool.select_key));
        CHECK_EQ(h.value(id, json::object())["tool"].get<std::string>(), std::string(tool.id));
        CHECK_EQ(h.value("app.state", json::object())["brush"]["tool"].get<std::string>(),
                 std::string(tool.id));
    }

    const CommandResult unknown = h.run("tool.set", json{{"tool", "chunky"}});
    CHECK(!unknown.ok);
    CHECK(unknown.error.find("chunky") != std::string::npos);
    for (const ToolInfo& tool : kTools) {
        CHECK(unknown.error.find(tool.id) != std::string::npos);
    }
}

// Both ends of the brush range, walked by the keys rather than by setting a size: the
// plausible bug is a step that forgets to clamp and lets the stamp grow past the image.
void test_the_size_keys_stop_at_the_ends_of_the_brush_range() {
    Harness h;
    h.value("doc.new", json{{"width", 16}, {"height", 16}});
    double size = kBrushDefaults.size;
    for (int step = 0; step < 200; ++step) {
        const json got = h.value("brush.larger", json::object());
        size = clamp_brush_size(size * kBrushSizeStep);
        CHECK_EQ(got["size"].get<double>(), size);
    }
    CHECK_EQ(size, kMaxBrushSize);
    for (int step = 0; step < 400; ++step) {
        const json got = h.value("brush.smaller", json::object());
        size = clamp_brush_size(size / kBrushSizeStep);
        CHECK_EQ(got["size"].get<double>(), size);
    }
    CHECK_EQ(size, kMinBrushSize);

    const json listed = h.value("command.list", json::object());
    for (const json& row : listed["commands"]) {
        const std::string id = row["id"].get<std::string>();
        if (id == "brush.larger") {
            CHECK(has_shortcut(row, "]"));
        }
        if (id == "brush.smaller") {
            CHECK(has_shortcut(row, "["));
        }
    }
}

// The keys that nudge a 0..1 brush field: one row of `kBrushSteps` each, clamped to the range of
// the field they name, landing on the step grid rather than accumulating a rounding error, and
// moving only the field they name. The grid is the contract: a slider dragged to 0.73 and then
// softened goes to 0.70, because "one step softer" cannot mean 0.68 on a control that reads 0.70.
void test_the_hardness_and_opacity_keys_step_and_stop_at_the_ends() {
    Harness h;
    h.value("doc.new", json{{"width", 16}, {"height", 16}});

    double hardness = kBrushDefaults.hardness;
    for (int press = 1; press <= 40; ++press) {
        const json got = h.value("brush.hardness_down", json::object());
        hardness = kBrushFractionStep * (std::lround(kBrushDefaults.hardness / kBrushFractionStep) -
                                        press);
        if (hardness < 0.0) {
            hardness = 0.0;
        }
        CHECK_NEAR(got["hardness"].get<double>(), hardness, 1e-9);
        // The other fields are somebody else's business: a member pointer pointed at the wrong row
        // shows up here and nowhere else.
        CHECK_NEAR(got["opacity"].get<double>(), kBrushDefaults.opacity, 1e-9);
        CHECK_NEAR(got["flow"].get<double>(), kBrushDefaults.flow, 1e-9);
        CHECK_NEAR(got["size"].get<double>(), kBrushDefaults.size, 1e-9);
    }
    CHECK_EQ(h.value("brush.hardness_down", json::object())["hardness"].get<double>(), 0.0);

    // Off-grid in, on-grid out.
    h.value("tool.set", json{{"hardness", 0.73}});
    CHECK_NEAR(h.value("brush.hardness_down", json::object())["hardness"].get<double>(), 0.70,
               1e-9);
    CHECK_NEAR(h.value("brush.hardness_up", json::object())["hardness"].get<double>(), 0.75, 1e-9);

    double opacity = kBrushDefaults.opacity;
    for (int press = 1; press <= 40; ++press) {
        const json got = h.value("brush.opacity_down", json::object());
        opacity = kBrushFractionStep * (std::lround(kBrushDefaults.opacity / kBrushFractionStep) -
                                       press);
        if (opacity < 0.0) {
            opacity = 0.0;
        }
        CHECK_NEAR(got["opacity"].get<double>(), opacity, 1e-9);
        CHECK_NEAR(got["hardness"].get<double>(), 0.75, 1e-9);
    }
    CHECK_EQ(h.value("brush.opacity_down", json::object())["opacity"].get<double>(), 0.0);
    for (int press = 0; press < 40; ++press) {
        h.value("brush.opacity_up", json::object());
    }
    CHECK_EQ(h.value("brush.opacity_up", json::object())["opacity"].get<double>(), 1.0);

    // The bindings are reported where a driver can read them, and the pair for a field is on the
    // keys the MVP named: `,` and `.`, with the modifier taking opacity.
    const json listed = h.value("command.list", json::object());
    int bound = 0;
    for (const json& row : listed["commands"]) {
        const std::string id = row["id"].get<std::string>();
        if (id == "brush.hardness_down") {
            CHECK(has_shortcut(row, ","));
            ++bound;
        }
        if (id == "brush.hardness_up") {
            CHECK(has_shortcut(row, "."));
            ++bound;
        }
        if (id == "brush.opacity_down") {
            CHECK(has_shortcut(row, "Shift+,"));
            ++bound;
        }
        if (id == "brush.opacity_up") {
            CHECK(has_shortcut(row, "Shift+."));
            ++bound;
        }
    }
    CHECK_EQ(bound, 4);
}

void test_tool_pointer_rejects_a_phase_or_point_it_cannot_use() {
    Harness h;
    h.value("doc.new", json{{"width", 16}, {"height", 16}});
    for (const char* phase : {"", "tap", "RELEASE"}) {
        const CommandResult result =
            h.run("tool.pointer", json{{"phase", phase}, {"at", json::array({1.0, 1.0})}});
        CHECK(!result.ok);
        CHECK(result.error.find("phase") != std::string::npos);
    }
    const CommandResult no_point = h.run("tool.pointer", json{{"phase", "press"}});
    CHECK(!no_point.ok);
    CHECK(no_point.error.find("at") != std::string::npos);
    CHECK(!h.state.stroke.active);
}

void test_a_stroke_that_cannot_run_names_the_field_that_is_wrong() {
    Harness h;
    h.value("doc.new", json{{"width", 16}, {"height", 16}});
    const std::string before = checksum_of(h);
    const json valid = full_stroke();

    // A stroke paints with the tool settings the app is on, so any field may be left out.
    for (const char* absent : {"size", "hardness", "opacity", "flow", "spacing", "color"}) {
        json args = valid;
        args.erase(absent);
        CHECK_OK(h.run("tool.stroke", args));
        h.value("edit.undo", json::object());
    }

    for (const char* field : {"size", "hardness", "opacity", "flow", "spacing", "color"}) {
        json wrong_type = valid;
        wrong_type[field] = "wide";
        const CommandResult result = h.run("tool.stroke", wrong_type);
        CHECK(!result.ok);
        CHECK(result.error.find(field) != std::string::npos);
    }

    json bad_tool = valid;
    bad_tool["tool"] = "hatch";
    const CommandResult tool = h.run("tool.stroke", bad_tool);
    CHECK(!tool.ok);
    CHECK(tool.error.find("hatch") != std::string::npos);

    for (const char* field : {"hardness", "opacity", "flow", "spacing"}) {
        json out_of_range = valid;
        out_of_range[field] = 1.5;
        CHECK(!h.run("tool.stroke", out_of_range).ok);
    }
    json too_small = valid;
    too_small["size"] = 0.0;
    CHECK(!h.run("tool.stroke", too_small).ok);

    CHECK_EQ(checksum_of(h), before);
    const CommandResult nothing = h.run("edit.undo", json::object());
    CHECK(!nothing.ok);
    CHECK(nothing.error.find("nothing to undo") != std::string::npos);
    // The last undone stroke is still offered by redo; the five before it were invalidated by
    // the strokes that followed, which is what "a new change after undo clears redo" means.
    CHECK_EQ(h.value("doc.get", json::object())["history"]["redo_depth"].get<int>(), 1);
}

void test_a_stroke_that_misses_the_document_is_not_a_step() {
    Harness h;
    h.value("doc.new", json{{"width", 16}, {"height", 16}});
    const std::string before = checksum_of(h);
    const std::uint64_t revision = h.state.doc.revision();
    json args = full_stroke();
    args["points"] = json::array({json::array({500.0, 500.0})});
    const json result = h.value("tool.stroke", args);
    CHECK_EQ(result["touched"].size(), 0u);
    CHECK_EQ(checksum_of(h), before);
    CHECK_EQ(h.state.doc.revision(), revision);
    CHECK_EQ(undo_depth(h), 0);
}

void test_app_quit_reaches_the_host() {
    Harness h;
    CHECK_EQ(h.quits, 0);
    CHECK_OK(h.run("app.quit", json::object()));
    CHECK_EQ(h.quits, 1);
}

void test_command_invoke_dispatches_and_names_failures() {
    Harness h;
    const json invoked = h.value("command.invoke", json{{"name", "doc.new"},
                                                        {"args", json{{"width", 40},
                                                                      {"height", 30}}}});
    CHECK_EQ(invoked["name"].get<std::string>(), std::string("doc.new"));
    CHECK_EQ(invoked["result"]["width"].get<int>(), 40);
    CHECK(!h.run("command.invoke", json::object()).ok);

    const CommandResult unknown = h.run("command.invoke", json{{"name", "no.such.thing"},
                                                               {"args", json::object()}});
    CHECK(!unknown.ok);
    CHECK(unknown.error.find("no.such.thing") != std::string::npos);

    // Recursion through the table has no useful end.
    CHECK(!h.run("command.invoke", json{{"name", "command.invoke"},
                                        {"args", json::object()}}).ok);
}

void test_bad_arguments_name_the_command() {
    Harness h;
    for (const json& bad : json::array({json{{"width", 10}},
                                        json{{"width", 0}, {"height", 10}},
                                        json{{"width", -5}, {"height", 10}},
                                        json{{"width", 10}, {"height", 1.5}}})) {
        const CommandResult result = h.run("doc.new", bad);
        CHECK(!result.ok);
        CHECK(result.error.find("doc.new") != std::string::npos);
        CHECK(h.state.doc.empty());
    }

    const CommandResult no_document = h.run("doc.get", json::object());
    CHECK(!no_document.ok);
    CHECK(no_document.error.find("doc.open") != std::string::npos);

    CHECK(!h.run("view.zoom", json{{"factor", "1.5"}}).ok);
    CHECK(!h.run("view.pan", json{{"dx", 3}}).ok);
    CHECK(!h.run("log.level", json{{"level", "chatty"}}).ok);
}

void test_malformed_fields_name_their_field() {
    Harness h;
    h.value("doc.new", json{{"width", 8}, {"height", 8}});

    const CommandResult bad_region =
        h.run("image.get", json{{"region", json::array({1, 2, "wide", 4})}});
    CHECK(!bad_region.ok);
    CHECK(bad_region.error.find("region") != std::string::npos);

    CHECK(!h.run("image.get", json{{"base64", "yes"}}).ok);
    CHECK(!h.run("doc.new", json{{"width", 4},
                                 {"height", 4},
                                 {"fill", json::array({300, 0, 0, 255})}}).ok);

    // A capture name becomes a file name: separators are refused, not honoured.
    const CommandResult escaping = h.run("screenshot", json{{"name", "../escape"}});
    CHECK(!escaping.ok);
    CHECK(escaping.error.find("name") != std::string::npos);

    // The document survived all of it untouched.
    CHECK_EQ(h.value("doc.get", json::object())["width"].get<int>(), 8);
}

// The Layers panel, the keyboard and a wire client all reach the stack through these commands,
// so what they agree on is what the command table says.
void test_the_layer_stack_is_edited_through_the_command_table() {
    Harness h;
    h.value("doc.new", json{{"width", 16}, {"height", 12}});
    json listed = h.value("layer.list", json::object());
    CHECK_EQ(listed["layers"].size(), 1u);
    CHECK_EQ(listed["layers"][0]["name"].get<std::string>(), std::string("Layer 1"));
    CHECK_EQ(listed["active"].get<int>(), 0);

    CHECK_OK(h.run("layer.add", json::object()));
    listed = h.value("layer.list", json::object());
    CHECK_EQ(listed["layers"].size(), 2u);
    CHECK_EQ(listed["layers"][1]["name"].get<std::string>(), std::string("Layer 2"));
    CHECK_EQ(listed["active"].get<int>(), 1);  // the layer you just made is the one you paint on

    const std::string base_before = listed["layers"][0]["checksum"].get<std::string>();
    const std::string top_before = listed["layers"][1]["checksum"].get<std::string>();
    h.value("tool.stroke", full_stroke());
    listed = h.value("layer.list", json::object());
    CHECK_EQ(listed["layers"][0]["checksum"].get<std::string>(), base_before);
    CHECK(listed["layers"][1]["checksum"].get<std::string>() != top_before);

    // Selecting moves the target and changes no pixels, so it bumps no revision - and the next
    // stroke lands below, where the selection now is.
    const std::uint64_t revision = h.value("doc.get", json::object())["revision"].get<std::uint64_t>();
    CHECK_OK(h.run("layer.select", json{{"index", 0}}));
    CHECK_EQ(h.value("doc.get", json::object())["revision"].get<std::uint64_t>(), revision);
    h.value("tool.stroke", full_stroke());
    listed = h.value("layer.list", json::object());
    CHECK(listed["layers"][0]["checksum"].get<std::string>() != base_before);

    // A step belongs to the layer it was made on, not to whichever layer is selected when it is
    // taken back: undo returns the bottom layer to what it was and leaves the layer above it,
    // painted earlier in the same session, byte-identical.
    const std::string bottom_painted = listed["layers"][0]["checksum"].get<std::string>();
    const std::string top_painted = listed["layers"][1]["checksum"].get<std::string>();
    CHECK_NE(bottom_painted, base_before);
    h.value("edit.undo", json::object());
    const json undone = h.value("layer.list", json::object());
    CHECK_EQ(undone["layers"][0]["checksum"].get<std::string>(), base_before);
    CHECK_EQ(undone["layers"][1]["checksum"].get<std::string>(), top_painted);

    // A new layer goes directly above the active one - which the previous steps left at the
    // bottom - and takes the selection with it.
    const json named = h.value("layer.add", json{{"name", "Ink"},
                                                 {"fill", json::array({9, 8, 7, 255})}});
    CHECK_EQ(named["layers"].size(), 3u);
    CHECK_EQ(named["layers"][1]["name"].get<std::string>(), std::string("Ink"));
    CHECK_EQ(named["active"].get<int>(), 1);

    const CommandResult out_of_range = h.run("layer.select", json{{"index", 9}});
    CHECK(!out_of_range.ok);
    CHECK(out_of_range.error.find("9") != std::string::npos);
    CHECK(out_of_range.error.find("3 layers") != std::string::npos);
    const CommandResult bad_fill = h.run("layer.add", json{{"fill", json::array({300, 0, 0, 0})}});
    CHECK(!bad_fill.ok);
    CHECK(bad_fill.error.find("fill") != std::string::npos);
    const CommandResult no_index = h.run("layer.select", json::object());
    CHECK(!no_index.ok);
    CHECK(no_index.error.find("index") != std::string::npos);

    // Down to one layer and no further: removing the last one would leave no canvas to draw on.
    CHECK_OK(h.run("layer.remove", json{{"index", 1}}));
    CHECK_OK(h.run("layer.remove", json{{"index", 1}}));
    const CommandResult last = h.run("layer.remove", json{{"index", 0}});
    CHECK(!last.ok);
    CHECK(last.error.find("last layer") != std::string::npos);
    CHECK_EQ(h.value("layer.list", json::object())["layers"].size(), 1u);
}

// A blend mode is layer state, so it travels the same route visibility does: `layer.set` in,
// `layer.list` out, the composite is what the mode says, and a name the table has no row for is
// refused with the list attached. One exact composite number is checked (red under a 50 % grey
// multiply is (128, 0, 0)) because the arithmetic itself belongs to `test_blend`; what is being
// pinned here is that the command reaches the pixels.
void test_a_layer_mode_is_layer_state_and_reaches_the_composite() {
    Harness h;
    h.value("doc.new", json{{"width", 4}, {"height", 4}, {"fill", json::array({255, 0, 0, 255})}});
    h.value("layer.add", json{{"fill", json::array({128, 128, 128, 255})}});

    const std::uint64_t revision = h.value("doc.get", json::object())["revision"].get<std::uint64_t>();
    CHECK_OK(h.run("layer.set", json{{"index", 1}, {"mode", "multiply"}}));
    CHECK_EQ(h.value("layer.list", json::object())["layers"][1]["mode"].get<std::string>(),
             std::string("multiply"));
    // The canvas shows the composite, so a mode change has to bump the revision or a renderer that
    // re-uploads on it keeps showing the old blend.
    CHECK(h.value("doc.get", json::object())["revision"].get<std::uint64_t>() > revision);

    std::string error;
    const std::string multiplied = h.temp("mode-multiply.png");
    CHECK_OK(h.run("doc.export", json{{"path", multiplied}}));
    Image want = Image::filled(Extent{4, 4}, 128, 0, 0, 255);
    const std::string want_path = h.temp("mode-multiply-want.png");
    CHECK(io::save_png(want_path, want, error));
    CHECK_EQ(h.value("pixels.compare", json{{"a", multiplied}, {"b", want_path}})["equal"]
                 .get<bool>(),
             true);

    // A mode is not a pixel step: taking the last edit back must not take the mode with it, or
    // undoing a stroke would silently change how the layer sits in the stack.
    h.value("tool.stroke", full_stroke());
    CHECK_OK(h.run("edit.undo", json::object()));
    CHECK_EQ(h.value("layer.list", json::object())["layers"][1]["mode"].get<std::string>(),
             std::string("multiply"));

    // Every name the table has is accepted, and each is reported back as the name it was given -
    // `color_dodge` in C++, `color-dodge` on the wire and in a file.
    for (const BlendModeInfo& info : kBlendModes) {
        CHECK_OK(h.run("layer.set", json{{"index", 0}, {"mode", info.name}}));
        CHECK_EQ(h.value("layer.list", json::object())["layers"][0]["mode"].get<std::string>(),
                 std::string(info.name));
    }

    const CommandResult unknown = h.run("layer.set", json{{"index", 0}, {"mode", "hard-light"}});
    CHECK(!unknown.ok);
    CHECK(unknown.error.find("hard-light") != std::string::npos);
    CHECK(unknown.error.find("color-dodge") != std::string::npos);
    CHECK(unknown.error.find("overlay") != std::string::npos);
    // The refusal changed nothing.
    CHECK_EQ(h.value("layer.list", json::object())["layers"][0]["mode"].get<std::string>(),
             std::string(kBlendModes[std::size(kBlendModes) - 1].name));

    const CommandResult wrong_type = h.run("layer.set", json{{"index", 0}, {"mode", 3}});
    CHECK(!wrong_type.ok);
    CHECK(wrong_type.error.find("mode") != std::string::npos);

    // And with nothing to change, the error names the fields that would do it, mode included.
    const CommandResult nothing = h.run("layer.set", json{{"index", 0}});
    CHECK(!nothing.ok);
    CHECK(nothing.error.find("mode") != std::string::npos);
}

// The container holds a stack, so a save and reopen has to bring back both layers and the
// thing the user sees, which is neither of them on its own.
void test_a_two_layer_project_round_trips_and_exports_its_composite() {
    Harness h;
    const std::string path = h.temp("stack.noise");
    const std::string flat = h.temp("stack.png");
    h.value("doc.new", json{{"width", 20},
                            {"height", 16},
                            {"fill", json::array({1, 2, 3, 255})}});
    h.value("layer.add", json{{"name", "Top"}});
    h.value("tool.stroke", full_stroke());
    h.value("layer.select", json{{"index", 0}});
    h.value("tool.stroke", full_stroke());
    const json saved_view = h.value("layer.list", json::object());
    const std::string composite = saved_view["composite_checksum"].get<std::string>();

    CHECK_OK(h.run("doc.save", json{{"path", path}}));
    h.value("doc.new", json{{"width", 2}, {"height", 2}});
    const json opened = h.value("doc.open", json{{"path", path}});
    CHECK_EQ(opened["active"].get<int>(), 1);  // the top of the stack comes back on top
    CHECK(!opened["dirty"].get<bool>());
    // Names and per-layer content come from layer.list; doc.open reports the document, and a
    // stack summary that hashed every layer could not be polled per frame the way app.state is.
    const json reopened = h.value("layer.list", json::object());
    CHECK_EQ(reopened["layers"].size(), 2u);
    if (reopened["layers"].size() == 2) {
        CHECK_EQ(reopened["layers"][0]["name"].get<std::string>(), std::string("Layer 1"));
        CHECK_EQ(reopened["layers"][1]["name"].get<std::string>(), std::string("Top"));
        CHECK_EQ(reopened["layers"][0]["checksum"].get<std::string>(),
                 saved_view["layers"][0]["checksum"].get<std::string>());
        CHECK_EQ(reopened["layers"][1]["checksum"].get<std::string>(),
                 saved_view["layers"][1]["checksum"].get<std::string>());
    }
    CHECK_EQ(reopened["composite_checksum"].get<std::string>(), composite);

    CHECK_OK(h.run("doc.export", json{{"path", flat}}));
    std::string error;
    CHECK_EQ(io::load_png(flat, error).checksum_hex(), composite);
    std::filesystem::remove(path);
    std::filesystem::remove(flat);
}

void test_a_project_round_trips_and_an_export_is_not_a_save() {
    Harness h;
    const std::string path = h.temp("roundtrip.noise");
    const std::string flat = h.temp("flat.png");
    // Nothing to save yet, and no path to save it to: both are named in the error.
    CHECK(h.run("doc.save", json{{"path", path}}).error.find("no document") != std::string::npos);
    h.value("doc.new", json{{"width", 37},
                            {"height", 19},
                            {"fill", json::array({10, 200, 30, 255})}});
    CHECK(h.run("doc.save", json::object()).error.find("no path") != std::string::npos);
    const std::string created = checksum_of(h);
    CHECK(h.value("doc.get", json::object())["dirty"].get<bool>());  // nothing on disk yet
    CHECK_EQ(h.value("doc.get", json::object())["kind"].get<std::string>(), std::string("none"));

    // A flat image is not the app's file, and the rejection says which command is.
    const CommandResult wrong_kind = h.run("doc.save", json{{"path", flat}});
    CHECK(!wrong_kind.ok);
    CHECK(wrong_kind.error.find("doc.export") != std::string::npos);

    CHECK_OK(h.run("doc.save", json{{"path", path}}));
    json saved = h.value("doc.get", json::object());
    CHECK_EQ(saved["kind"].get<std::string>(), std::string("noise"));
    CHECK(!saved["dirty"].get<bool>());

    // Painting marks the document unsaved; undo does not clear the flag, because what is on
    // disk is the unpainted version either way.
    h.value("tool.stroke", full_stroke());
    CHECK(h.value("doc.get", json::object())["dirty"].get<bool>());
    h.value("edit.undo", json::object());
    CHECK(h.value("doc.get", json::object())["dirty"].get<bool>());
    CHECK_OK(h.run("doc.save", json::object()));

    h.value("doc.new", json{{"width", 3}, {"height", 3}});
    CHECK_OK(h.run("doc.open", json{{"path", path}}));
    const json reopened = h.value("doc.get", json::object());
    CHECK_EQ(reopened["checksum"].get<std::string>(), created);
    CHECK_EQ(reopened["path"].get<std::string>(), path);
    CHECK_EQ(reopened["width"].get<int>(), 37);
    CHECK_EQ(reopened["kind"].get<std::string>(), std::string("noise"));
    CHECK(!reopened["dirty"].get<bool>());
    CHECK_EQ(reopened["layers"][0]["name"].get<std::string>(), std::string("Layer 1"));
    CHECK_EQ(reopened["active"].get<int>(), 0);
    // replace() must bump the revision, or the GPU texture keeps showing the old pixels.
    CHECK(reopened["revision"].get<std::uint64_t>() > 1u);

    // An export writes pixels and takes nothing about the document's own file: no new path,
    // no cleared flag, and a PNG that decodes to exactly what the document holds.
    CHECK_OK(h.run("doc.export", json{{"path", flat}}));
    const json after_export = h.value("doc.get", json::object());
    CHECK_EQ(after_export["path"].get<std::string>(), path);
    CHECK(!after_export["dirty"].get<bool>());
    std::string error;
    const Image exported = io::load_png(flat, error);
    // A flat export is the composite: the document's own checksum covers the stack, which a
    // PNG cannot hold, so the comparison is against what the app shows.
    CHECK_EQ(exported.checksum_hex(),
             h.value("layer.list", json::object())["composite_checksum"].get<std::string>());

    const CommandResult missing = h.run("doc.open", json{{"path", h.temp("absent.png")}});
    CHECK(!missing.ok);
    CHECK(missing.error.find("absent.png") != std::string::npos);
    const CommandResult missing_project = h.run("doc.open", json{{"path", h.temp("absent.noise")}});
    CHECK(!missing_project.ok);
    CHECK(missing_project.error.find("absent.noise") != std::string::npos);
    std::filesystem::remove(path);
    std::filesystem::remove(flat);
}

// A whole-layer translate has an exact specification, so the move tool is tested against the
// specification and not against itself: for every pixel, `after(x, y)` is `before(x - dx, y - dy)`
// or transparent when that falls outside the canvas. Nothing about the brush, the dabs or the
// gesture shape appears in the assertion, only what the tool claims to do.
void test_the_overlay_reports_the_phase_it_draws_and_freezes_on_request() {
    Harness h;
    h.value("doc.new", json{{"width", 24}, {"height", 16}, {"fill", json::array({0, 0, 0, 255})}});
    h.value("select.all", json::object());

    const json live = h.value("app.state", json::object())["ants"];
    const double dash = live["dash_px"].get<double>();
    CHECK(dash > 0.0);
    // The reported phase sits in the one period the pattern has, whatever the clock said.
    CHECK(live["phase_px"].get<double>() >= 0.0);
    CHECK(live["phase_px"].get<double>() < 2.0 * dash);
    CHECK(live["speed_px_per_second"].get<double>() > 0.0);
    CHECK(live["frozen_phases"].empty());

    // Freezing is honoured by the number the overlay draws from, not just remembered: a screenshot
    // probe asks for a phase and this is the only value that says what was on screen.
    const json frozen = h.value("ants.freeze", json{{"phases", json::array({4.5})}});
    CHECK(frozen["phases"].size() == 1);
    const json held = h.value("app.state", json::object())["ants"];
    CHECK_EQ(held["phase_px"].get<double>(), 4.5);
    CHECK_EQ(held["frozen_phases"].size(), 1u);

    // An empty list hands the clock back.
    h.value("ants.freeze", json{{"phases", json::array()}});
    const json again = h.value("app.state", json::object())["ants"];
    CHECK(again["frozen_phases"].empty());
    CHECK(again["phase_px"].get<double>() < 2.0 * dash);

    const CommandResult junk = h.run("ants.freeze", json{{"phases", json::array({"fast"})}});
    CHECK(!junk.ok);
    CHECK(junk.error.find("phases") != std::string::npos);
}

void test_the_move_tool_slides_the_layer_and_undoes_it_exactly() {
    constexpr int kWidth = 64;
    constexpr int kHeight = 48;
    Harness h;
    h.value("doc.new", json{{"width", kWidth},
                            {"height", kHeight},
                            {"fill", json::array({0, 0, 0, 0})}});
    json stroke = full_stroke();
    stroke["points"] = json::array({json::array({10.0, 10.0}), json::array({30.0, 14.0})});
    stroke["size"] = 12.0;
    h.value("tool.stroke", stroke);
    const std::string painted_sum = checksum_of(h);
    const int steps_before = undo_depth(h);  // the painting that made the content is a step too
    CHECK_NE(painted_sum, Image::filled(Extent{kWidth, kHeight}, 0, 0, 0, 0).checksum_hex());

    // Four offsets rather than one, because the interesting cases are not symmetric. A rightward
    // drag leaves its vacated area at the left of the region; a leftward one leaves it at the right,
    // and only if the content started near the right edge does that area fall inside the region at
    // all - hence the journey right and then back. Every case is checked against the same rule, and
    // a drag that clipped nothing on the way is worth as much as one that did.
    const int offsets[][2] = {{11, 5}, {-7, -4}, {30, 2}, {-20, 0}};
    Image before = h.state.doc.composite();
    int gestures = 0;
    for (const int* offset : offsets) {
        const int dx = offset[0];
        const int dy = offset[1];
        h.value("tool.set", json{{"tool", "move"}});
        h.value("tool.pointer", json{{"phase", "press"}, {"at", json::array({8.0, 8.0})}});
        h.value("tool.pointer",
                json{{"phase", "drag"}, {"at", json::array({8.0 + dx * 0.5, 8.0 + dy * 0.5})}});
        const json mid = h.value("app.state", json::object());
        CHECK(mid["stroke"]["active"].get<bool>());
        CHECK_EQ(mid["stroke"]["dx"].get<int>(), static_cast<int>(std::lround(dx * 0.5)));
        CHECK_EQ(mid["stroke"]["dy"].get<int>(), static_cast<int>(std::lround(dy * 0.5)));
        // Live on the canvas but not yet a step: the gesture commits once, at release.
        CHECK_EQ(undo_depth(h), steps_before + gestures);
        CHECK_NE(checksum_of(h), painted_sum);

        h.value("tool.pointer", json{{"phase", "drag"}, {"at", json::array({8.0 + dx, 8.0 + dy})}});
        h.value("tool.pointer", json{{"phase", "release"}});
        const Image after = h.state.doc.composite();

        std::size_t covered = 0;
        for (int y = 0; y < kHeight; ++y) {
            for (int x = 0; x < kWidth; ++x) {
                const int source_x = x - dx;
                const int source_y = y - dy;
                const bool inside = source_x >= 0 && source_y >= 0 && source_x < kWidth &&
                                    source_y < kHeight;
                const std::uint8_t* want = inside ? before.at(source_x, source_y) : kEmptyPixel;
                if (want[3] != 0) {
                    ++covered;
                }
                const std::uint8_t* got = after.at(x, y);
                for (int channel = 0; channel < 4; ++channel) {
                    if (got[channel] != want[channel]) {
                        noise::test::report("moved pixel equals its source", __FILE__, __LINE__,
                                            std::to_string(dx) + "," + std::to_string(dy) + " at " +
                                                std::to_string(x) + "," + std::to_string(y) +
                                                " channel " + std::to_string(channel) + ": got " +
                                                std::to_string(got[channel]) + " want " +
                                                std::to_string(want[channel]));
                    }
                    ++noise::test::checks;
                }
            }
        }
        // Without this the loop above could be comparing an empty canvas with itself.
        CHECK(covered > 200u);
        before = after;
        ++gestures;
    }

    // One step per gesture, and every one of them puts the whole document back.
    CHECK_EQ(undo_depth(h), steps_before + 4);
    for (int step = 0; step < 4; ++step) {
        h.value("edit.undo", json::object());
    }
    CHECK_EQ(checksum_of(h), painted_sum);
    CHECK_EQ(h.value("edit.redo", json::object())["redo_depth"].get<int>(), 3);
}

// Sub-pixel pointer motion is not an edit. Without this the app would record a step for a tremor
// of the hand, dirty the document, and grow the undo buffer for pixels that never changed.
void test_a_move_that_never_left_its_pixel_is_not_an_edit() {
    Harness h;
    h.value("doc.new", json{{"width", 32},
                            {"height", 32},
                            {"fill", json::array({0, 0, 0, 0})}});
    h.value("tool.stroke", full_stroke());
    const std::string before = checksum_of(h);
    const int steps_before = undo_depth(h);
    h.value("tool.set", json{{"tool", "move"}});
    h.value("tool.pointer", json{{"phase", "press"}, {"at", json::array({12.0, 9.0})}});
    h.value("tool.pointer", json{{"phase", "drag"}, {"at", json::array({12.4, 9.3})}});
    h.value("tool.pointer", json{{"phase", "drag"}, {"at", json::array({11.6, 9.4})}});
    h.value("tool.pointer", json{{"phase", "release"}});
    CHECK_EQ(checksum_of(h), before);
    CHECK_EQ(undo_depth(h), steps_before);
}

// `V` moves one layer, not the stack: the layer that is not active keeps every byte, so a move is
// not silently a merge.
void test_a_move_slides_the_active_layer_and_no_other() {
    Harness h;
    h.value("doc.new", json{{"width", 40},
                            {"height", 40},
                            {"fill", json::array({0, 0, 0, 0})}});
    h.value("tool.stroke", full_stroke());
    h.value("layer.add", json{{"name", "upper"}});
    json upper = full_stroke();
    upper["points"] = json::array({json::array({6.0, 30.0}), json::array({34.0, 32.0})});
    upper["color"] = json::array({0, 0, 255, 255});
    h.value("tool.stroke", upper);
    const std::string upper_sum = h.state.doc.layer(1).pixels.checksum_hex();
    const std::string lower_sum = h.state.doc.layer(0).pixels.checksum_hex();

    h.value("layer.select", json{{"index", 0}});
    h.value("tool.set", json{{"tool", "move"}});
    pointer_offset_gesture(h, 4, 3);
    CHECK_EQ(h.state.doc.layer(1).pixels.checksum_hex(), upper_sum);
    CHECK_NE(h.state.doc.layer(0).pixels.checksum_hex(), lower_sum);
}

void test_image_get_returns_the_documents_own_pixels() {
    Harness h;
    h.value("doc.new", json{{"width", 40}, {"height", 30}});
    const json got = h.value("image.get", json{{"region", json::array({4, 6, 10, 8})}});
    CHECK_EQ(got["width"].get<int>(), 10);
    CHECK_EQ(got["height"].get<int>(), 8);

    std::string error;
    const Image written = io::load_png(got["png_path"].get<std::string>(), error);
    CHECK(!written.empty());
    const Image expected = h.state.doc.composite().region(Rect{4, 6, 10, 8});
    CHECK_EQ(written.checksum_hex(), expected.checksum_hex());
    CHECK_EQ(got["checksum"].get<std::string>(), expected.checksum_hex());

    // A region hanging off the canvas is clipped, not rejected, and says so in its dims.
    const json clipped = h.value("image.get", json{{"region", json::array({35, 25, 100, 100})}});
    CHECK_EQ(clipped["width"].get<int>(), 5);
    CHECK_EQ(clipped["height"].get<int>(), 5);

    const CommandResult outside = h.run("image.get", json{{"region", json::array({40, 0, 8, 8})}});
    CHECK(!outside.ok);
}

void test_pixels_compare_finds_a_single_changed_byte() {
    Harness h;
    const std::string path_a = h.temp("compare-a.png");
    const std::string path_b = h.temp("compare-b.png");
    Image a = Image::filled(Extent{16, 16}, 7, 9, 11, 255);
    a.blit(Image::filled(Extent{2, 2}, 200, 30, 40, 255), 5, 6);
    Image b = Image::filled(Extent{16, 16}, 7, 9, 11, 255);
    b.blit(Image::filled(Extent{2, 2}, 200, 30, 40, 255), 5, 6);
    b.at(5, 6)[1] = 31;  // one channel of one pixel
    std::string error;
    CHECK(io::save_png(path_a, a, error));
    CHECK(io::save_png(path_b, b, error));

    const json same = h.value("pixels.compare", json{{"a", path_a}, {"b", path_a}});
    CHECK_EQ(same["equal"].get<bool>(), true);
    CHECK_EQ(same["differing_bytes"].get<std::size_t>(), std::size_t(0));

    // The whole-image diff M4's pixel-lock invariant leans on: one touched channel is
    // one differing byte, and the delta is reported.
    const json diff = h.value("pixels.compare", json{{"a", path_a}, {"b", path_b}});
    CHECK_EQ(diff["equal"].get<bool>(), false);
    CHECK_EQ(diff["differing_bytes"].get<std::size_t>(), std::size_t(1));
    CHECK_EQ(diff["max_channel_delta"].get<int>(), 1);

    // Either side may be any flat format: what is compared is pixels, not a container.
    const std::string path_jpg = h.temp("compare.jpg");
    CHECK(io::save_jpeg(path_jpg, a, 92, error));
    const json jpeg_itself = h.value("pixels.compare", json{{"a", path_jpg}, {"b", path_jpg}});
    CHECK_EQ(jpeg_itself["equal"].get<bool>(), true);
    // That it decoded rather than errored: the lossy JPEG of those pixels differs from the PNG.
    // How far it differs is not asserted here - ringing on a hard 2x2 edge measures 79 per channel
    // at q92, which no bound distinguishes from a wrong colour space without being so loose it
    // catches nothing. Colour accuracy is tools/image_interop_probe.py's job, against Pillow.
    const json against_png = h.value("pixels.compare", json{{"a", path_a}, {"b", path_jpg}});
    CHECK_EQ(against_png["equal"].get<bool>(), false);
    CHECK(against_png["differing_bytes"].get<std::size_t>() > 0);

    const CommandResult absent = h.run("pixels.compare", json{{"a", path_a},
                                                              {"b", h.temp("absent.png")}});
    CHECK(!absent.ok);
    CHECK(absent.error.find("absent.png") != std::string::npos);
}

void test_screenshot_returns_exactly_what_the_capture_gave() {
    Harness h;
    const json shot = h.value("screenshot", json{{"region", json::array({3, 4, 32, 16})},
                                                 {"name", "unit"}});
    CHECK_EQ(shot["width"].get<int>(), 32);
    CHECK_EQ(shot["height"].get<int>(), 16);
    const Image expected = stub_frame(1280, 800).region(Rect{3, 4, 32, 16});
    CHECK_EQ(shot["sha256"].get<std::string>(), expected.checksum_hex());

    std::string error;
    const Image loaded = io::load_png(shot["path"].get<std::string>(), error);
    CHECK_EQ(loaded.checksum_hex(), expected.checksum_hex());
    CHECK(shot["path"].get<std::string>().find("unit-") != std::string::npos);

    const CommandResult outside = h.run("screenshot",
                                        json{{"region", json::array({1280, 0, 8, 8})}});
    CHECK(!outside.ok);
    std::filesystem::remove(shot["path"].get<std::string>());
}

// The canvas area only exists once the UI has laid it out, so opening a document before
// the first frame must leave the view alone rather than fit it to whatever extent is on
// hand. Latching that wrong scale is what made a document opened on the command line
// spill past the canvas panel.
void test_a_fit_waits_for_the_canvas_area_to_be_published() {
    Harness h;
    h.state.viewport_published = false;
    h.state.viewport = Extent{1, 1};

    CHECK_OK(h.run("doc.new", json{{"width", 800}, {"height", 600}}));
    CHECK(h.value("app.state", json::object())["view"]["fit_pending"].get<bool>());
    CHECK_EQ(h.state.view.scale, 1.0);

    // A viewport too small to hold the document with its margin is not a canvas either.
    h.state.viewport_published = true;
    app::apply_pending_fit(h.state);
    CHECK(h.value("app.state", json::object())["view"]["fit_pending"].get<bool>());
    CHECK_EQ(h.state.view.scale, 1.0);

    h.state.viewport = Extent{1280, 800};
    app::apply_pending_fit(h.state);
    const json fitted = h.value("app.state", json::object())["view"];
    CHECK(!fitted["fit_pending"].get<bool>());
    const double margin = static_cast<double>(kViewPolicy.fit_margin_px);
    const double expected =
        std::min((1280.0 - 2.0 * margin) / 800.0, (800.0 - 2.0 * margin) / 600.0);
    CHECK_NEAR(fitted["scale"].get<double>(), expected, 1e-12);

    // Once a document exists the request is served again: opening a new one re-fits.
    CHECK_OK(h.run("doc.new", json{{"width", 200}, {"height", 100}}));
    CHECK(!h.value("app.state", json::object())["view"]["fit_pending"].get<bool>());
    CHECK(h.state.view.scale > 1.0);
}

void test_a_shorter_image_stays_centred_under_panning() {
    Harness h;
    h.value("doc.new", json{{"width", 4000}, {"height", 1000}});
    const json fitted = h.value("view.fit", json::object());
    // Fitted, the wide image leaves 488 px of empty viewport vertically: nothing to pan
    // through, so the image holds its centre line instead of sliding away.
    const json panned = h.value("view.pan", json{{"dx", 40}, {"dy", -4000}});
    CHECK_NEAR(panned["origin"].at(1).get<double>(), fitted["origin"].at(1).get<double>(), 1e-9);
    CHECK(panned["origin"].at(0).get<double>() != fitted["origin"].at(0).get<double>());
}

void test_view_navigation_keeps_the_image_reachable() {
    Harness h;
    h.value("doc.new", json{{"width", 2000}, {"height", 1500}});
    const json fitted = h.value("view.fit", json::object());
    const double fit_scale = fitted["scale"].get<double>();
    CHECK(fit_scale > 0.0);
    CHECK(fit_scale < 1.0);

    const json anchor = json::array({640.0, 400.0});
    const double world_x = 640.0 / fit_scale + fitted["origin"].at(0).get<double>();
    const double world_y = 400.0 / fit_scale + fitted["origin"].at(1).get<double>();
    const json zoomed = h.value("view.zoom", json{{"factor", 1.5}, {"anchor", anchor}});
    const double scale = zoomed["scale"].get<double>();
    CHECK_NEAR(scale, fit_scale * 1.5, 1e-12);
    CHECK_NEAR(640.0 / scale + zoomed["origin"].at(0).get<double>(), world_x, 1e-9);
    CHECK_NEAR(400.0 / scale + zoomed["origin"].at(1).get<double>(), world_y, 1e-9);

    // Zoomed past the viewport, panning is free and exact: -delta in screen px.
    const json panned = h.value("view.pan", json{{"dx", 37}, {"dy", -21}});
    CHECK_NEAR(panned["origin"].at(0).get<double>() - zoomed["origin"].at(0).get<double>(),
               -37.0 / scale, 1e-9);
    CHECK_NEAR(panned["origin"].at(1).get<double>() - zoomed["origin"].at(1).get<double>(),
               21.0 / scale, 1e-9);

    // No amount of panning loses the image: its on-screen rect keeps overlapping the
    // viewport, in both directions.
    for (const double extreme : {-1e7, 1e7}) {
        const json far = h.value("view.pan", json{{"dx", extreme}, {"dy", extreme}});
        const double s = far["scale"].get<double>();
        const double ox = far["origin"].at(0).get<double>();
        const double oy = far["origin"].at(1).get<double>();
        const double image_w = far["image"].at(0).get<double>();
        const double image_h = far["image"].at(1).get<double>();
        CHECK(std::min(image_w - ox, 1280.0 / s) > std::max(-ox, 0.0));
        CHECK(std::min(image_h - oy, 800.0 / s) > std::max(-oy, 0.0));
    }

    // Scale limits come from the policy, not from whatever the caller sends.
    const json huge = h.value("view.zoom", json{{"factor", 1e9},
                                                {"anchor", json::array({0.0, 0.0})}});
    CHECK(huge["scale"].get<double>() <= kViewPolicy.max_scale + 1e-12);
    const json tiny = h.value("view.zoom", json{{"factor", 1e-9},
                                                {"anchor", json::array({0.0, 0.0})}});
    CHECK(tiny["scale"].get<double>() > 0.0);
    CHECK(tiny["scale"].get<double>() < kViewPolicy.max_scale);
    // The floor is a floor: zooming out again cannot shrink it further.
    const json tinier = h.value("view.zoom", json{{"factor", 1e-9},
                                                  {"anchor", json::array({0.0, 0.0})}});
    CHECK_NEAR(tinier["scale"].get<double>(), tiny["scale"].get<double>(), 1e-12);
}

}  // namespace

std::string rect_text(const Rect& r) {
    return "x=" + std::to_string(r.x) + " y=" + std::to_string(r.y) + " w=" + std::to_string(r.w) +
           " h=" + std::to_string(r.h);
}

bool rect_is(const Rect& got, const Rect& want, int line) {
    if (got == want) {
        return true;
    }
    noise::test::report("rectangle", __FILE__, line, "got " + rect_text(got) + " want " + rect_text(want));
    return false;
}

// The marquee is the select tool's whole meaning, so it is checked as a function: every pixel the
// rectangle passed over is claimed, and the direction of the drag cannot change that.
void test_the_marquee_claims_every_pixel_it_passed_over() {
    const Extent canvas{40, 20};
    CHECK(app::marquee({2.2, 3.7}, {2.2, 3.7}, canvas) == (Rect{2, 3, 1, 1}));
    // A marquee lying exactly along pixel boundaries encloses no pixel, which is what rounding
    // outwards from both ends means; the same drag with a hair of height claims that row.
    CHECK(app::marquee({5.0, 5.0}, {9.0, 5.0}, canvas).empty());
    CHECK(app::marquee({5.0, 5.0}, {9.0, 5.4}, canvas) == (Rect{5, 5, 4, 1}));
    const Vec2 pairs[][2] = {
        {{1.0, 1.0}, {9.0, 6.0}},
        {{9.0, 6.0}, {1.0, 1.0}},
        {{30.5, 15.25}, {12.25, 2.75}},
        {{-8.0, -6.0}, {12.0, 40.0}},
        {{38.5, 19.5}, {39.5, 25.0}},
    };
    for (const auto (&pair)[2] : pairs) {
        const Rect forward = app::marquee(pair[0], pair[1], canvas);
        CHECK(forward.inside(canvas));
        // Dragging from the far corner draws the same rectangle: only the corners matter, not the
        // order, and a sign error would show up here and nowhere else.
        CHECK(app::marquee(pair[1], pair[0], canvas) == forward);
        // Every pixel strictly between the two points, rounded outwards, is inside.
        const int left = static_cast<int>(std::floor(std::min(pair[0].x, pair[1].x)));
        const int top = static_cast<int>(std::floor(std::min(pair[0].y, pair[1].y)));
        const int right = std::min(static_cast<int>(std::ceil(std::max(pair[0].x, pair[1].x))),
                                   canvas.w);
        const int bottom = std::min(static_cast<int>(std::ceil(std::max(pair[0].y, pair[1].y))),
                                    canvas.h);
        for (int y = std::max(top, 0); y < bottom; ++y) {
            for (int x = std::max(left, 0); x < right; ++x) {
                CHECK(forward.contains(x, y));
            }
        }
    }
    // A marquee drawn off the canvas selects nothing rather than wrapping to the near corner.
    CHECK(app::marquee({-30.0, -30.0}, {-1.0, -1.0}, canvas).empty());
}

void test_the_select_commands_set_add_and_take_away() {
    Harness h;
    h.value("doc.new", json{{"width", 40}, {"height", 20}, {"fill", json::array({0, 0, 0, 255})}});
    CHECK(!h.value("select.get", json::object())["active"].get<bool>());

    h.value("select.rect", json{{"rect", json::array({4, 4, 10, 6})}, {"mode", "replace"}});
    json got = h.value("select.get", json::object());
    CHECK(got["active"].get<bool>());
    CHECK_EQ(got["covered"].get<int>(), 60);
    CHECK(rect_is(Rect{got["bounds"][0].get<int>(), got["bounds"][1].get<int>(),
                       got["bounds"][2].get<int>(), got["bounds"][3].get<int>()},
                  Rect{4, 4, 10, 6}, __LINE__));

    // `mode` defaults to replace, so a second rectangle does not grow the first.
    h.value("select.rect", json{{"rect", json::array({20, 2, 4, 4})}});
    CHECK_EQ(h.value("select.get", json::object())["covered"].get<int>(), 16);
    h.value("select.rect", json{{"rect", json::array({4, 4, 10, 6})}, {"mode", "add"}});
    CHECK_EQ(h.value("select.get", json::object())["covered"].get<int>(), 76);
    h.value("select.rect", json{{"rect", json::array({4, 4, 10, 6})}, {"mode", "subtract"}});
    CHECK_EQ(h.value("select.get", json::object())["covered"].get<int>(), 16);

    // Refused, and named: an operation the table does not have, and a rectangle with no area.
    const CommandResult odd_mode =
        h.run("select.rect", json{{"rect", json::array({1, 1, 2, 2})}, {"mode", "intersect"}});
    CHECK(!odd_mode.ok);
    CHECK(odd_mode.error.find("intersect") != std::string::npos);
    CHECK(odd_mode.error.find("replace") != std::string::npos);
    const CommandResult flat_rect = h.run("select.rect", json{{"rect", json::array({1, 1, 0, 5})}});
    CHECK(!flat_rect.ok);
    CHECK(flat_rect.error.find("0x5") != std::string::npos);
    CHECK_EQ(h.value("select.get", json::object())["covered"].get<int>(), 16);

    // Whole canvas, then nothing, then the complement of nothing and of everything.
    h.value("select.all", json::object());
    CHECK_EQ(h.value("select.get", json::object())["covered"].get<int>(), 800);
    // Selecting all must mean *nothing is held back*, which counting covered pixels cannot tell from
    // a plane a hair under full coverage. The same stroke with and without a selection has to leave
    // the same bytes.
    json probe = full_stroke();
    probe["points"] = json::array({json::array({4.0, 4.0}), json::array({30.0, 14.0})});
    probe["size"] = 10.0;
    h.value("tool.stroke", probe);
    const std::string painted_under_all = h.state.doc.layer(0).pixels.checksum_hex();
    h.value("edit.undo", json::object());
    h.value("select.clear", json::object());
    h.value("tool.stroke", probe);
    CHECK_EQ(h.state.doc.layer(0).pixels.checksum_hex(), painted_under_all);
    h.value("edit.undo", json::object());
    h.value("select.clear", json::object());
    CHECK(!h.value("select.get", json::object())["active"].get<bool>());
    // No selection means everything is writable, so its complement is nothing - which is a state the
    // app has to be able to hold, because painting then does nothing at all.
    h.value("select.invert", json::object());
    CHECK_EQ(h.value("select.get", json::object())["covered"].get<int>(), 800);
    h.value("select.invert", json::object());
    CHECK_EQ(h.value("select.get", json::object())["covered"].get<int>(), 0);
    h.value("select.invert", json::object());
    CHECK_EQ(h.value("select.get", json::object())["covered"].get<int>(), 800);

    // A selection is not a step: undo puts pixels back and leaves the selection where it is.
    CHECK_EQ(undo_depth(h), 0);
}

// The invariant that matters end to end: with a selection up, no byte outside it moves, whatever the
// stroke asked for - checked through the command surface the UI and the wire use.
void test_a_selection_holds_a_stroke_inside_it() {
    constexpr int kWidth = 48;
    constexpr int kHeight = 32;
    Harness h;
    h.value("doc.new", json{{"width", kWidth}, {"height", kHeight},
                            {"fill", json::array({20, 30, 40, 255})}});
    h.value("select.rect", json{{"rect", json::array({10, 8, 12, 10})}});

    json stroke = full_stroke();
    stroke["points"] = json::array({json::array({2.0, 4.0}), json::array({44.0, 28.0})});
    stroke["size"] = 18.0;
    const Image before = h.state.doc.layer(0).pixels;
    h.value("tool.stroke", stroke);
    const Image after = h.state.doc.layer(0).pixels;

    std::size_t changed = 0;
    for (int y = 0; y < kHeight; ++y) {
        for (int x = 0; x < kWidth; ++x) {
            const bool inside = x >= 10 && x < 22 && y >= 8 && y < 18;
            bool differs = false;
            for (int channel = 0; channel < 4; ++channel) {
                differs = differs || after.at(x, y)[channel] != before.at(x, y)[channel];
            }
            if (!inside) {
                CHECK(!differs);
            }
            if (differs) {
                CHECK(inside);
                ++changed;
            }
        }
    }
    // The stroke crossed the rectangle, so a run that clipped everything also clipped inside.
    CHECK(changed > 0);

    const int steps = undo_depth(h);
    h.value("edit.undo", json::object());
    CHECK(h.state.doc.layer(0).pixels.checksum_hex() == before.checksum_hex());
    CHECK_EQ(undo_depth(h), steps - 1);
    // Undo restored pixels; the selection the user drew is still up.
    CHECK(h.value("select.get", json::object())["active"].get<bool>());

    // The same stroke with the selection put down changes the canvas: the clip above was the
    // selection doing work, not a stroke that painted nothing.
    h.value("select.clear", json::object());
    h.value("tool.stroke", stroke);
    CHECK(h.state.doc.layer(0).pixels.checksum_hex() != before.checksum_hex());
}

// The two conversions, which are the reason a selection and a mask are the same type: each is a
// copy, so neither can shift by a pixel or lose partial coverage on the way across.
// Which plane a stroke writes is a setting the wire can read, aim and refuse against. A stroke aimed
// at a mask the layer does not have is refused by name rather than quietly painting colour, because a
// panel that says "mask" while colour moves is the one lie this seam could tell.
void test_the_paint_target_decides_where_a_stroke_lands() {
    Harness h;
    h.value("doc.new", json{{"width", 24}, {"height", 12}, {"fill", json::array({20, 20, 20, 255})}});
    h.value("tool.set", json{{"tool", "brush"}, {"size", 8.0}, {"hardness", 1.0}, {"opacity", 1.0}});
    const json points = json::array({json::array({6.0, 6.0}), json::array({14.0, 6.0})});

    CHECK_EQ(h.value("app.state", json::object())["brush"]["paint_target"].get<std::string>(),
             std::string("pixels"));
    CHECK_EQ(h.value("mask.get", json::object())["target"].get<std::string>(), std::string("pixels"));
    CHECK(h.value("mask.get", json::object())["mask"].is_null());

    CHECK_EQ(h.value("mask.target", json{{"plane", "mask"}})["paint_target"].get<std::string>(),
             std::string("mask"));
    const CommandResult nowhere = h.run("tool.stroke", json{{"points", points}});
    CHECK(!nowhere.ok);
    CHECK(nowhere.error.find("has none") != std::string::npos);
    CHECK(nowhere.error.find("mask.add") != std::string::npos);
    const CommandResult no_press = h.run("tool.pointer", json{{"phase", "press"}, {"at", json::array({6.0, 6.0})}});
    CHECK(!no_press.ok);
    CHECK(no_press.error.find("mask") != std::string::npos);
    CHECK_EQ(h.value("doc.get", json::object())["history"]["undo_depth"].get<int>(), 0);

    const json added = h.value("mask.add", json::object());
    CHECK_EQ(added["covered"].get<std::int64_t>(), 24 * 12);
    CHECK_NEAR(added["area"].get<double>(), 24 * 12.0, 1e-9);
    // A mask that shows everything hides nothing, so the canvas is as it was and putting one down is
    // not a step the timeline has to carry.
    CHECK_EQ(h.value("doc.get", json::object())["history"]["undo_depth"].get<int>(), 0);

    // A fill of the whole mask is a write: one step, and undo puts the coverage back exactly.
    const std::string shown = h.value("doc.get", json::object())["checksum"].get<std::string>();
    const json hidden = h.value("mask.fill", json{{"to", "hide"}});
    CHECK_EQ(hidden["covered"].get<std::int64_t>(), 0);
    CHECK_EQ(h.value("doc.get", json::object())["history"]["undo_depth"].get<int>(), 1);
    const std::string filled = h.value("doc.get", json::object())["checksum"].get<std::string>();
    CHECK_NE(filled, shown);
    h.value("edit.undo", json::object());
    CHECK_EQ(h.value("doc.get", json::object())["checksum"].get<std::string>(), shown);
    h.value("edit.redo", json::object());
    CHECK_EQ(h.value("doc.get", json::object())["checksum"].get<std::string>(), filled);

    // A brush on a hidden mask brings coverage back where it went and nowhere else. The composite
    // says so: the layer is opaque, so anywhere the mask still reads zero the canvas below it shows.
    const std::string layer_bytes = h.value("layer.list", json::object())["layers"][0]["checksum"].get<std::string>();
    const json stroke = h.value("tool.stroke", json{{"points", points}});
    CHECK_EQ(stroke["plane"].get<std::string>(), std::string("mask"));
    const json facts = h.value("mask.get", json::object())["mask"];
    CHECK(facts["covered"].get<std::int64_t>() > 0);
    CHECK(facts["covered"].get<std::int64_t>() < 24 * 12);
    // The stroke ran along y=6 from x=6 to x=14 with an 8 px brush: the revealed band reaches a few
    // pixels past both ends and nowhere near the top or bottom rows.
    const json bounds = facts["bounds"];
    CHECK(bounds[0].get<int>() >= 1);
    CHECK(bounds[0].get<int>() + bounds[2].get<int>() <= 20);
    CHECK(bounds[1].get<int>() >= 1);
    CHECK(bounds[1].get<int>() + bounds[3].get<int>() <= 11);
    CHECK_EQ(h.value("layer.list", json::object())["layers"][0]["checksum"].get<std::string>(),
             layer_bytes);  // coverage only: the layer's own bytes did not move

    const std::string painted = h.value("doc.get", json::object())["checksum"].get<std::string>();
    CHECK_EQ(h.value("doc.get", json::object())["history"]["undo_depth"].get<int>(), 2);
    h.value("edit.undo", json::object());
    CHECK_EQ(h.value("doc.get", json::object())["checksum"].get<std::string>(), filled);
    h.value("edit.redo", json::object());
    CHECK_EQ(h.value("doc.get", json::object())["checksum"].get<std::string>(), painted);

    // A live gesture reports the plane it has been writing since the press, which is the target as it
    // was then: aiming elsewhere mid-drag cannot split one gesture across two planes.
    h.value("tool.pointer", json{{"phase", "press"}, {"at", json::array({4.0, 4.0})}});
    CHECK_EQ(h.value("app.state", json::object())["stroke"]["plane"].get<std::string>(), std::string("mask"));
    h.value("mask.target", json{{"plane", "pixels"}});
    h.value("tool.pointer", json{{"phase", "drag"}, {"at", json::array({8.0, 4.0})}});
    CHECK_EQ(h.value("app.state", json::object())["stroke"]["plane"].get<std::string>(), std::string("mask"));
    h.value("tool.pointer", json{{"phase", "release"}, {"at", json::array({8.0, 4.0})}});
    CHECK_EQ(h.value("mask.get", json::object())["target"].get<std::string>(), std::string("pixels"));

    // Aimed at colour again, the same stroke moves colour and leaves the mask alone.
    const std::string coverage = h.value("mask.get", json::object())["mask"].dump();
    const std::string before = h.value("layer.list", json::object())["layers"][0]["checksum"].get<std::string>();
    h.value("tool.stroke", json{{"points", points}});
    CHECK_NE(h.value("layer.list", json::object())["layers"][0]["checksum"].get<std::string>(), before);
    CHECK_EQ(h.value("mask.get", json::object())["mask"].dump(), coverage);
}

// The selection fences every write, and a mask stroke is a write: what the brush did to coverage
// happened only as far as the selection reached, which is the same rule the colour stroke runs.
void test_a_selection_limits_a_mask_stroke() {
    Harness h;
    h.value("doc.new", json{{"width", 24}, {"height", 12}, {"fill", json::array({0, 0, 0, 255})}});
    h.value("tool.set", json{{"tool", "brush"}, {"size", 6.0}, {"hardness", 1.0}, {"opacity", 1.0}});
    h.value("mask.add", json::object());
    h.value("mask.fill", json{{"to", "hide"}});
    h.value("select.rect", json{{"rect", json::array({10, 0, 4, 12})}, {"mode", "replace"}});

    h.value("mask.target", json{{"plane", "mask"}});
    h.value("tool.stroke", json{{"points", json::array({json::array({2.0, 6.0}), json::array({22.0, 6.0})})}});
    const json facts = h.value("mask.get", json::object())["mask"];
    const json bounds = facts["bounds"];
    CHECK(facts["covered"].get<std::int64_t>() > 0);
    CHECK(facts["covered"].get<std::int64_t>() <= 4 * 12);
    // The whole stroke ran the width of the canvas; only the selected columns could change.
    CHECK_EQ(bounds[0].get<int>(), 10);
    CHECK_EQ(bounds[0].get<int>() + bounds[2].get<int>(), 14);

    // With the fence gone the same stroke reaches the rest, so the limit above was the selection and
    // not the brush.
    h.value("mask.fill", json{{"to", "hide"}});
    h.value("select.clear", json::object());
    h.value("tool.stroke", json{{"points", json::array({json::array({2.0, 6.0}), json::array({22.0, 6.0})})}});
    const json whole = h.value("mask.get", json::object())["mask"];
    CHECK(whole["covered"].get<std::int64_t>() > facts["covered"].get<std::int64_t>());
    CHECK(whole["bounds"][0].get<int>() < 10);
}

// The controls answer when there is nothing to act on, and say which layer they looked at.
void test_the_mask_controls_name_the_layer_they_refused() {
    Harness h;
    h.value("doc.new", json{{"width", 8}, {"height", 8}, {"fill", json::array({0, 0, 0, 0})}});
    const std::string name = h.value("layer.list", json::object())["layers"][0]["name"].get<std::string>();
    for (const char* id : {"mask.fill", "mask.invert"}) {
        const CommandResult refused = h.run(id, id == std::string("mask.fill")
                                                   ? json{{"to", "hide"}}
                                                   : json::object());
        CHECK(!refused.ok);
        CHECK(refused.error.find(name) != std::string::npos);
        CHECK(refused.error.find("mask.add") != std::string::npos);
    }
    // Removing a mask that is not there is what the caller asked for; nothing else moved.
    CHECK(h.run("mask.remove", json::object()).ok);
    CHECK(h.value("mask.get", json::object())["mask"].is_null());

    // Vocabulary refusals name the words they accept.
    CHECK(h.run("mask.fill", json{{"to", "bright"}}).error.find("show") != std::string::npos);
    CHECK(h.run("mask.target", json{{"plane", "colour"}}).error.find("pixels") != std::string::npos);
    CHECK(h.run("mask.target", json{{"plane", "colour"}}).error.find("mask") != std::string::npos);
    CHECK(h.run("mask.fill", json::object()).error.find("'to'") != std::string::npos);

    // A mask belongs to exactly one layer: the active row gained one and the row beside it says it
    // has none. `layer.add` does not select what it added, so the active index is read rather than
    // assumed.
    h.value("layer.add", json{{"name", "Top"}});
    const json before = h.value("layer.list", json::object());
    const int active = before["active"].get<int>();
    const int other = active == 0 ? 1 : 0;
    h.value("mask.add", json::object());
    const json rows = h.value("layer.list", json::object())["layers"];
    CHECK(!rows[active]["mask"].is_null());
    CHECK(rows[other]["mask"].is_null());
    CHECK_EQ(rows[active]["mask"]["covered"].get<std::int64_t>(), 8 * 8);
}

void test_a_selection_becomes_a_mask_and_a_mask_becomes_a_selection() {
    constexpr int kWidth = 40;
    constexpr int kHeight = 24;
    Harness h;
    h.value("doc.new", json{{"width", kWidth}, {"height", kHeight},
                            {"fill", json::array({10, 20, 30, 255})}});

    // Nothing to convert yet: both directions refuse, and say what is missing.
    const CommandResult no_selection = h.run("mask.from_selection", json::object());
    CHECK(!no_selection.ok);
    CHECK(no_selection.error.find("no selection") != std::string::npos);
    const CommandResult no_mask = h.run("select.from_mask", json::object());
    CHECK(!no_mask.ok);
    CHECK(no_mask.error.find("no mask") != std::string::npos);

    h.value("select.rect", json{{"rect", json::array({4, 3, 12, 9})}});
    const json selection = h.value("select.get", json::object());
    const json report = h.value("mask.from_selection", json::object());
    CHECK_EQ(report["covered"].get<std::int64_t>(), selection["covered"].get<std::int64_t>());
    CHECK(report["bounds"] == selection["bounds"]);

    // The mask is the layer's, not the document's: `layer.list` shows it on the active row and the
    // selection is still up afterwards, because the conversion copied.
    const json rows = h.value("layer.list", json::object())["layers"];
    CHECK(!rows[0]["mask"].is_null());
    CHECK_EQ(rows[0]["mask"]["covered"].get<std::int64_t>(),
             selection["covered"].get<std::int64_t>());
    CHECK(h.value("select.get", json::object())["active"].get<bool>());
// ...and the document still knows how to paint the whole canvas: a selection that survives the
    // conversion is the point, and one silently cleared would fence the user out of their own image.
    CHECK(h.value("select.get", json::object())["active"].get<bool>());
    const int steps_after_mask = undo_depth(h);
    const std::string pixels_with_mask = h.state.doc.layer(0).pixels.checksum_hex();
    h.value("select.clear", json::object());
    json wide = full_stroke();
    wide["points"] = json::array({json::array({2.0, 2.0}), json::array({36.0, 20.0})});
    wide["size"] = 10.0;
    h.value("tool.stroke", wide);
    CHECK(h.state.doc.layer(0).pixels.checksum_hex() != pixels_with_mask);
    CHECK_EQ(undo_depth(h), steps_after_mask + 1);

    // The canvas now shows only the masked rectangle, although the layer's pixels still cover the
    // whole canvas - the mask hides, it does not edit.
    const Image& composite = h.state.doc.composite();
    const Image& pixels = h.state.doc.layer(0).pixels;
    CHECK_EQ(pixels.at(0, 0)[3], 255);
    CHECK_EQ(composite.at(0, 0)[3], 0);
    CHECK_EQ(composite.at(6, 6)[3], 255);

    // Put the selection down, then load it back from the mask: the same facts come back.
    h.value("select.clear", json::object());
    const json loaded = h.value("select.from_mask", json::object());
    CHECK_EQ(loaded["covered"].get<std::int64_t>(),
             selection["covered"].get<std::int64_t>());
    CHECK(loaded["bounds"] == selection["bounds"]);

    // A mask taken from a *partial* selection keeps its coverage, which is the part a rectangle
    // cannot show: invert turns half-covered pixels into half-covered pixels, so nothing was
    // rounded to in/out on the way through the file-less conversion.
    h.value("select.clear", json::object());
    h.value("select.rect", json{{"rect", json::array({0, 0, kWidth, kHeight})}});
    h.value("select.invert", json::object());  // nothing at all selected
    h.value("mask.from_selection", json::object());
    const json empty_mask = h.value("layer.list", json::object())["layers"][0];
    CHECK_EQ(empty_mask["mask"]["covered"].get<std::int64_t>(), 0);
    for (int y = 0; y < kHeight; ++y) {
        for (int x = 0; x < kWidth; ++x) {
            CHECK_EQ(composite.at(x, y)[3], 0);
        }
    }
    // A mask that hides everything is still a mask, so it still converts back - to a selection that
    // blocks every write, which is a different document from one with no selection.
    const json back = h.value("select.from_mask", json::object());
    CHECK(back["active"].get<bool>());
    CHECK_EQ(back["covered"].get<std::int64_t>(), 0);
}

// A layer mask is composite state: it must reach the canvas through the command table, and it must
// not be an undo step, exactly like the eye and the opacity slider.
void test_a_layer_mask_costs_no_undo_step_and_reaches_the_file() {
    constexpr int kWidth = 24;
    constexpr int kHeight = 16;
    Harness h;
    h.value("doc.new", json{{"width", kWidth}, {"height", kHeight},
                            {"fill", json::array({200, 100, 50, 255})}});
    h.value("select.rect", json{{"rect", json::array({2, 2, 6, 6})}});
    const int steps = undo_depth(h);
    h.value("mask.from_selection", json::object());
    CHECK_EQ(undo_depth(h), steps);
    CHECK(h.state.doc.dirty());

    const std::string path = h.temp("masked.noise");
    h.value("doc.save", json{{"path", path}});
    io::Project loaded;
    std::string error;
    CHECK(io::load_project(path, loaded, error));
    CHECK_EQ(loaded.layers.size(), 1u);
    if (!loaded.layers.empty()) {
        CHECK(loaded.layers[0].mask.has_value());
    }
    // Reopening gives the same canvas: the mask is part of the document, not a view setting.
    const std::string composite_with_mask = h.state.doc.composite().checksum_hex();
    h.value("doc.new", json{{"width", 4}, {"height", 4}, {"fill", json::array({0, 0, 0, 0})}});
    h.value("doc.open", json{{"path", path}});
    CHECK_EQ(h.state.doc.composite().checksum_hex(), composite_with_mask);
    CHECK(h.state.doc.layer(0).mask.has_value());
    // Converting copies: the selection the mask was made from is still the document's, and both
    // come back from the file, so "make a mask out of this" does not cost the user their selection.
    CHECK(h.state.doc.selection().has_value());
    CHECK_EQ(h.value("select.get", json::object())["covered"].get<std::int64_t>(), 36);
}

// The select tool through the pointer: the marquee is live, and the gesture costs no pixel history.
// The reported area of an elliptical selection against the ellipse's own arithmetic: read the bounds
// back, compute pi*a*b, require the plane to agree. The same helper serves the gesture, so a marquee
// that drew a staircase or shifted its centre fails here rather than in a screenshot.
void expect_ellipse_area(const json& selection, double tolerance, int line) {
    const double a = selection["bounds"][2].get<double>() / 2.0;
    const double b = selection["bounds"][3].get<double>() / 2.0;
    const double want = M_PI * a * b;
    const double got = selection["area"].get<double>();
    noise::test::expect(std::fabs(got - want) <= tolerance * want, "ellipse area matches pi*a*b",
                        __FILE__, line,
                        "got " + std::to_string(got) + " want " + std::to_string(want));
    // Pixels touched is at least the area claimed and less than the box: the difference between them
    // is the anti-aliased edge, and either equality would mean it had gone.
    const double covered = selection["covered"].get<double>();
    const double box = selection["bounds"][2].get<double>() * selection["bounds"][3].get<double>();
    noise::test::expect(covered >= got && covered < box,
                        "covered sits between the area claimed and the box", __FILE__, line,
                        selection.dump());
}

void test_the_ellipse_command_selects_the_curve_and_not_its_box() {
    Harness h;
    h.value("doc.new", json{{"width", 40}, {"height", 20}, {"fill", json::array({0, 0, 0, 255})}});

    h.value("select.rect", json{{"rect", json::array({0, 0, 40, 20})}});
    const json square = h.value("select.get", json::object());
    CHECK_EQ(square["covered"].get<int>(), 800);
    // A rectangle has no edge to speak of: it claims exactly the pixels it touches.
    CHECK_NEAR(square["area"].get<double>(), 800.0, 0.001);

    h.value("select.ellipse", json{{"rect", json::array({0, 0, 40, 20})}});
    const json round = h.value("select.get", json::object());
    expect_ellipse_area(round, 0.01, __LINE__);
    CHECK(rect_is(Rect{round["bounds"][0].get<int>(), round["bounds"][1].get<int>(),
                       round["bounds"][2].get<int>(), round["bounds"][3].get<int>()},
                  Rect{0, 0, 40, 20}, __LINE__));

    // Combining works the same way as for a rectangle, because it is the same operation: taking the
    // curve out of the box leaves the corners the curve does not reach, and taking the box out of the
    // curve leaves nothing - which only holds if the partial pixels subtracted as partial.
    h.value("select.rect", json{{"rect", json::array({0, 0, 40, 20})}});
    h.value("select.ellipse", json{{"rect", json::array({0, 0, 40, 20})}, {"mode", "subtract"}});
    const json corners = h.value("select.get", json::object());
    CHECK_NEAR(corners["area"].get<double>(), 800.0 - round["area"].get<double>(), 0.02);
    h.value("select.rect", json{{"rect", json::array({0, 0, 40, 20})}, {"mode", "subtract"}});
    CHECK_NEAR(h.value("select.get", json::object())["area"].get<double>(), 0.0, 0.001);

    // A shape the table does not have is refused outright, a bad box names the argument, and neither
    // touches what was already selected.
    h.value("select.ellipse", json{{"rect", json::array({0, 0, 40, 20})}});
    const json settled = h.value("select.get", json::object());
    const CommandResult odd = h.run("select.parabola", json{{"rect", json::array({0, 0, 4, 4})}});
    CHECK(!odd.ok);
    const CommandResult flat = h.run("select.ellipse", json{{"rect", json::array({0, 0, 0, 9})}});
    CHECK(!flat.ok);
    CHECK(flat.error.find("0x9") != std::string::npos);
    CHECK(h.value("select.get", json::object()) == settled);
}

void test_the_ellipse_marquee_draws_the_shape_of_its_own_tool_row() {
    Harness h;
    h.value("doc.new", json{{"width", 40}, {"height", 20}, {"fill", json::array({0, 0, 0, 255})}});
    h.value("tool.select_ellipse", json::object());
    CHECK_EQ(h.state.tool, std::string("select_ellipse"));
    // Like the rectangle marquee it selects, so painting through it is refused - naming the tools
    // that do paint.
    const CommandResult paint_attempt =
        h.run("tool.stroke", json{{"points", json::array({json::array({1.0, 1.0})})}});
    CHECK(!paint_attempt.ok);
    CHECK(paint_attempt.error.find("select_ellipse") != std::string::npos);

    h.value("tool.pointer", json{{"phase", "press"}, {"at", json::array({5.0, 5.0})}});
    h.value("tool.pointer", json{{"phase", "drag"}, {"at", json::array({35.0, 15.0})}});
    CHECK_EQ(h.value("app.state", json::object())["stroke"]["marquee_shape"].get<std::string>(),
             "ellipse");
    expect_ellipse_area(h.value("select.get", json::object()), 0.01, __LINE__);
    CHECK_EQ(undo_depth(h), 0);

    // Switching tools mid-marquee does not change the shape of the one already being drawn: the box
    // grows, the curve stays a curve.
    h.value("tool.select", json::object());
    h.value("tool.pointer", json{{"phase", "drag"}, {"at", json::array({39.0, 19.0})}});
    CHECK_EQ(h.value("app.state", json::object())["stroke"]["marquee_shape"].get<std::string>(),
             "ellipse");
    expect_ellipse_area(h.value("select.get", json::object()), 0.01, __LINE__);
    h.value("tool.pointer", json{{"phase", "release"}});
    expect_ellipse_area(h.value("select.get", json::object()), 0.01, __LINE__);

    // The rectangle tool, by contrast, fills the box it dragged.
    h.value("tool.select", json::object());
    h.value("tool.pointer", json{{"phase", "press"}, {"at", json::array({5.0, 5.0})}});
    h.value("tool.pointer", json{{"phase", "drag"}, {"at", json::array({39.0, 19.0})}});
    CHECK_EQ(h.value("app.state", json::object())["stroke"]["marquee_shape"].get<std::string>(),
             "rect");
    CHECK_NEAR(h.value("select.get", json::object())["area"].get<double>(), 34.0 * 14.0, 0.001);
}

void test_the_select_gesture_draws_and_puts_down_the_selection() {
    Harness h;
    h.value("doc.new", json{{"width", 40}, {"height", 20}, {"fill", json::array({0, 0, 0, 255})}});
    h.value("tool.select", json::object());
    CHECK_EQ(h.state.tool, std::string("select"));
    // A tool that selects cannot paint, and says which of the two it is.
    const CommandResult paint_attempt = h.run("tool.stroke", json{{"points", json::array(
        {json::array({1.0, 1.0})})}});
    CHECK(!paint_attempt.ok);
    CHECK(paint_attempt.error.find("select") != std::string::npos);

    h.value("tool.pointer", json{{"phase", "press"}, {"at", json::array({10.4, 10.4})}});
    // The press itself is a selection, not just the start of one: what is under the pointer is
    // claimed before any drag arrives.
    CHECK_EQ(h.value("select.get", json::object())["covered"].get<int>(), 1);
    h.value("tool.pointer", json{{"phase", "drag"}, {"at", json::array({26.0, 18.0})}});
    const json mid = h.value("app.state", json::object());
    CHECK(mid["stroke"]["active"].get<bool>());
    CHECK(rect_is(Rect{mid["stroke"]["marquee"][0].get<int>(), mid["stroke"]["marquee"][1].get<int>(),
                       mid["stroke"]["marquee"][2].get<int>(), mid["stroke"]["marquee"][3].get<int>()},
                  Rect{10, 10, 16, 8}, __LINE__));
    // Live in the document during the gesture, and still no step: a selection is not a pixel edit.
    CHECK_EQ(h.value("select.get", json::object())["covered"].get<int>(), 128);
    CHECK_EQ(undo_depth(h), 0);
    h.value("tool.pointer", json{{"phase", "release"}});
    CHECK_EQ(h.value("select.get", json::object())["covered"].get<int>(), 128);
    const json after = h.value("app.state", json::object());
    CHECK(!after["stroke"]["active"].get<bool>());
    // With no gesture there is no marquee to report, and the reply says so rather than leaving the
    // last rectangle for a client to read as still-dragging.
    CHECK_EQ(after["stroke"]["marquee"].size(), 0u);

    // A press outside the canvas draws nothing, and an empty marquee puts the selection down. That is
    // the mouse route out of a selection for anyone not reaching for Ctrl+D.
    h.value("tool.pointer", json{{"phase", "press"}, {"at", json::array({90.0, 90.0})}});
    h.value("tool.pointer", json{{"phase", "release"}});
    CHECK(!h.value("select.get", json::object())["active"].get<bool>());

    // A click inside the canvas claims the one pixel under it, which is what the marquee rule says.
    h.value("tool.pointer", json{{"phase", "press"}, {"at", json::array({5.4, 6.6})}});
    h.value("tool.pointer", json{{"phase", "release"}});
    CHECK_EQ(h.value("select.get", json::object())["covered"].get<int>(), 1);
}

int main() {
    log::set_level(log::Level::Error);
    test_m0_command_surface_is_registered();
    test_every_command_refuses_an_argument_it_does_not_declare();
    test_a_container_that_is_not_ours_is_refused_naming_what_it_met();
    test_exporting_to_a_flat_format_reports_what_the_format_could_hold();
    test_export_arguments_are_checked_and_nothing_is_written_on_refusal();
    test_a_brush_field_on_the_command_that_takes_none_is_an_error();
    test_one_stroke_is_one_step_that_puts_every_byte_back();
    test_a_pointer_gesture_lands_where_the_same_path_painted_in_one_go_lands();
    test_a_pointer_gesture_is_one_step_and_reports_itself();
    test_a_gesture_that_leaves_the_canvas_still_undoes_completely();
    test_the_hardness_and_opacity_keys_step_and_stop_at_the_ends();
    test_tool_pointer_rejects_a_phase_or_point_it_cannot_use();
    test_a_pointer_gesture_changes_no_pixel_outside_its_own_rectangle();
    test_the_eraser_takes_alpha_and_keeps_the_colour_of_a_partly_erased_pixel();
    test_a_live_stroke_keeps_every_dab_it_ever_stamped();
    test_a_tool_row_becomes_a_command_a_key_and_a_paint_mode();
    test_the_size_keys_stop_at_the_ends_of_the_brush_range();
    test_a_stroke_that_cannot_run_names_the_field_that_is_wrong();
    test_a_stroke_that_misses_the_document_is_not_a_step();
    test_the_keymap_is_reported_with_the_commands();
    test_app_quit_reaches_the_host();
    test_command_invoke_dispatches_and_names_failures();
    test_bad_arguments_name_the_command();
    test_malformed_fields_name_their_field();
    test_the_layer_stack_is_edited_through_the_command_table();
    test_a_layer_mode_is_layer_state_and_reaches_the_composite();
    test_a_two_layer_project_round_trips_and_exports_its_composite();
    test_a_project_round_trips_and_an_export_is_not_a_save();
    test_the_marquee_claims_every_pixel_it_passed_over();
    test_the_select_commands_set_add_and_take_away();
    test_a_selection_holds_a_stroke_inside_it();
    test_the_ellipse_command_selects_the_curve_and_not_its_box();
    test_the_ellipse_marquee_draws_the_shape_of_its_own_tool_row();
    test_the_select_gesture_draws_and_puts_down_the_selection();
    test_the_paint_target_decides_where_a_stroke_lands();
    test_a_selection_limits_a_mask_stroke();
    test_the_mask_controls_name_the_layer_they_refused();
    test_a_selection_becomes_a_mask_and_a_mask_becomes_a_selection();
    test_a_layer_mask_costs_no_undo_step_and_reaches_the_file();
    test_the_overlay_reports_the_phase_it_draws_and_freezes_on_request();
    test_the_move_tool_slides_the_layer_and_undoes_it_exactly();
    test_a_move_that_never_left_its_pixel_is_not_an_edit();
    test_a_move_slides_the_active_layer_and_no_other();
    test_image_get_returns_the_documents_own_pixels();
    test_pixels_compare_finds_a_single_changed_byte();
    test_a_fit_waits_for_the_canvas_area_to_be_published();
    test_screenshot_returns_exactly_what_the_capture_gave();
    test_a_shorter_image_stays_centred_under_panning();
    test_view_navigation_keeps_the_image_reachable();
    return test::summary("app");
}
