// SPDX-License-Identifier: Apache-2.0
#include "test.h"

#include "app/ai_intent.h"
#include "ai/provider.h"
#include "ai/recorded.h"
#include "ai/recorded.h"
#include "app/ai_session.h"
#include "app/ai_verbs.h"
#include "app/commands.h"
#include "app/file_commands.h"
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
#include "io/config.h"
#include "platform/platform.h"

#include <nlohmann/json.hpp>

#include <sys/stat.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <thread>
#include <map>
#include <numbers>
#include <filesystem>
#include <string>
#include <vector>

using namespace noise;
using nlohmann::json;

namespace {

// Keep fixture configuration local to this test process on all supported hosts.
void set_test_env(const char* key, const std::string& value) {
#ifdef _WIN32
    _putenv_s(key, value.c_str());
#else
    ::setenv(key, value.c_str(), 1);
#endif
}

void unset_test_env(const char* key) {
#ifdef _WIN32
    _putenv_s(key, "");
#else
    ::unsetenv(key);
#endif
}

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

Image patterned_image(Extent size, int seed) {
    Image image = Image::filled(size, 0, 0, 0, 255);
    for (int y = 0; y < size.h; ++y) {
        for (int x = 0; x < size.w; ++x) {
            std::uint8_t* pixel = image.at(x, y);
            pixel[0] = static_cast<std::uint8_t>((seed + x * 7 + y * 3) % 256);
            pixel[1] = static_cast<std::uint8_t>((seed * 3 + x * 2 + y * 11) % 256);
            pixel[2] = static_cast<std::uint8_t>((seed * 5 + x * 13 + y) % 256);
        }
    }
    return image;
}

struct Harness {
    // Which server the session believes it has. `unconfigured` is where a person lands when they
    // installed the editor and set nothing, and the answer has to name the variable.
    enum class Config { recorded, unconfigured };

    AppState state;
    CommandTable table;
    dbg::DebugWire wire{table};
    int quits = 0;
    Config config_ = Config::recorded;

    Harness() : Harness(Config::recorded, NOISE_TEST_FIXTURES_DIR) {}

    explicit Harness(Config config) : Harness(config, NOISE_TEST_FIXTURES_DIR) {}

    Harness(Config config, const std::string& fixtures) : config_(config) {
        // One directory per harness, not per process: a second harness in the same test would otherwise
        // remove the directory the first one is still writing into.
        static int instance = 0;
        state.runtime_dir = (std::filesystem::temp_directory_path() /
                             ("noise-app-test-" + std::to_string(platform::pid()) + "-" +
                              std::to_string(instance++)))
                                .string();
        std::filesystem::remove_all(state.runtime_dir);
        std::filesystem::create_directories(state.runtime_dir);
        // The host publishes the canvas area every frame it draws; a harness with no
        // frames has to publish it by hand or viewport commands stay pending.
        state.viewport = Extent{1280, 800};
        state.viewport_published = true;
        state.framebuffer = Extent{1280, 800};
        state.canvas_origin = {0.0, 0.0};
        // The AI tables come from the tree, not from a machine's environment: a test that resolved roles
        // against an empty table would pass by proving nothing.
        state.ai.resource_dir = NOISE_TEST_ASSETS_DIR;
        // The recorded server answers instead of a socket: `server.connect`, `ai.run` and `job.wait`
        // then exercise the transport, the queue and the failure classes against the shapes the dev box
        // was observed sending, and the default test run stays hermetic.
        if (config_ == Config::recorded) {
            set_test_env("NOISE_AI_FIXTURES", fixtures);
        } else {
            unset_test_env("NOISE_AI_FIXTURES");
            unset_test_env("THENOISE_URL");
        }
        app::open_ai_session(state);
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
                                 "screenshot", "pixels.compare", "log.tail", "ui.theme", "view.fit",
                                 "view.pan", "view.zoom", "view.set", "ai.references",
                                 "ai.references.add", "ai.references.remove", "ai.references.move",
                                 "select.lasso"}) {
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
    CHECK_EQ(bound["tool.lasso"], std::string("L "));
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
    const int made = undo_depth(h);
    const json named = h.value("layer.add", json{{"name", "Ink"},
                                                 {"fill", json::array({9, 8, 7, 255})}});
    CHECK_EQ(named["layers"].size(), 3u);
    CHECK_EQ(named["layers"][1]["name"].get<std::string>(), std::string("Ink"));
    CHECK_EQ(named["active"].get<int>(), 1);

    // Making a layer is one step, and redo of it brings the layer back **as it was made** - a Ctrl+Z
    // that took the layer away and a redo that gave back an empty shell would be two different pictures,
    // which is why the fill travels with the arrival.
    CHECK_EQ(undo_depth(h), made + 1);
    h.value("edit.undo", json::object());
    CHECK_EQ(h.value("layer.list", json::object())["layers"].size(), 2u);
    h.value("edit.redo", json::object());
    const json remade = h.value("layer.list", json::object());
    CHECK_EQ(remade["layers"].size(), 3u);
    CHECK_EQ(remade["layers"][1]["name"].get<std::string>(), std::string("Ink"));
    CHECK_EQ(static_cast<int>(h.state.doc.layer(1).pixels.at(0, 0)[0]), 9);
    CHECK_EQ(static_cast<int>(h.state.doc.layer(1).pixels.at(0, 0)[3]), 255);

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

    // Deleting a layer is one step too, and what comes back is the layer - its place in the stack, its
    // name, its blend state and its own picture - rather than an empty one wearing the right name.
    const std::string ink_name = h.state.doc.layer(1).name;
    const json ink_row = h.value("layer.list", json::object())["layers"][1];
    const std::string ink_picture = ink_row["checksum"].get<std::string>();
    const std::string ink_mode = ink_row["mode"].get<std::string>();
    const double ink_opacity = ink_row["opacity"].get<double>();
    const int deleted = undo_depth(h);
    CHECK_OK(h.run("layer.remove", json{{"index", 1}}));
    CHECK_EQ(undo_depth(h), deleted + 1);
    h.value("edit.undo", json::object());
    const json restored = h.value("layer.list", json::object());
    CHECK_EQ(restored["layers"].size(), 3u);
    CHECK_EQ(restored["layers"][1]["name"].get<std::string>(), ink_name);
    CHECK_EQ(restored["layers"][1]["checksum"].get<std::string>(), ink_picture);
    CHECK_EQ(restored["layers"][1]["mode"].get<std::string>(), ink_mode);
    CHECK_EQ(restored["layers"][1]["opacity"].get<double>(), ink_opacity);
    h.value("edit.redo", json::object());
    CHECK_EQ(h.value("layer.list", json::object())["layers"].size(), 2u);

    // Down to one layer and no further: removing the last one would leave no canvas to draw on - and
    // undoing it all the way back gives the stack of three, each layer still itself.
    CHECK_OK(h.run("layer.remove", json{{"index", 1}}));
    const CommandResult last = h.run("layer.remove", json{{"index", 0}});
    CHECK(!last.ok);
    CHECK(last.error.find("last layer") != std::string::npos);
    CHECK_EQ(h.value("layer.list", json::object())["layers"].size(), 1u);

    h.value("edit.undo", json::object());
    h.value("edit.undo", json::object());
    const json back = h.value("layer.list", json::object());
    CHECK_EQ(back["layers"].size(), 3u);
    CHECK_EQ(back["layers"][1]["name"].get<std::string>(), ink_name);
    CHECK_EQ(back["layers"][1]["checksum"].get<std::string>(), ink_picture);
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

    // Marking is an edit, and one step per command that did it. Walking every step back changes no
    // pixel of the canvas - a coverage step puts coverage back and never colour - and leaves the
    // document with no marked area, which is what the first of them replaced.
    const std::string untouched = h.state.doc.composite().checksum_hex();
    const int marks = undo_depth(h);
    CHECK_EQ(marks, 9);  // the nine coverage commands above; the two strokes are in the redo stack
    for (int step = 0; step < marks; ++step) {
        CHECK(h.run("edit.undo", json::object()).ok);
        CHECK_EQ(h.state.doc.composite().checksum_hex(), untouched);
    }
    CHECK(!h.value("select.get", json::object())["active"].get<bool>());
    // Forward again is the same operation, and nine redos walk exactly these steps - the two undone
    // strokes sit under them in the redo stack. The coverage comes back and the canvas still does not
    // move.
    for (int step = 0; step < marks; ++step) {
        CHECK(h.run("edit.redo", json::object()).ok);
        CHECK_EQ(h.state.doc.composite().checksum_hex(), untouched);
    }
    CHECK_EQ(h.value("select.get", json::object())["covered"].get<int>(), 800);
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
// Which coverage a stroke writes is a setting the wire can read, aim and refuse against; the aim
// outlives the switch, because a gesture holds the lifetime it was pressed on. A stroke aimed at a mask
// the layer does not have is refused by name rather than quietly marking something else.
void test_the_coverage_target_decides_where_a_stroke_lands() {
    Harness h;
    h.value("doc.new", json{{"width", 24}, {"height", 12}, {"fill", json::array({20, 20, 20, 255})}});
    h.value("tool.set", json{{"tool", "cover"}, {"size", 8.0}, {"hardness", 1.0}, {"opacity", 1.0}});
    const json points = json::array({json::array({6.0, 6.0}), json::array({14.0, 6.0})});

    CHECK_EQ(h.value("app.state", json::object())["brush"]["coverage_target"].get<std::string>(),
             std::string("marked"));
    // The marked area is always writable; the mask half says so only once the layer has one.
    CHECK(h.value("coverage.target", json::object())["writable"].get<bool>());
    CHECK(h.value("mask.get", json::object())["mask"].is_null());

    const json aimed = h.value("coverage.target", json{{"to", "mask"}});
    CHECK_EQ(aimed["target"].get<std::string>(), std::string("mask"));
    CHECK(!aimed["writable"].get<bool>());
    const CommandResult nowhere = h.run("tool.stroke", json{{"points", points}});
    CHECK(!nowhere.ok);
    CHECK(nowhere.error.find("has none") != std::string::npos);
    CHECK(nowhere.error.find("mask.add") != std::string::npos);
    const CommandResult no_press = h.run("tool.pointer", json{{"phase", "press"}, {"at", json::array({6.0, 6.0})}});
    CHECK(!no_press.ok);
    CHECK(no_press.error.find("mask") != std::string::npos);
    CHECK_EQ(h.value("doc.get", json::object())["history"]["undo_depth"].get<int>(), 0);

    const std::string before_plane = h.value("doc.get", json::object())["checksum"].get<std::string>();
    const json added = h.value("mask.add", json::object());
    CHECK_EQ(added["covered"].get<std::int64_t>(), 24 * 12);
    CHECK_NEAR(added["area"].get<double>(), 24 * 12.0, 1e-9);
    // A mask that shows everything hides nothing, so the canvas is as it was - but the plane is in the
    // document, so putting one down is a step, and undoing it takes the mask off rather than moving a
    // pixel.
    CHECK_EQ(h.value("doc.get", json::object())["history"]["undo_depth"].get<int>(), 1);
    h.value("edit.undo", json::object());
    CHECK(h.value("mask.get", json::object())["mask"].is_null());
    CHECK_EQ(h.value("doc.get", json::object())["checksum"].get<std::string>(), before_plane);
    h.value("edit.redo", json::object());
    CHECK_EQ(h.value("mask.get", json::object())["mask"]["covered"].get<std::int64_t>(), 24 * 12);

    // A fill of the whole mask is a write: one step, and undo puts the coverage back exactly.
    const std::string shown = h.value("doc.get", json::object())["checksum"].get<std::string>();
    const json hidden = h.value("mask.fill", json{{"to", "hide"}});
    CHECK_EQ(hidden["covered"].get<std::int64_t>(), 0);
    CHECK_EQ(h.value("doc.get", json::object())["history"]["undo_depth"].get<int>(), 2);
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
    CHECK_EQ(stroke["coverage"].get<std::string>(), std::string("mask"));
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
    CHECK_EQ(h.value("doc.get", json::object())["history"]["undo_depth"].get<int>(), 3);
    h.value("edit.undo", json::object());
    CHECK_EQ(h.value("doc.get", json::object())["checksum"].get<std::string>(), filled);
    h.value("edit.redo", json::object());
    CHECK_EQ(h.value("doc.get", json::object())["checksum"].get<std::string>(), painted);

    // A live gesture reports the lifetime it has been writing since the press: aiming elsewhere
    // mid-drag cannot split one gesture across two homes for the same coverage.
    h.value("tool.pointer", json{{"phase", "press"}, {"at", json::array({4.0, 4.0})}});
    CHECK_EQ(h.value("app.state", json::object())["stroke"]["coverage"].get<std::string>(),
             std::string("mask"));
    h.value("coverage.target", json{{"to", "marked"}});
    h.value("tool.pointer", json{{"phase", "drag"}, {"at", json::array({8.0, 4.0})}});
    CHECK_EQ(h.value("app.state", json::object())["stroke"]["coverage"].get<std::string>(),
             std::string("mask"));
    h.value("tool.pointer", json{{"phase", "release"}, {"at", json::array({8.0, 4.0})}});
    CHECK_EQ(h.value("coverage.target", json::object())["target"].get<std::string>(),
             std::string("marked"));

    // The same gesture over the other lifetime marks the area instead: the layer's own bytes and its
    // mask both stay where they were, because a mark is coverage and not a picture - and the mark is
    // one undo step, which is what lets a freehand mark be taken back at all.
    const std::string coverage = h.value("mask.get", json::object())["mask"].dump();
    const std::string before = h.value("layer.list", json::object())["layers"][0]["checksum"].get<std::string>();
    const int undo_before = h.value("doc.get", json::object())["history"]["undo_depth"].get<int>();
    h.value("tool.stroke", json{{"points", points}});
    CHECK_EQ(h.value("layer.list", json::object())["layers"][0]["checksum"].get<std::string>(), before);
    CHECK_EQ(h.value("mask.get", json::object())["mask"].dump(), coverage);
    const std::int64_t marked_px = h.value("select.get", json::object())["covered"].get<std::int64_t>();
    CHECK(marked_px > 0);
    CHECK_EQ(h.value("doc.get", json::object())["history"]["undo_depth"].get<int>(), undo_before + 1);

    // The same dab at the other end of the coverage scale takes coverage back, so a mark is corrected
    // rather than cleared and redrawn. Still no pixel byte, and one step for the stroke.
    h.value("tool.set", json{{"tool", "un-cover"}, {"size", 10.0}, {"hardness", 1.0},
                             {"opacity", 1.0}});
    h.value("tool.stroke", json{{"points", json::array({json::array({3.0, 6.0})})}});
    const json rubbed = h.value("select.get", json::object());
    CHECK(rubbed["covered"].get<std::int64_t>() > 0);
    CHECK(rubbed["covered"].get<std::int64_t>() < marked_px);
    CHECK_EQ(h.value("layer.list", json::object())["layers"][0]["checksum"].get<std::string>(), before);
    CHECK_EQ(h.value("doc.get", json::object())["history"]["undo_depth"].get<int>(), undo_before + 2);

    // Colour is a tool, not a target: aimed at the mask, the brush still moves colour and leaves the
    // coverage alone, because the switch decides where *coverage* goes and nothing else.
    h.value("coverage.target", json{{"to", "mask"}});
    h.value("tool.set", json{{"tool", "brush"}});
    h.value("tool.stroke", json{{"points", points}});
    CHECK_NE(h.value("layer.list", json::object())["layers"][0]["checksum"].get<std::string>(), before);
    CHECK_EQ(h.value("mask.get", json::object())["mask"].dump(), coverage);
}

// The marquee is the other half of the same editor: aimed at the mask it puts its shape there, aimed at
// the marked area it puts it there, and it never needs a second tool to do the other job.
void test_the_marquee_lands_on_whichever_coverage_is_named() {
    Harness h;
    h.value("doc.new", json{{"width", 32}, {"height", 16}, {"fill", json::array({9, 9, 9, 255})}});
    h.value("tool.set", json{{"tool", "select"}});
    h.value("mask.add", json::object());
    h.value("mask.fill", json{{"to", "hide"}});

    h.value("tool.pointer", json{{"phase", "press"}, {"at", json::array({4.0, 4.0})}});
    h.value("tool.pointer", json{{"phase", "drag"}, {"at", json::array({12.0, 12.0})}});
    h.value("tool.pointer", json{{"phase", "release"}});
    CHECK(h.value("select.get", json::object())["active"].get<bool>());
    CHECK(h.value("mask.get", json::object())["mask"]["covered"].get<std::int64_t>() == 0);

    // The mark from the first drag is still the fence on every write, so put it down first: the mask
    // half of this test is about where the shape lands, not about the fence (which has its own test).
    h.value("select.clear", json::object());
    h.value("coverage.target", json{{"to", "mask"}});
    h.value("tool.pointer", json{{"phase", "press"}, {"at", json::array({20.0, 4.0})}});
    h.value("tool.pointer", json{{"phase", "drag"}, {"at", json::array({28.0, 12.0})}});
    h.value("tool.pointer", json{{"phase", "release"}});
    const json masked = h.value("mask.get", json::object())["mask"];
    CHECK(masked["covered"].get<std::int64_t>() > 0);
    CHECK_EQ(masked["bounds"][0].get<int>(), 20);
}

// A marquee is one gesture and one undo step, whichever lifetime it lands on. Three pointer events
// cost one step; the box that is marked is the one the button lifted on, not the union of the boxes the
// drag swept; and undo hands back the coverage the press found without moving a pixel of the picture.
void test_a_marquee_gesture_is_one_step_in_whichever_coverage() {
    Harness h;
    h.value("doc.new", json{{"width", 40}, {"height", 20}, {"fill", json::array({9, 9, 9, 255})}});
    h.value("tool.set", json{{"tool", "select"}});
    const int clean = undo_depth(h);
    const std::string picture = checksum_of(h);

    h.value("tool.pointer", json{{"phase", "press"}, {"at", json::array({4.0, 4.0})}});
    h.value("tool.pointer", json{{"phase", "drag"}, {"at", json::array({24.0, 16.0})}});
    CHECK(h.value("select.get", json::object())["active"].get<bool>());
    CHECK_EQ(undo_depth(h), clean);  // live on the canvas, not yet an edit
    h.value("tool.pointer", json{{"phase", "drag"}, {"at", json::array({12.0, 10.0})}});
    h.value("tool.pointer", json{{"phase", "release"}});
    CHECK_EQ(undo_depth(h), clean + 1);

    // The box the drag ended on rather than the one it swept: a trail of the wider box would mark
    // area nobody described, and the verb pressed there would ask about it.
    const json marked = h.value("select.get", json::object());
    CHECK_EQ(marked["covered"].get<int>(), 8 * 6);
    CHECK(marked["bounds"] == json::array({4, 4, 8, 6}));
    CHECK_EQ(checksum_of(h), picture);

    CHECK(h.run("edit.undo", json::object()).ok);
    CHECK(!h.value("select.get", json::object())["active"].get<bool>());
    CHECK_EQ(checksum_of(h), picture);  // a step over coverage puts coverage back, never colour
    CHECK(h.run("edit.redo", json::object()).ok);
    CHECK_EQ(h.value("select.get", json::object())["covered"].get<int>(), 8 * 6);
    CHECK_EQ(checksum_of(h), picture);

    // The other lifetime, through the layer's provisional door: the same three events, one step, and
    // the mask comes back wholly shown because that is what the press found.
    h.value("select.clear", json::object());
    h.value("mask.add", json::object());
    h.value("mask.fill", json{{"to", "show"}});
    h.value("coverage.target", json{{"to", "mask"}});
    const int shown = undo_depth(h);
    const std::string masked = checksum_of(h);  // wholly shown: what the press will find
    CHECK_EQ(h.value("mask.get", json::object())["mask"]["covered"].get<int>(), 40 * 20);
    h.value("tool.pointer", json{{"phase", "press"}, {"at", json::array({6.0, 2.0})}});
    h.value("tool.pointer", json{{"phase", "drag"}, {"at", json::array({30.0, 18.0})}});
    CHECK_EQ(undo_depth(h), shown);
    h.value("tool.pointer", json{{"phase", "drag"}, {"at", json::array({14.0, 8.0})}});
    h.value("tool.pointer", json{{"phase", "release"}});
    CHECK_EQ(undo_depth(h), shown + 1);
    const json mask_now = h.value("mask.get", json::object())["mask"];
    CHECK_EQ(mask_now["covered"].get<int>(), 8 * 6);
    CHECK(mask_now["bounds"] == json::array({6, 2, 8, 6}));
    CHECK(h.run("edit.undo", json::object()).ok);
    CHECK_EQ(h.value("mask.get", json::object())["mask"]["covered"].get<int>(), 40 * 20);
    CHECK_EQ(undo_depth(h), shown);
    CHECK_EQ(checksum_of(h), masked);
    CHECK_EQ(h.state.doc.composite().at(7, 3)[3], 255);  // the canvas shows the layer again

    // A switch mid-drag moves neither the shape nor the lifetime. The press opened the marked door, so
    // the box lands on the marked area, the mask keeps every byte, and the gesture shuts the door it
    // came in by: read the switch at release instead and the marked gesture is never closed - after
    // which every selection command would write coverage and record nothing.
    h.value("coverage.target", json{{"to", "marked"}});
    const int unswitched = undo_depth(h);
    const std::string mask_before = h.value("mask.get", json::object())["mask"].dump();
    h.value("tool.pointer", json{{"phase", "press"}, {"at", json::array({4.0, 4.0})}});
    CHECK_EQ(h.value("app.state", json::object())["stroke"]["marquee_coverage"].get<std::string>(),
             std::string("marked"));
    h.value("coverage.target", json{{"to", "mask"}});
    h.value("tool.pointer", json{{"phase", "drag"}, {"at", json::array({12.0, 10.0})}});
    CHECK_EQ(h.value("app.state", json::object())["stroke"]["marquee_coverage"].get<std::string>(),
             std::string("marked"));
    h.value("tool.pointer", json{{"phase", "release"}});
    CHECK_EQ(undo_depth(h), unswitched + 1);
    CHECK_EQ(h.value("mask.get", json::object())["mask"].dump(), mask_before);
    CHECK_EQ(h.value("select.get", json::object())["covered"].get<int>(), 8 * 6);
}

void test_lasso_command_combines_coverage_and_never_changes_pixels() {
    Harness h;
    h.value("doc.new", json{{"width", 24}, {"height", 16},
                            {"fill", json::array({12, 34, 56, 255})}});
    const std::string pixels = checksum_of(h);
    const json triangle = json::array({json::array({2.0, 2.0}), json::array({10.0, 2.0}),
                                       json::array({2.0, 10.0})});
    const json first = h.value("select.lasso", json{{"points", triangle}});
    CHECK(first["active"].get<bool>());
    CHECK_EQ(h.state.doc.selection()->coverage(3, 3), kMaskIn);
    CHECK_EQ(h.state.doc.selection()->coverage(9, 9), kMaskOut);
    CHECK(std::any_of(h.state.doc.selection()->plane().begin(),
                      h.state.doc.selection()->plane().end(), [](std::uint8_t value) {
                          return value != kMaskOut && value != kMaskIn;
                      }));
    h.value("server.connect", json::object());
    const json preview = h.value("ai.preview", json{{"recipe", "inpaint-remove"}, {"seed", 3}});
    CHECK(preview["region"]["mask_bounds"] == json::array({2, 2, 8, 8}));
    const json square = json::array({json::array({14.0, 2.0}), json::array({21.0, 2.0}),
                                     json::array({21.0, 9.0}), json::array({14.0, 9.0})});
    h.value("select.lasso", json{{"points", square}, {"mode", "add"}});
    CHECK_EQ(h.state.doc.selection()->coverage(3, 3), kMaskIn);
    CHECK_EQ(h.state.doc.selection()->coverage(18, 5), kMaskIn);
    h.value("select.lasso", json{{"points", triangle}, {"mode", "subtract"}});
    CHECK_EQ(h.state.doc.selection()->coverage(3, 3), kMaskOut);
    CHECK_EQ(h.state.doc.selection()->coverage(18, 5), kMaskIn);

    const Mask before = *h.state.doc.selection();
    for (const json& args : {json{{"points", json::array({json::array({1.0, 1.0}),
                                                           json::array({5.0, 5.0})})}},
                             json{{"points", triangle}, {"mode", "intersect"}}}) {
        CHECK(!h.run("select.lasso", args).ok);
        CHECK(*h.state.doc.selection() == before);
    }
    h.value("select.lasso", json{{"points", square}});
    CHECK_EQ(h.state.doc.selection()->coverage(3, 3), kMaskOut);
    CHECK_EQ(h.state.doc.selection()->coverage(18, 5), kMaskIn);
    CHECK_EQ(checksum_of(h), pixels);
}

void test_lasso_pointer_commits_once_and_undo_redo_restore_exact_coverage() {
    Harness h;
    h.value("doc.new", json{{"width", 24}, {"height", 16},
                            {"fill", json::array({12, 34, 56, 255})}});
    h.value("select.rect", json{{"rect", json::array({1, 1, 5, 5})}});
    const Mask before = *h.state.doc.selection();
    const std::string pixels = checksum_of(h);
    const int depth = undo_depth(h);
    h.value("tool.set", json{{"tool", "lasso"}});
    CHECK_EQ(h.state.tool, std::string("lasso"));

    const std::vector<Vec2> path{{10.0, 2.0}, {20.0, 2.0}, {20.0, 12.0}, {10.0, 12.0}};
    h.value("tool.pointer", json{{"phase", "press"}, {"at", json::array({10.0, 2.0})},
                                  {"mode", "add"}});
    h.value("tool.pointer", json{{"phase", "drag"}, {"at", json::array({20.0, 2.0})}});
    h.value("tool.pointer", json{{"phase", "drag"}, {"at", json::array({20.0, 12.0})}});
    const json during = h.value("app.state", json::object())["stroke"];
    CHECK(during["active"].get<bool>());
    CHECK_EQ(during["marquee_shape"].get<std::string>(), std::string("lasso"));
    CHECK_EQ(during["lasso_points"].size(), 3u);
    CHECK(*h.state.doc.selection() == before);
    CHECK_EQ(undo_depth(h), depth);

    h.value("tool.set", json{{"tool", "brush"}});
    h.value("coverage.target", json{{"to", "mask"}});
    h.value("tool.pointer", json{{"phase", "release"}, {"at", json::array({10.0, 12.0})}});
    Mask expected = before;
    CHECK(op_lasso(expected, path, MaskOp::add));
    const Mask after = *h.state.doc.selection();
    CHECK(after == expected);
    CHECK_EQ(undo_depth(h), depth + 1);
    CHECK_EQ(checksum_of(h), pixels);
    CHECK(!h.state.select.active);
    CHECK(!h.state.doc.in_provisional());
    CHECK(h.state.doc.layer(0).mask == std::nullopt);

    CHECK(h.run("edit.undo", json::object()).ok);
    CHECK(*h.state.doc.selection() == before);
    CHECK(h.run("edit.redo", json::object()).ok);
    CHECK(*h.state.doc.selection() == after);

    const int before_cancel = undo_depth(h);
    h.value("coverage.target", json{{"to", "marked"}});
    h.value("tool.set", json{{"tool", "lasso"}});
    h.value("tool.pointer", json{{"phase", "press"}, {"at", json::array({3.0, 12.0})}});
    h.value("tool.pointer", json{{"phase", "drag"}, {"at", json::array({8.0, 12.0})}});
    h.value("tool.pointer", json{{"phase", "cancel"}});
    CHECK(!h.state.select.active);
    CHECK(!h.state.doc.in_provisional());
    CHECK(*h.state.doc.selection() == after);
    CHECK_EQ(undo_depth(h), before_cancel);
}

void test_lasso_edits_a_layer_mask_through_its_provisional_transaction() {
    Harness h;
    h.value("doc.new", json{{"width", 20}, {"height", 16},
                            {"fill", json::array({40, 50, 60, 255})}});
    h.value("mask.add", json::object());
    h.value("mask.fill", json{{"to", "hide"}});
    h.value("coverage.target", json{{"to", "mask"}});
    h.value("tool.set", json{{"tool", "lasso"}});
    const Mask before = *h.state.doc.layer(0).mask;
    const std::string pixels = h.state.doc.layer(0).pixels.checksum_hex();
    const int depth = undo_depth(h);
    const std::vector<Vec2> path{{3.0, 3.0}, {15.0, 3.0}, {15.0, 12.0}, {3.0, 12.0}};

    h.value("tool.pointer", json{{"phase", "press"}, {"at", json::array({3.0, 3.0})}});
    CHECK(h.state.doc.provisional_area() == Rect{3, 3, 1, 1});
    h.value("tool.pointer", json{{"phase", "drag"}, {"at", json::array({15.0, 3.0})}});
    h.value("tool.pointer", json{{"phase", "drag"}, {"at", json::array({15.0, 12.0})}});
    h.value("tool.pointer", json{{"phase", "release"}, {"at", json::array({3.0, 12.0})}});
    Mask expected(h.state.doc.size());
    CHECK(op_lasso(expected, path, MaskOp::replace));
    const Mask after = *h.state.doc.layer(0).mask;
    CHECK(after == expected);
    CHECK(!h.state.doc.selection().has_value());
    CHECK_EQ(undo_depth(h), depth + 1);
    CHECK_EQ(h.state.doc.layer(0).pixels.checksum_hex(), pixels);
    CHECK(!h.state.doc.in_provisional());
    CHECK(h.run("edit.undo", json::object()).ok);
    CHECK(*h.state.doc.layer(0).mask == before);
    CHECK(h.run("edit.redo", json::object()).ok);
    CHECK(*h.state.doc.layer(0).mask == after);

    const int before_cancel = undo_depth(h);
    h.value("tool.pointer", json{{"phase", "press"}, {"at", json::array({4.0, 4.0})}});
    h.value("tool.pointer", json{{"phase", "drag"}, {"at", json::array({10.0, 10.0})}});
    h.value("tool.pointer", json{{"phase", "cancel"}});
    CHECK(*h.state.doc.layer(0).mask == after);
    CHECK_EQ(undo_depth(h), before_cancel);
    CHECK(!h.state.doc.in_provisional());

    h.value("mask.remove", json::object());
    h.value("coverage.target", json{{"to", "mask"}});
    const int without_mask = undo_depth(h);
    const CommandResult refused = h.run(
        "tool.pointer", json{{"phase", "press"}, {"at", json::array({4.0, 4.0})}});
    CHECK(!refused.ok);
    CHECK(!h.state.select.active);
    CHECK_EQ(undo_depth(h), without_mask);
}

// Replacing a nonempty layer mask must clear its old coverage, not reveal the old bounds.
void test_lasso_mask_replace_discards_previous_visible_pixels() {
    Harness h;
    h.value("doc.new", json{{"width", 20}, {"height", 16},
                            {"fill", json::array({40, 50, 60, 255})}});
    h.value("mask.add", json::object());
    h.value("mask.fill", json{{"to", "show"}});
    h.value("coverage.target", json{{"to", "mask"}});
    h.value("tool.set", json{{"tool", "lasso"}});
    const Mask before = *h.state.doc.layer(0).mask;
    CHECK_EQ(before.coverage(16, 12), kMaskIn);
    const int depth = undo_depth(h);

    h.value("tool.pointer", json{{"phase", "press"}, {"at", json::array({2.0, 2.0})}});
    h.value("tool.pointer", json{{"phase", "drag"}, {"at", json::array({8.0, 2.0})}});
    h.value("tool.pointer", json{{"phase", "drag"}, {"at", json::array({8.0, 8.0})}});
    h.value("tool.pointer", json{{"phase", "release"}, {"at", json::array({2.0, 8.0})}});
    const Mask after = *h.state.doc.layer(0).mask;
    CHECK_EQ(after.coverage(4, 4), kMaskIn);
    CHECK_EQ(after.coverage(16, 12), kMaskOut);
    CHECK_EQ(undo_depth(h), depth + 1);
    CHECK(h.run("edit.undo", json::object()).ok);
    CHECK(*h.state.doc.layer(0).mask == before);
    CHECK(h.run("edit.redo", json::object()).ok);
    CHECK(*h.state.doc.layer(0).mask == after);
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

    h.value("tool.set", json{{"tool", "cover"}, {"size", 6.0}, {"hardness", 1.0}, {"opacity", 1.0}});
    h.value("coverage.target", json{{"to", "mask"}});
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
    CHECK(h.run("coverage.target", json{{"to", "colour"}}).error.find("marked") != std::string::npos);
    CHECK(h.run("coverage.target", json{{"to", "colour"}}).error.find("mask") != std::string::npos);
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
    const std::string pixels_with_mask = h.state.doc.layer(0).pixels.checksum_hex();
    h.value("select.clear", json::object());
    // Putting the mark down is a step of its own, and the stroke after it is the one this claim is about.
    const int steps_after_mask = undo_depth(h);
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

// A layer mask is composite state: it reaches the canvas and the file through the command table, and
// the plane arriving is the step that owns it - undoing that takes the mask off, which is what the eye
// and the opacity slider are not.
void test_a_layer_mask_is_one_step_and_reaches_the_file() {
    constexpr int kWidth = 24;
    constexpr int kHeight = 16;
    Harness h;
    h.value("doc.new", json{{"width", kWidth}, {"height", kHeight},
                            {"fill", json::array({200, 100, 50, 255})}});
    h.value("select.rect", json{{"rect", json::array({2, 2, 6, 6})}});
    const int steps = undo_depth(h);
    h.value("mask.from_selection", json::object());
    CHECK_EQ(undo_depth(h), steps + 1);
    CHECK(h.state.doc.dirty());
    // The step holds the plane, not the picture: undo takes the mask off and leaves the layer's own
    // bytes where they were, which is the difference between masking and erasing.
    const std::string layer_bytes = h.state.doc.layer(0).pixels.checksum_hex();
    h.value("edit.undo", json::object());
    CHECK(!h.state.doc.layer(0).mask.has_value());
    CHECK_EQ(h.state.doc.layer(0).pixels.checksum_hex(), layer_bytes);
    h.value("edit.redo", json::object());
    CHECK(h.state.doc.layer(0).mask.has_value());

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
    const double want = std::numbers::pi * a * b;
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

// The result compositor, reached the way the UI will reach it: through the command table, so the
// selection fence, the undo step and the revision all behave as they do for a stroke, and so a probe
// can look at the pixels.
// *Blend* compiles its region from the layer it was asked on, not from whatever happens to be marked:
// the coverage is that layer's own alpha, so a paste's soft edge stays soft and a mark left behind from
// the last operation cannot decide what the model is shown. The empty layer is the other half of the
// claim - no coverage is a refusal that names the layer, not a region over the whole frame.
void test_blend_takes_the_layer_for_its_region() {
    Harness h;
    h.value("doc.new", json{{"width", 120}, {"height", 100}, {"fill", json::array({30, 40, 50, 255})}});
    h.value("server.connect", json::object());
    h.value("layer.add", json{{"name", "pasted"}, {"fill", json::array({200, 100, 50, 128})}});
    h.value("select.rect", json{{"rect", json::array({10, 10, 20, 20})}});

    const json preview = h.value("ai.preview", json{{"recipe", "blend"}, {"seed", 42}});
    CHECK_EQ(preview["request"]["path"].get<std::string>(), std::string("/images/edits"));
    // Half-alpha over the whole canvas is coverage over the whole canvas, so the region is the frame and
    // the 20x20 mark is nowhere in it. Nothing thresholds the 128 away.
    CHECK_EQ(preview["region"]["mask_bounds"], (json::array({0, 0, 120, 100})));
    CHECK(preview["region"]["coverage"].get<std::string>().find("'pasted'") != std::string::npos);
    const std::string size = preview["request"]["size"].get<std::string>();
    const std::string::size_type cross = size.find('x');
    CHECK_EQ(std::stoi(size.substr(0, cross)) % 16, 0);
    CHECK_EQ(std::stoi(size.substr(cross + 1)) % 16, 0);
    // No words were asked for and none were invented by the caller: the standing instruction is the
    // recipe's, and the scene it is to match is in the upload, not in the prompt.
    CHECK(preview["prompt"].get<std::string>().find("Harmonize the marked object") !=
          std::string::npos);
    CHECK(!preview["additional_references"]["will_transmit"].get<bool>());
    CHECK(preview["request"].contains("seed"));

    h.value("layer.add", json{{"name", "empty"}, {"fill", json::array({0, 0, 0, 0})}});
    const CommandResult refused = h.run("ai.blend", json{{"seed", 42}});
    CHECK(!refused.ok);
    CHECK(refused.error.find("'empty'") != std::string::npos);
    CHECK_EQ(h.state.doc.layers().size(), std::size_t(3));
}

void test_ai_reference_commands_keep_images_separate_from_document_layers() {
    Harness h;
    h.value("doc.new", json{{"width", 64}, {"height", 64},
                             {"fill", json::array({30, 80, 120, 255})}});
    const json document_before = h.value("doc.get", json::object());
    const Image house = patterned_image(Extent{23, 15}, 31);
    const Image swing = patterned_image(Extent{11, 27}, 173);
    const std::string house_path = h.temp("House.png");
    const std::string swing_path = h.temp("Swing.png");
    std::string error;
    CHECK(io::save_png(house_path, house, error));
    CHECK(io::save_png(swing_path, swing, error));

    const json added = h.value("file.answer", json{{"for", "reference"},
                                                     {"paths", json::array(
                                                                   {house_path, swing_path})}});
    CHECK(added["session_only"].get<bool>());
    CHECK_EQ(added["count"].get<std::size_t>(), std::size_t(2));
    REQUIRE(added["references"].size() == std::size_t(2));
    CHECK_EQ(added["references"][0]["number"].get<int>(), 2);
    CHECK_EQ(added["references"][1]["number"].get<int>(), 3);
    CHECK_EQ(added["references"][0]["name"].get<std::string>(), std::string("House.png"));
    CHECK_EQ(added["references"][1]["name"].get<std::string>(), std::string("Swing.png"));
    CHECK_EQ(added["references"][0]["dimensions"], json::array({23, 15}));
    CHECK_EQ(added["references"][1]["dimensions"], json::array({11, 27}));
    CHECK_EQ(h.state.doc.revision(), document_before["revision"].get<std::uint64_t>());
    CHECK_EQ(checksum_of(h), document_before["checksum"].get<std::string>());
    CHECK_EQ(undo_depth(h), document_before["history"]["undo_depth"].get<int>());

    std::string decode_error;
    REQUIRE(h.state.ai.references.size() == std::size_t(2));
    const Image decoded_house = io::decode_png(h.state.ai.references[0].png, decode_error);
    const Image decoded_swing = io::decode_png(h.state.ai.references[1].png, decode_error);
    CHECK_EQ(decoded_house.checksum_hex(), house.checksum_hex());
    CHECK_EQ(decoded_swing.checksum_hex(), swing.checksum_hex());
    const std::string house_id = h.state.ai.references[0].id;
    const std::string swing_id = h.state.ai.references[1].id;

    const json moved = h.value("ai.references.move", json{{"id", swing_id}, {"to", 0}});
    CHECK_EQ(moved["references"][0]["name"].get<std::string>(), std::string("Swing.png"));
    CHECK_EQ(moved["references"][0]["number"].get<int>(), 2);
    h.value("ai.references.remove", json{{"id", house_id}});
    CHECK_EQ(h.value("ai.references", json::object())["count"].get<std::size_t>(), std::size_t(1));

    h.value("layer.add", json{{"name", "House_layer"},
                               {"fill", json::array({220, 35, 18, 255})}});
    h.value("select.rect", json{{"rect", json::array({11, 13, 17, 19})}});
    h.value("mask.from_selection", json::object());
    h.value("layer.add", json{{"name", "Unrelated_layer"},
                               {"fill", json::array({5, 190, 240, 255})}});
    h.value("layer.set", json{{"index", 1}, {"opacity", 0.5}});
    const json stack_before_reference = h.value("doc.get", json::object());
    const Image target_pixels = h.state.doc.layer(1).pixels;
    const std::uint64_t encodes_before_layer = h.state.ai.reference_encodes;
    const json layer_added = h.value("ai.references.add", json{{"layer", 1}});
    REQUIRE(layer_added["references"].size() == std::size_t(2));
    const json& layer_row = layer_added["references"][1];
    CHECK_EQ(layer_row["number"].get<int>(), 3);
    CHECK_EQ(layer_row["name"].get<std::string>(), std::string("House_layer"));
    CHECK(layer_row["linked"].get<bool>());
    CHECK_EQ(layer_row["dimensions"], json::array({17, 19}));
    CHECK_EQ(h.state.ai.reference_encodes, encodes_before_layer + 1);
    CHECK_EQ(h.state.doc.revision(), stack_before_reference["revision"].get<std::uint64_t>());
    CHECK_EQ(h.value("doc.get", json::object())["checksum"].get<std::string>(),
             stack_before_reference["checksum"].get<std::string>());
    CHECK_EQ(h.state.doc.layer(1).pixels.checksum_hex(), target_pixels.checksum_hex());

    const app::AiReference& cached_layer = h.state.ai.references.back();
    const std::string first_layer_hash = cached_layer.sha256;
    const std::uint64_t first_layer_revision = cached_layer.revision;
    const Image decoded_layer = io::decode_png(cached_layer.png, decode_error);
    REQUIRE(!decoded_layer.empty());
    CHECK_EQ(decoded_layer.size(), (Extent{17, 19}));
    CHECK_EQ(decoded_layer.at(0, 0)[0], std::uint8_t(220));
    CHECK_EQ(decoded_layer.at(0, 0)[1], std::uint8_t(35));
    CHECK_EQ(decoded_layer.at(0, 0)[2], std::uint8_t(18));
    CHECK_EQ(decoded_layer.at(0, 0)[3], std::uint8_t(128));
    const std::uint64_t cached_encodes = h.state.ai.reference_encodes;
    h.value("ai.references", json::object());
    h.value("ai.references", json::object());
    CHECK_EQ(h.state.ai.reference_encodes, cached_encodes);

    h.value("layer.set", json{{"index", 1}, {"opacity", 0.25}});
    const json opacity_changed = h.value("ai.references", json::object());
    CHECK_EQ(h.state.ai.reference_encodes, cached_encodes + 1);
    CHECK(opacity_changed["references"][1]["revision"].get<std::uint64_t>() >
          first_layer_revision);
    CHECK(opacity_changed["references"][1]["sha256"].get<std::string>() != first_layer_hash);
    const Image quarter_opacity = io::decode_png(h.state.ai.references.back().png, decode_error);
    CHECK_EQ(quarter_opacity.at(0, 0)[3], std::uint8_t(64));

    h.value("layer.select", json{{"index", 1}});
    json stroke = full_stroke();
    stroke["points"] = json::array({json::array({14.0, 15.0}), json::array({16.0, 15.0})});
    stroke["size"] = 4.0;
    stroke["color"] = json::array({40, 240, 90, 255});
    h.value("tool.stroke", stroke);
    const std::string opacity_hash = h.state.ai.references.back().sha256;
    const json pixels_changed = h.value("ai.references", json::object());
    CHECK_EQ(h.state.ai.reference_encodes, cached_encodes + 2);
    CHECK(pixels_changed["references"][1]["sha256"].get<std::string>() != opacity_hash);

    const std::string frozen_hash = h.state.ai.references.back().sha256;
    const std::uint64_t frozen_encodes = h.state.ai.reference_encodes;
    h.value("layer.select", json{{"index", 1}});
    h.value("layer.remove", json::object());
    const json frozen = h.value("ai.references", json::object());
    CHECK(!frozen["references"][1]["linked"].get<bool>());
    CHECK_EQ(frozen["references"][1]["sha256"].get<std::string>(), frozen_hash);
    CHECK_EQ(h.state.ai.reference_encodes, frozen_encodes);
}

void test_ai_references_relink_after_undo_but_detach_across_documents() {
    Harness h;
    h.value("doc.new", json{{"width", 32}, {"height", 32},
                             {"fill", json::array({32, 64, 96, 255})}});
    h.value("layer.add", json{{"name", "Linked-reference"},
                               {"fill", json::array({180, 90, 45, 255})}});
    const json added = h.value("ai.references.add", json{{"layer", 1}});
    REQUIRE(added["references"].size() == std::size_t(1));
    const std::uint64_t layer_id = h.state.ai.references[0].source_layer_id;
    const std::string original_hash = h.state.ai.references[0].sha256;
    const std::uint64_t encodes = h.state.ai.reference_encodes;

    h.value("layer.select", json{{"index", 1}});
    h.value("layer.remove", json::object());
    const json absent = h.value("ai.references", json::object());
    CHECK(!absent["references"][0]["linked"].get<bool>());
    CHECK_EQ(h.state.ai.references[0].source_layer_id, layer_id);
    CHECK_EQ(absent["references"][0]["sha256"].get<std::string>(), original_hash);
    CHECK_EQ(h.state.ai.reference_encodes, encodes);

    h.value("edit.undo", json::object());
    const json restored = h.value("ai.references", json::object());
    CHECK(restored["references"][0]["linked"].get<bool>());
    CHECK_EQ(h.state.ai.references[0].source_layer_id, layer_id);
    CHECK_EQ(restored["references"][0]["sha256"].get<std::string>(), original_hash);
    const std::uint64_t restored_encodes = h.state.ai.reference_encodes;
    h.value("layer.set", json{{"index", 1}, {"opacity", 0.5}});
    const json changed = h.value("ai.references", json::object());
    CHECK(changed["references"][0]["linked"].get<bool>());
    CHECK_NE(changed["references"][0]["sha256"].get<std::string>(), original_hash);
    CHECK_EQ(h.state.ai.reference_encodes, restored_encodes + 1);

    const std::string cached_hash = h.state.ai.references[0].sha256;
    h.value("doc.new", json{{"width", 32}, {"height", 32},
                             {"fill", json::array({12, 24, 48, 255})}});
    const json other_document = h.value("ai.references", json::object());
    CHECK(!other_document["references"][0]["linked"].get<bool>());
    CHECK(other_document["references"][0]["layer_id"].is_null());
    CHECK_EQ(other_document["references"][0]["sha256"].get<std::string>(), cached_hash);
    CHECK_EQ(h.state.ai.reference_encodes, restored_encodes + 1);
}

void test_reference_inclusion_defaults_and_preview_match_the_operation() {
    Harness h;
    h.value("doc.new", json{{"width", 64}, {"height", 64},
                             {"fill", json::array({30, 40, 50, 255})}});
    h.value("select.rect", json{{"rect", json::array({20, 20, 16, 16})}});
    const std::string path = h.temp("Independent.png");
    std::string error;
    CHECK(io::save_png(path, patterned_image(Extent{9, 11}, 202), error));
    h.value("ai.references.add", json{{"path", path}});
    h.value("server.connect", json::object());

    struct Operation {
        const char* recipe;
        bool includes_by_default;
        json vars;
    };
    const std::vector<Operation> operations{
        {"inpaint-replace", true, json{{"instruction", "Replace the selected object."}}},
        {"region-fill", true, json{{"prompt", "Place the swing."}}},
        {"blend", false, json::object()},
        {"heal", false, json::object()},
        {"inpaint-remove", false, json::object()},
    };
    for (const Operation& operation : operations) {
        const json intent = h.value("ai.intent", json{{"recipe", operation.recipe}});
        CHECK_EQ(intent["include_references_default"].get<bool>(), operation.includes_by_default);
        CHECK_EQ(intent["include_references"].get<bool>(), operation.includes_by_default);
        const json preview = h.value("ai.preview", json{{"recipe", operation.recipe},
                                                         {"vars", operation.vars}});
        CHECK_EQ(preview["additional_references"]["will_transmit"].get<bool>(),
                 operation.includes_by_default);
        CHECK_EQ(preview["additional_references"]["transmitted_count"].get<std::size_t>(),
                 operation.includes_by_default ? std::size_t(1) : std::size_t(0));
        CHECK_EQ(preview["request"]["image_count"].get<std::size_t>(),
                 operation.includes_by_default ? std::size_t(2) : std::size_t(1));
    }

    const json enabled = h.value("ai.intent", json{{"recipe", "blend"},
                                                     {"include_references", true}});
    CHECK(enabled["include_references"].get<bool>());
    CHECK_EQ(h.value("ai.preview", json{{"recipe", "blend"}})["request"]["image_count"]
                 .get<std::size_t>(), std::size_t(2));
    const json disabled = h.value("ai.intent", json{{"include_references", false}});
    CHECK(!disabled["include_references"].get<bool>());
    const json preview = h.value("ai.preview", json{{"recipe", "blend"}});
    CHECK_EQ(preview["additional_references"]["status"].get<std::string>(),
             std::string("configured_not_included"));
    CHECK_EQ(preview["request"]["image_count"].get<std::size_t>(), std::size_t(1));
    CHECK_EQ(h.value("ai.preview", json{{"recipe", "inpaint-remove"},
                                         {"include_references", true}})["request"]["image_count"]
                 .get<std::size_t>(), std::size_t(2));
}

void test_multi_image_edit_uses_an_ordered_snapshot_and_keeps_the_existing_landing() {
    Harness h;
    const Image garden = patterned_image(Extent{64, 64}, 9);
    const std::string garden_path = h.temp("Garden.png");
    std::string error;
    CHECK(io::save_png(garden_path, garden, error));
    h.value("doc.open", json{{"path", garden_path}});
    h.value("select.rect", json{{"rect", json::array({24, 24, 16, 16})}});
    const std::string before = checksum_of(h);
    const std::string original_layer = h.state.doc.layer(0).pixels.checksum_hex();

    const Image house = patterned_image(Extent{31, 21}, 73);
    const Image swing = patterned_image(Extent{17, 29}, 181);
    const std::string house_path = h.temp("House.png");
    const std::string swing_path = h.temp("Swing.png");
    CHECK(io::save_png(house_path, house, error));
    CHECK(io::save_png(swing_path, swing, error));
    h.value("ai.references.add", json{{"paths", json::array({house_path, swing_path})}});
    REQUIRE(h.state.ai.references.size() == std::size_t(2));
    const std::vector<std::uint8_t> transmitted_house = h.state.ai.references[0].png;
    const std::vector<std::uint8_t> transmitted_swing = h.state.ai.references[1].png;
    const std::string house_id = h.state.ai.references[0].id;
    const std::string swing_id = h.state.ai.references[1].id;
    CHECK_EQ(checksum_of(h), before);
    CHECK_EQ(h.state.doc.layer(0).pixels.checksum_hex(), original_layer);

    CHECK(h.value("server.connect", json::object())["connected"].get<bool>());
    const std::string change_words =
        "Replace the selected object in Image 1 with the object shown in Image 2. "
        "Preserve its recognizable appearance and keep the remaining scene unchanged.";
    const json args{{"recipe", "inpaint-replace"}, {"seed", 313},
                    {"vars", {{"instruction", change_words}}}};
    const json preview = h.value("ai.preview", args);
    const json request = preview["request"];
    CHECK_EQ(preview["additional_references"]["status"].get<std::string>(),
             std::string("included"));
    CHECK_EQ(preview["additional_references"]["transmitted_count"].get<std::size_t>(),
             std::size_t(2));
    CHECK(preview["prompt"].get<std::string>().find(change_words) != std::string::npos);
    REQUIRE(request["images"].size() == std::size_t(3));
    CHECK_EQ(request["image_count"].get<std::size_t>(), std::size_t(3));
    CHECK_EQ(request["images"][0]["source"].get<std::string>(), std::string("document"));
    CHECK_EQ(request["images"][1]["name"].get<std::string>(), std::string("House.png"));
    CHECK_EQ(request["images"][2]["name"].get<std::string>(), std::string("Swing.png"));
    CHECK_EQ(request["images"][1]["dimensions"], json::array({31, 21}));
    CHECK_EQ(request["images"][2]["dimensions"], json::array({17, 29}));
    CHECK_EQ(request["wire_image_fields"], json::array({"image[]", "image[]", "image[]"}));
    CHECK_EQ(request["image_bytes"].get<std::size_t>(), transmitted_house.size() +
                                                          transmitted_swing.size() +
                                                          request["images"][0]["encoded_bytes"]
                                                              .get<std::size_t>());

    const json run = h.value("ai.run", args);
    const std::string job_id = run["job"]["id"].get<std::string>();
    const json queued_request = run["job"]["request"];
    CHECK_EQ(queued_request["image_count"].get<std::size_t>(), std::size_t(3));
    CHECK_EQ(queued_request["blob"].get<std::string>(), request["blob"].get<std::string>());

    h.value("ai.references.move", json{{"id", swing_id}, {"to", 0}});
    const json reordered_preview = h.value("ai.preview", args)["request"];
    CHECK_EQ(reordered_preview["images"][1]["name"].get<std::string>(), std::string("Swing.png"));
    CHECK_NE(reordered_preview["blob"].get<std::string>(), queued_request["blob"].get<std::string>());
    h.value("ai.references.remove", json{{"id", house_id}});
    h.value("ai.references.remove", json{{"id", swing_id}});
    const json without_references = h.value("ai.preview", args)["request"];
    CHECK_EQ(without_references["image_count"].get<std::size_t>(), std::size_t(1));
    CHECK_NE(without_references["blob"].get<std::string>(), queued_request["blob"].get<std::string>());
    CHECK_EQ(queued_request["image_count"].get<std::size_t>(), std::size_t(3));
    CHECK_EQ(checksum_of(h), before);

    const json done = h.value("job.wait", json{{"id", job_id}, {"timeout", 30}});
    CHECK_EQ(done["state"].get<std::string>(), std::string("succeeded"));
    CHECK(done["landing"]["landed"].get<bool>());
    CHECK_EQ(h.state.doc.layers().size(), std::size_t(2));
    CHECK_EQ(h.state.doc.layer(0).pixels.checksum_hex(), original_layer);
    const Image result_layer = h.state.doc.layer(1).pixels;
    CHECK(result_layer.at(0, 0)[3] == 0);
    CHECK(result_layer.at(63, 63)[3] == 0);
    const Image after = h.state.doc.composite();
    std::size_t outside_written_layer = 0;
    for (int y = 0; y < after.size().h; ++y) {
        for (int x = 0; x < after.size().w; ++x) {
            if (result_layer.at(x, y)[3] == 0 &&
                std::memcmp(after.at(x, y), garden.at(x, y), kChannels) != 0) {
                ++outside_written_layer;
            }
        }
    }
    CHECK_EQ(outside_written_layer, std::size_t(0));
    CHECK_NE(after.checksum_hex(), before);

    auto* recorded = dynamic_cast<ai::RecordedTransport*>(h.state.ai.transport.get());
    REQUIRE(recorded != nullptr);
    if (recorded == nullptr) {
        return;
    }
    const std::vector<ai::CapturedRequest> sent = recorded->sent();
    const auto post = std::find_if(sent.rbegin(), sent.rend(), [](const ai::CapturedRequest& item) {
        return item.method == "POST" && item.path.find("/images/edits") != std::string::npos;
    });
    REQUIRE(post != sent.rend());
    if (post == sent.rend()) {
        return;
    }
    REQUIRE(post->images.size() == std::size_t(3));
    if (post->images.size() == std::size_t(3)) {
        CHECK_EQ(post->images[0].field, std::string("image[]"));
        CHECK_EQ(post->images[1].field, std::string("image[]"));
        CHECK_EQ(post->images[2].field, std::string("image[]"));
        CHECK_EQ(post->images[0].sha256, queued_request["images"][0]["sha256"].get<std::string>());
        CHECK_EQ(post->images[1].bytes, transmitted_house);
        CHECK_EQ(post->images[2].bytes, transmitted_swing);
        CHECK_EQ(post->images[1].sha256, queued_request["images"][1]["sha256"].get<std::string>());
        CHECK_EQ(post->images[2].sha256, queued_request["images"][2]["sha256"].get<std::string>());
        CHECK_EQ(post->images[1].content_type, std::string("image/png"));
        CHECK_EQ(post->images[2].content_type, std::string("image/png"));
        CHECK_EQ(post->request_body_bytes, queued_request["request_body_bytes"].get<std::size_t>());
        CHECK_EQ(post->images[1].filename, std::string("image-2.png"));
        CHECK_EQ(post->images[2].filename, std::string("image-3.png"));
    }

    h.value("edit.undo", json::object());
    CHECK_EQ(checksum_of(h), before);
    CHECK_EQ(h.state.doc.layers().size(), std::size_t(1));
    h.value("edit.redo", json::object());
    CHECK_EQ(h.state.doc.composite().checksum_hex(), after.checksum_hex());
    CHECK_EQ(h.state.doc.layers().size(), std::size_t(2));

    const std::string fill_words =
        "Place the swing shown in Image 2 into the selected area of Image 1. Preserve the swing's "
        "design and match perspective, scale, lighting and shadows.";
    h.value("ai.references.add", json{{"paths", json::array({house_path, swing_path})}});
    const json fill_args{{"recipe", "region-fill"}, {"seed", 314},
                         {"vars", {{"prompt", fill_words}, {"context_desc", "the garden"}}}};
    const json fill_preview = h.value("ai.preview", fill_args);
    const json fill_request = fill_preview["request"];
    CHECK_EQ(fill_preview["additional_references"]["transmitted_count"].get<std::size_t>(),
             std::size_t(2));
    CHECK(fill_preview["prompt"].get<std::string>().find(fill_words) != std::string::npos);
    REQUIRE(fill_request["images"].size() == std::size_t(3));
    CHECK_EQ(fill_request["images"][0]["source"].get<std::string>(), std::string("document"));
    CHECK_EQ(fill_request["images"][1]["name"].get<std::string>(), std::string("House.png"));
    CHECK_EQ(fill_request["images"][2]["name"].get<std::string>(), std::string("Swing.png"));
    const std::string before_fill = checksum_of(h);
    const Image before_fill_pixels = h.state.doc.composite();
    const json fill_run = h.value("ai.run", fill_args);
    CHECK_EQ(fill_run["job"]["request"]["blob"].get<std::string>(),
             fill_request["blob"].get<std::string>());
    const json fill_done = h.value("job.wait",
                                   json{{"id", fill_run["job"]["id"]}, {"timeout", 30}});
    CHECK_EQ(fill_done["state"].get<std::string>(), std::string("succeeded"));
    CHECK(fill_done["landing"]["landed"].get<bool>());
    REQUIRE(h.state.doc.layers().size() == std::size_t(3));
    const Image fill_layer = h.state.doc.layer(2).pixels;
    const Image after_fill = h.state.doc.composite();
    std::size_t outside_fill_written = 0;
    for (int y = 0; y < after_fill.size().h; ++y) {
        for (int x = 0; x < after_fill.size().w; ++x) {
            if (fill_layer.at(x, y)[3] == 0 &&
                std::memcmp(after_fill.at(x, y), before_fill_pixels.at(x, y), kChannels) != 0) {
                ++outside_fill_written;
            }
        }
    }
    CHECK_EQ(outside_fill_written, std::size_t(0));
    auto* fill_recorded = dynamic_cast<ai::RecordedTransport*>(h.state.ai.transport.get());
    REQUIRE(fill_recorded != nullptr);
    if (fill_recorded != nullptr) {
        const std::vector<ai::CapturedRequest> fill_sent = fill_recorded->sent();
        const auto fill_post = std::find_if(fill_sent.rbegin(), fill_sent.rend(),
                                             [](const ai::CapturedRequest& item) {
            return item.method == "POST" && item.path.find("/images/edits") != std::string::npos;
        });
        REQUIRE(fill_post != fill_sent.rend());
        if (fill_post != fill_sent.rend()) {
            REQUIRE(fill_post->images.size() == std::size_t(3));
            if (fill_post->images.size() == std::size_t(3)) {
                CHECK_EQ(fill_post->images[0].field, std::string("image[]"));
                CHECK_EQ(fill_post->images[1].field, std::string("image[]"));
                CHECK_EQ(fill_post->images[2].field, std::string("image[]"));
                CHECK_EQ(fill_post->images[1].bytes, transmitted_house);
                CHECK_EQ(fill_post->images[2].bytes, transmitted_swing);
                CHECK_EQ(fill_post->request_body_bytes,
                         fill_request["request_body_bytes"].get<std::size_t>());
            }
        }
    }
    h.value("edit.undo", json::object());
    CHECK_EQ(checksum_of(h), before_fill);
    h.value("edit.redo", json::object());
    CHECK_EQ(h.state.doc.composite().checksum_hex(), after_fill.checksum_hex());
}

void test_a_multi_image_backend_failure_leaves_the_document_untouched() {
    const std::filesystem::path fixtures = std::filesystem::temp_directory_path() /
                                           ("noise-reference-rejection-" +
                                            std::to_string(platform::pid()));
    std::filesystem::remove_all(fixtures);
    std::filesystem::create_directories(fixtures);
    for (const char* name : {"01-models.json", "02-health.json"}) {
        std::filesystem::copy_file(std::filesystem::path(NOISE_TEST_FIXTURES_DIR) / name,
                                   fixtures / name);
    }
    const json refusal{{"name", "multi-image refusal"},
                       {"method", "POST"},
                       {"path", "/images/edits"},
                       {"status", 400},
                       {"json", {{"error", {{"message", "multiple images are unsupported"},
                                               {"code", "multi_image_unsupported"}}}}}};
    const std::string refusal_text = refusal.dump();
    const std::vector<std::uint8_t> refusal_bytes(refusal_text.begin(), refusal_text.end());
    std::string write_error;
    CHECK(io::write_file_atomic((fixtures / "03-multi-image-refusal.json").string(),
                                refusal_bytes, write_error));

    Harness h(Harness::Config::recorded, fixtures.string());
    h.value("doc.new", json{{"width", 64}, {"height", 64},
                             {"fill", json::array({42, 62, 82, 255})}});
    h.value("select.rect", json{{"rect", json::array({24, 24, 16, 16})}});
    const std::string before = checksum_of(h);
    const int undo_before = undo_depth(h);
    const std::string reference_path = h.temp("reference.png");
    CHECK(io::save_png(reference_path, patterned_image(Extent{13, 19}, 91), write_error));
    h.value("ai.references.add", json{{"path", reference_path}});
    CHECK(h.value("server.connect", json::object())["connected"].get<bool>());
    const json run = h.value("ai.run", json{{"recipe", "inpaint-remove"}, {"seed", 55},
                                              {"include_references", true}});
    CHECK_EQ(run["job"]["request"]["image_count"].get<std::size_t>(), std::size_t(2));
    const json done = h.value("job.wait", json{{"id", run["job"]["id"]}, {"timeout", 30}});
    CHECK_EQ(done["state"].get<std::string>(), std::string("failed"));
    CHECK(done["error"].get<std::string>().find("multiple images are unsupported") !=
          std::string::npos);
    CHECK_EQ(checksum_of(h), before);
    CHECK_EQ(undo_depth(h), undo_before);
    CHECK_EQ(h.state.doc.layers().size(), std::size_t(1));
    std::filesystem::remove_all(fixtures);
}

// The whole AI path against the recorded server: discovery, role resolution, the compiled request, the
// queue, and the answer landing through the same compositor door `ai.compose` uses. The fixtures are the
// transport, so this runs in the default `ctest` and still proves the parts no unit test reaches - the
// app's own wiring between them.
void test_a_recipe_runs_against_the_recorded_server_and_lands() {
    Harness h;
    h.value("doc.new", json{{"width", 128}, {"height", 96}, {"fill", json::array({30, 40, 50, 255})}});
    h.value("select.rect", json{{"rect", json::array({24, 20, 40, 28})}});
    const std::string before = checksum_of(h);

    const json connected = h.value("server.connect", json::object());
    CHECK(connected["connected"].get<bool>());
    CHECK_EQ(connected["models"].get<std::size_t>(), std::size_t(7));
    // The role resolves from the table's preference and the box's list together, and the model it lands
    // on is the one the table prefers that this box has downloaded.
    const json roles = h.value("ai.roles", json::object());
    std::string edit_model;
    for (const json& role : roles["roles"]) {
        if (role["role"].get<std::string>() == "edit") {
            edit_model = role["model"].get<std::string>();
            CHECK(role["resolved"].get<bool>());
            CHECK(!edit_model.empty());
        }
    }

    // Remove is asked for nothing, so the only value a caller may overrule is the line the recipe
    // fills for itself until `enhance` does.
    const json vars = json{{"context_desc", "a painted wall"}};
    const json preview = h.value("ai.preview",
                                 json{{"recipe", "inpaint-remove"}, {"vars", vars}, {"seed", 42}});
    const json request = preview["request"];
    CHECK_EQ(request["path"].get<std::string>(), std::string("/images/edits"));
    CHECK_EQ(request["model"].get<std::string>(), edit_model);
    const std::string size = request["size"].get<std::string>();
    CHECK(size.find('x') != std::string::npos);
    const int asked_w = std::stoi(size.substr(0, size.find('x')));
    // The size is the region compiler's, so it is a multiple of the table's 16 and covers the selection
    // plus the context the recipe asked for.
    CHECK_EQ(asked_w % 16, 0);
    CHECK(asked_w >= 40);
    std::string sent_prompt;
    for (const json& pair : request["fields"]) {
        if (pair[0].get<std::string>() == "prompt") {
            sent_prompt = pair[1].get<std::string>();
        }
        CHECK(pair[0].get<std::string>() != "mask");
    }
    CHECK(sent_prompt.find("Remove the marked content") != std::string::npos);
    CHECK(sent_prompt.find("a painted wall") != std::string::npos);
    // The prepped region exists on disk and is what the request names by hash.
    CHECK(std::filesystem::exists(preview["prepped"].get<std::string>()));
    CHECK_EQ(request["image_bytes"].get<std::size_t>() > 0, true);

    // Nothing has been sent yet, and nothing has been written.
    CHECK_EQ(checksum_of(h), before);
    CHECK_EQ(h.value("job.list", json::object())["jobs"].size(), std::size_t(0));

    const json run = h.value("ai.run", json{{"recipe", "inpaint-remove"}, {"vars", vars}, {"seed", 42}});
    const std::string id = run["job"]["id"].get<std::string>();
    CHECK(!id.empty());
    const json waited = h.value("job.wait", json{{"id", id}, {"timeout", 30}});
    CHECK_EQ(waited["state"].get<std::string>(), std::string("succeeded"));
    CHECK_EQ(waited["recipe"].get<std::string>(), std::string("inpaint-remove"));
    // The recording answers 64x64 whatever was asked, which is the case the compositor exists for.
    CHECK_EQ(waited["result_dims"][0].get<int>(), 64);
    CHECK_EQ(waited["result_dims"][1].get<int>(), 64);
    CHECK(std::filesystem::exists(waited["result"].get<std::string>()));

    // `colour_match: false` because the claim here is *where* the answer lands: the recording is a flat
    // field and the frame a flat fill of another colour, so measuring the drift would - correctly - take
    // the answer to the frame's own colour and leave nothing to count. What the correction does is its
    // own test, in `test_compose`.
    // Nobody asked for it: waiting for the job is enough. The answer is on the document already, on a
    // layer named for the verb and the job, and the job says so - which is rule A ("no Apply step")
    // being a fact rather than a promise.
    CHECK(waited["landing"]["landed"].get<bool>());
    CHECK_EQ(waited["landing"]["landed_layer"].get<std::string>(), std::string("Remove " + id));
    const json layers = h.value("layer.list", json::object());
    CHECK_EQ(layers["layers"].size(), std::size_t(2));
    CHECK_EQ(layers["layers"][1]["name"].get<std::string>(), std::string("Remove " + id));
    CHECK(waited["landing"]["report"]["composite"]["composed"].get<bool>());

    // Asking for the same answer again adds nothing: it is the idempotent door, and it answers with the
    // report of the landing that already happened.
    const json applied = h.value("job.apply", json{{"id", id}});
    CHECK(applied["already_landed"].get<bool>());
    CHECK(applied["applied"].get<bool>());
    CHECK(applied["composite"]["composed"].get<bool>());
    const std::size_t changed = applied["composite"]["pixels_changed"].get<std::size_t>();
    CHECK(changed > 0);
    CHECK_NE(checksum_of(h), before);

    // The report describes the document: every byte that differs is one the composite counted, and the
    // answer reached no pixel the job's own coverage did not claim.
    // The document as it was before the answer landed: one flat fill, so the diff below is the composite
    // and nothing else.
    const Image scene = Image::filled(Extent{128, 96}, 30, 40, 50, 255);
    const Image landed = h.state.doc.composite();
    const Rect crop = [&applied] {
        const json& region = applied["region"]["crop"];
        return Rect{region[0].get<int>(), region[1].get<int>(), region[2].get<int>(),
                    region[3].get<int>()};
    }();
    std::size_t different = 0;
    std::size_t outside_crop = 0;
    for (int y = 0; y < landed.size().h; ++y) {
        for (int x = 0; x < landed.size().w; ++x) {
            if (std::memcmp(landed.at(x, y), scene.at(x, y), kChannels) != 0) {
                ++different;
                if (!crop.contains(x, y)) {
                    ++outside_crop;
                }
            }
        }
    }
    // What the report counted and what the document shows are the same pixels or fewer: the recipe's
    // colour match is in force here (auto-landing uses the recipe's policy), and a flat answer matched
    // to a flat frame legitimately changes fewer pixels than were written. What must never happen is a
    // pixel outside the crop differing - the answer reaching somewhere the mask never claimed.
    CHECK(different <= changed);
    CHECK_EQ(outside_crop, std::size_t(0));

    h.value("edit.undo", json::object());
    CHECK_EQ(checksum_of(h), before);

    // A second run of the identical request is a second job, with its own answer and its own layer: the
    // app keeps no answer store to answer you from, so what you get is what you asked for (the request
    // is sent again - `test_backend` asserts that where the transport is visible).
    const std::string first_id = id;
    const json again =
        h.value("ai.run", json{{"recipe", "inpaint-remove"}, {"vars", vars}, {"seed", 42}});
    const json second =
        h.value("job.wait", json{{"id", again["job"]["id"].get<std::string>()}, {"timeout", 30}});
    CHECK(second["state"].get<std::string>() == "succeeded");
    CHECK(second["id"].get<std::string>() != first_id);

    // A job's answer outlives the moment it was made in. The answer landed by itself, undo took the
    // layer away, and asking for the same answer again puts the same pixels back from the file on disk -
    // which is what makes an answer worth keeping. The policy outlives the run too: nothing was passed
    // here, and the landing still says what the recipe asked for and that the correction ran.
    const json reapplied = h.value("job.apply", json{{"id", id}});
    CHECK(!reapplied.contains("already_landed"));
    CHECK_EQ(reapplied["composite"]["pixels_changed"].get<std::size_t>(), changed);
    CHECK(reapplied["policy"]["colour_match"].get<bool>());
    CHECK(reapplied["composite"]["colour_corrected"].get<bool>());
    CHECK_NE(checksum_of(h), before);
}

// Rule A: the answer lands by itself, in the document that asked for it, exactly once. Landing is not a
// mode a caller opts into, so the two things that could go wrong are landing twice and landing in the
// wrong file - both asserted here, plus that an undo is respected instead of fought.
void test_an_answer_lands_itself_into_the_document_that_asked_for_it() {
    Harness h;
    h.value("doc.new", json{{"width", 96}, {"height", 64}, {"fill", json::array({25, 35, 45, 255})}});
    h.value("server.connect", json::object());
    h.value("select.rect", json{{"rect", json::array({16, 10, 40, 24})}});
    const json started = h.value("ai.run", json{{"recipe", "inpaint-remove"}, {"seed", 11}});
    const std::string id = started["job"]["id"].get<std::string>();

    // Every door that reads the queue lands what has finished. `job.get` is enough: no `job.apply`.
    const json waited = h.value("job.wait", json{{"id", id}, {"timeout", 30}});
    CHECK_EQ(waited["state"].get<std::string>(), std::string("succeeded"));
    const std::size_t with_answer = h.value("layer.list", json::object())["layers"].size();
    CHECK_EQ(with_answer, std::size_t(2));

    // Reading the queue again - which the status line does every frame - must not stack a second copy.
    for (int pass = 0; pass < 4; ++pass) {
        h.value("job.list", json::object());
        h.value("ai.intent", json::object());
    }
    CHECK_EQ(h.value("layer.list", json::object())["layers"].size(), with_answer);

    // `Ctrl+Z` is the answer's undo, and the beat does not put the layer back: an auto-lander that
    // undoes an undo is a fight with the person using the app. Asking for it again is how it comes back.
    h.value("edit.undo", json::object());
    CHECK_EQ(h.value("layer.list", json::object())["layers"].size(), std::size_t(1));
    h.value("ai.intent", json::object());
    CHECK_EQ(h.value("layer.list", json::object())["layers"].size(), std::size_t(1));
    CHECK(h.value("job.apply", json{{"id", id}})["composite"].is_object());

    // A different document, the same queue: the answer from the file it came from stays out of it.
    h.value("doc.new", json{{"width", 48}, {"height", 48}, {"fill", json::array({5, 5, 5, 255})}});
    for (int pass = 0; pass < 3; ++pass) {
        h.value("job.list", json::object());
        h.value("ai.intent", json::object());
    }
    CHECK_EQ(h.value("layer.list", json::object())["layers"].size(), std::size_t(1));
    const CommandResult foreign = h.run("job.apply", json{{"id", id}});
    CHECK(!foreign.ok);
    CHECK(foreign.error.find("different document") != std::string::npos);
}

void test_an_answer_lands_on_the_document_and_undoes_completely() {
    Harness h;
    h.value("doc.new", json{{"width", 64}, {"height", 48}, {"fill", json::array({20, 60, 100, 255})}});
    const std::string before = checksum_of(h);
    h.value("select.rect", json{{"rect", json::array({10, 8, 20, 14})}});
    const int marked = undo_depth(h);  // the mark is a step of its own

    // An answer at a size nobody asked for, because that is what comes back: the engine snaps, and
    // the compositor resamples.
    std::string error;
    const Image answer = Image::filled(Extent{320, 240}, 250, 10, 10, 255);
    const std::string answer_path = h.temp("answer.png");
    CHECK(io::save_png(answer_path, answer, error));

    // Flat answer on a flat frame again: colour matching is switched off so that the numbers below
    // count placement, and one test of the correction itself lives next door.
    const json preview =
        h.value("ai.compose", json{{"image", answer_path}, {"colour_match", false}});
    CHECK(preview["composite"]["composed"].get<bool>());
    CHECK(!preview["applied"].get<bool>());
    CHECK(!preview["composite"]["answered_as_requested"].get<bool>());
    const std::size_t changed = preview["composite"]["pixels_changed"].get<std::size_t>();
    CHECK(changed > 0);
    CHECK(preview["composite"]["coverage_pixels"].get<std::size_t>() >= 20u * 14u);
    CHECK(std::filesystem::exists(preview["preview"].get<std::string>()));
    // A preview previews: the document is untouched and no step was taken beyond the mark.
    CHECK_EQ(checksum_of(h), before);
    CHECK_EQ(undo_depth(h), marked);

    const Image scene = h.state.doc.composite();
    const json applied = h.value("ai.compose",
                                json{{"image", answer_path}, {"apply", true}, {"colour_match", false}});
    CHECK(applied["applied"].get<bool>());
    CHECK_EQ(applied["composite"]["pixels_changed"].get<std::size_t>(), changed);
    CHECK_NE(checksum_of(h), before);
    CHECK_EQ(undo_depth(h), marked + 1);

    // The report describes the document rather than an edit that was then fenced: the feather is
    // wider than the selection it was compiled from, and the pixels that differ are the ones the
    // composite counted.
    const Image landed = h.state.doc.composite();
    const Mask& selection = *h.state.doc.selection();
    std::size_t different = 0;
    std::size_t past_selection = 0;
    for (int y = 0; y < landed.size().h; ++y) {
        for (int x = 0; x < landed.size().w; ++x) {
            if (std::memcmp(landed.at(x, y), scene.at(x, y), kChannels) != 0) {
                ++different;
                if (selection.coverage(x, y) == 0) {
                    ++past_selection;
                }
            }
        }
    }
    CHECK_EQ(different, applied["composite"]["pixels_changed"].get<std::size_t>());
    CHECK(past_selection > 0);
    h.value("edit.undo", json::object());
    CHECK_EQ(checksum_of(h), before);
    const std::string redone = [&h] {
        h.value("edit.redo", json::object());
        return checksum_of(h);
    }();
    CHECK_NE(redone, before);

    // The feather the recipe did not name is the one the mask's shape asked for, and it is inside
    // the range the compositor is allowed to work in.
    const json feathered = h.value("ai.compose",
            json{{"image", answer_path}, {"feather", 0}, {"colour_match", false}});
    CHECK_EQ(feathered["composite"]["feather_px"].get<int>(), 0);
    CHECK_EQ(feathered["policy"]["feather"].get<int>(), 0);
    const json autoed = h.value("ai.compose",
            json{{"image", answer_path}, {"feather", "auto"}, {"colour_match", false}});
    CHECK_EQ(autoed["policy"]["feather"].get<std::string>(), "auto");
    CHECK(autoed["composite"]["feather_px"].get<int>() >= 2);
    CHECK(autoed["composite"]["feather_px"].get<int>() <= 8);

    // Refused, and naming the thing: an answer that is not there, a feather that is negative, a
    // word that is not "auto".
    const CommandResult missing = h.run("ai.compose", json{{"image", h.temp("nope.png")}});
    CHECK(!missing.ok);
    CHECK(missing.error.find("nope.png") != std::string::npos);
    const CommandResult negative =
        h.run("ai.compose", json{{"image", answer_path}, {"feather", -3}});
    CHECK(!negative.ok);
    CHECK(negative.error.find("'feather'") != std::string::npos);
    const CommandResult odd_word =
        h.run("ai.compose", json{{"image", answer_path}, {"feather", "soft"}});
    CHECK(!odd_word.ok);
    CHECK(odd_word.error.find("auto") != std::string::npos);
    // None of them touched a pixel: three refusals and the document exactly as the redo left it.
    CHECK_EQ(checksum_of(h), redone);

    // A selection that covers nothing is not a region.
    h.value("select.rect", json{{"rect", json::array({10, 8, 20, 14})}, {"mode", "subtract"}});
    const CommandResult nothing = h.run("ai.compose", json{{"image", answer_path}});
    CHECK(!nothing.ok);
    CHECK(nothing.error.find("covers nothing") != std::string::npos);

    // No selection at all is the whole document, which is what a full-frame recipe means.
    h.value("select.clear", json::object());
    const json full =
        h.value("ai.compose", json{{"image", answer_path}, {"colour_match", false}});
    CHECK(full["composite"]["composed"].get<bool>());
    CHECK(full["region"]["coverage"].get<std::string>().find("whole document") != std::string::npos);
    CHECK(full["composite"]["pixels_changed"].get<std::size_t>() > 0);
}

void test_the_selection_panel_runs_its_intent_through_one_command() {
    Harness h;
    h.value("doc.new", json{{"width", 96}, {"height", 64}, {"fill", json::array({30, 40, 50, 255})}});
    // A frame with something in it, so a landed answer is a visible edit rather than a flat field
    // meeting its own mean.
    h.value("tool.stroke", json{{"points", json::array({json::array({0, 40}), json::array({95, 40})})},
                                {"size", 10.0}, {"hardness", 1.0}, {"opacity", 1.0}, {"flow", 1.0},
                                {"spacing", 0.25}, {"color", json::array({0, 200, 60, 255})}});
    const std::string before = checksum_of(h);
    h.value("select.rect", json{{"rect", json::array({20, 12, 36, 24})}});

    const json read = h.value("ai.intent", json::object());
    CHECK_EQ(read["recipe"].get<std::string>(), std::string("inpaint-remove"));
    std::fprintf(stderr, "  [mark A]\n");
    CHECK_EQ(read["label"].get<std::string>(), std::string("Remove"));
    // Remove asks for nothing: the marked area *is* the instruction, and a box holding the recipe's own
    // guess is a run that spends the box's quota on words nobody typed.
    CHECK(read["ask"].empty());
    // What is on show is what a headless run would send: the recipe's own defaults, and no question.
    CHECK_EQ(read["vars"]["context_desc"].get<std::string>(), std::string("background"));
    std::fprintf(stderr, "  [mark B]\n");
    // Five intents read the marked area and one reads the active layer's own coverage, so the panel has
    // *Blend* to offer whether or not anything is marked. The count is here because a dropped intent
    // would otherwise show up only as a missing row somewhere below.
    CHECK_EQ(read["intents"].size(), std::size_t(6));
    bool offers_blend = false;
    for (const json& offered : read["intents"]) {
        offers_blend = offers_blend || offered["id"].get<std::string>() == "blend";
    }
    CHECK(offers_blend);
    // Reading the action costs an upload encode nothing, and still says what a press would send: the
    // price line is a quote from the region compiler and the model's defaults, not a compiled request.
    CHECK_EQ(read["upload_encodes"].get<std::uint64_t>(), std::uint64_t(0));
    CHECK_EQ(read["quote"]["endpoint"].get<std::string>(), std::string("/images/edits"));
    CHECK(read["quote"]["size"][0].get<int>() >= 36);
    CHECK(read["quote"]["size"][0].get<int>() <= 512);
    CHECK(!read["ready"].get<bool>());
    CHECK_EQ(read["fix"].get<std::string>(), std::string("server.connect"));
    // The panel's handshake, and the flag that keeps it to one attempt: a widget that retried a down
    // server every frame would freeze the window on a connect timeout.
    CHECK(!read["connect_attempted"].get<bool>());
    CHECK(read["configured"].get<bool>());
    std::fprintf(stderr, "  [mark C]\n");
    const json connected = h.value("ai.intent", json{{"connect", true}});
    CHECK(connected["connected"].get<bool>());
    CHECK(connected["connect_attempted"].get<bool>());
    CHECK(connected["ready"].get<bool>());
    const json& bounds = read["selection"]["bounds"];
    CHECK_EQ(bounds[0].get<int>(), 20);
    CHECK_EQ(bounds[2].get<int>(), 36);

    // Refusals name the thing that was wrong, including what *was* offered.
    std::fprintf(stderr, "  [mark D]\n");
    const CommandResult unknown_intent = h.run("ai.intent", json{{"recipe", "spline-reticulate"}});
    CHECK(!unknown_intent.ok);
    CHECK(unknown_intent.error.find("spline-reticulate") != std::string::npos);
    CHECK(unknown_intent.error.find("inpaint-remove") != std::string::npos);
    const CommandResult unknown_var = h.run("ai.intent", json{{"vars", json{{"colour", "pink"}}}});
    CHECK(!unknown_var.ok);
    CHECK(unknown_var.error.find("colour") != std::string::npos);
    CHECK(!h.run("ai.intent", json{{"vars", json{{"target", 3}}}}).ok);
    CHECK(!h.run("ai.intent", json{{"run", "yes"}}).ok);

    // Nothing has been asked for yet, so there is no seed to show: the panel reports a number only once a
    // request carried one.
    CHECK_EQ(read["seed"].get<std::int64_t>(), std::int64_t(0));
    CHECK(!read["seed_pinned"].get<bool>());
    CHECK(!h.run("ai.intent", json{{"seed", -1}}).ok);
    CHECK(!h.run("ai.intent", json{{"seed_pin", -1}}).ok);
    CHECK(!h.run("ai.intent", json{{"seed_pin", "yesterday"}}).ok);

    std::fprintf(stderr, "  [mark E]\n");
    h.value("server.connect", json::object());
    // The one thing a caller may still overrule on Remove is a line the recipe fills for itself.
    const json with_words = h.value("ai.intent", json{{"vars", json{{"context_desc", "a painted wall"}}}});
    CHECK(with_words["ready"].get<bool>());
    CHECK(!with_words["swap"].get<bool>());   // nothing was resident, so nothing has to change

    // The exact request is a thing a person asks for - `ai.preview` - and asking for it is what encodes
    // the upload. Nothing on the frame path does.
    const json exact = h.value("ai.preview", json{{"recipe", "inpaint-remove"},
                                                 {"vars", json{{"context_desc", "a painted wall"}}}});
    CHECK_EQ(exact["request"]["path"].get<std::string>(), std::string("/images/edits"));
    CHECK(exact["prompt"].get<std::string>().find("Remove the marked content") != std::string::npos);
    CHECK(exact["prompt"].get<std::string>().find("a painted wall") != std::string::npos);
    const std::uint64_t encoded = h.state.upload_encodes;
    for (int pass = 0; pass < 10; ++pass) {
        h.value("ai.intent", json::object());
    }
    CHECK_EQ(h.state.upload_encodes, encoded);

    // Holding a seed is its own argument, because a seed on a press belongs to that press alone. Held, it
    // changes what every later ask sends and nothing about the compile.
    std::fprintf(stderr, "  [mark F]\n");
    const json held = h.value("ai.intent", json{{"seed_pin", 1234}});
    CHECK_EQ(held["seed"].get<std::int64_t>(), std::int64_t(1234));
    CHECK(held["seed_pinned"].get<bool>());
    CHECK_EQ(h.value("server.state", json::object())["seed"].get<std::int64_t>(), std::int64_t(1234));
    const json seeded_preview = h.value("ai.preview", json{{"recipe", "inpaint-remove"}});
    CHECK_EQ(seeded_preview["request"]["seed"].get<std::int64_t>(), std::int64_t(1234));
    CHECK_EQ(seeded_preview["request"]["size"].get<std::string>(),
             exact["request"]["size"].get<std::string>());
    h.value("ai.intent", json{{"seed_pin", nullptr}});
    CHECK(!h.value("server.state", json::object())["seed_pinned"].get<bool>());

    // The compile is a function of the document and the words, and of nothing the app drew at random:
    // the same marked region twice uploads the same bytes, by hash.
    const json first_compile = h.value("ai.preview", json{{"recipe", "inpaint-remove"},
                                                          {"vars", json{{"context_desc", "a painted wall"}}}});
    const json twice_compile = h.value("ai.preview", json{{"recipe", "inpaint-remove"},
                                                          {"vars", json{{"context_desc", "a painted wall"}}}});
    CHECK_EQ(first_compile["request"]["blob"].get<std::string>(),
             twice_compile["request"]["blob"].get<std::string>());
    CHECK_EQ(first_compile["request"]["size"].get<std::string>(),
             twice_compile["request"]["size"].get<std::string>());

    // The quote is not an estimate of the request, it is the request's own endpoint and size - so there
    // is nothing to go stale, and nothing to re-check after the region moves.
    const json grown = h.value("ai.preview", json{{"recipe", "inpaint-remove"}});
    const std::string grown_size = grown["request"]["size"].get<std::string>();
    const std::string::size_type cross = grown_size.find('x');
    const json shown = h.value("ai.intent", json::object())["quote"];
    CHECK_EQ(shown["endpoint"].get<std::string>(), grown["request"]["path"].get<std::string>());
    CHECK_EQ(shown["size"][0].get<int>(), std::stoi(grown_size.substr(0, cross)));
    CHECK_EQ(shown["size"][1].get<int>(), std::stoi(grown_size.substr(cross + 1)));

    std::fprintf(stderr, "  [mark G]\n");
    // Switching verb keeps what the next one asks and forgets what it does not, so an instruction
    // cannot ride silently into an operation that never asked for one. Asked fields have no defaults,
    // so the box shows either what was typed or nothing.
    h.value("ai.intent", json{{"recipe", "inpaint-replace"}});
    CHECK(!h.value("ai.intent", json::object())["vars"].contains("instruction"));
    h.value("ai.intent", json{{"vars", json{{"instruction", "a floor lamp"}}}});
    CHECK_EQ(h.value("ai.intent", json::object())["vars"]["instruction"].get<std::string>(),
             std::string("a floor lamp"));
    // And the forgetting is real: Remove asks for nothing, so the words typed for Change go rather
    // than wait somewhere for its return. Asked fields have no default to fall back to, so coming back
    // to Change shows an empty box - which is the point of asking.
    h.value("ai.intent", json{{"recipe", "inpaint-remove"}});
    CHECK(!h.value("ai.intent", json::object())["vars"].contains("instruction"));
    h.value("ai.intent", json{{"recipe", "inpaint-replace"}});
    CHECK(!h.value("ai.intent", json::object())["vars"].contains("instruction"));

    // A swap is priced before it happens, not discovered afterwards: load a different model, and Run
    // refuses with both names and the word that would let it through.
    std::fprintf(stderr, "  [mark H]\n");
    h.value("server.load", json{{"model", "Krea-v0.3.1-INT8"}});
    const json swapped = h.value("ai.intent", json{{"recipe", "inpaint-remove"}});
    CHECK(swapped["swap"].get<bool>());
    CHECK_EQ(swapped["resident"].get<std::string>(), std::string("Krea-v0.3.1-INT8"));
    const CommandResult unconfirmed = h.run("ai.intent", json{{"run", true}});
    CHECK(!unconfirmed.ok);
    CHECK(unconfirmed.error.find("Krea-v0.3.1-INT8") != std::string::npos);
    CHECK(unconfirmed.error.find("confirm") != std::string::npos);
    // The panel can show that refusal, and nothing ran: no job, no pixel, no undo step.
    CHECK(h.value("ai.intent", json::object())["error"].get<std::string>().find("confirm") !=
          std::string::npos);
    CHECK_EQ(h.value("job.list", json::object())["jobs"].size(), std::size_t(0));
    CHECK_EQ(checksum_of(h), before);

    // And the free refusal comes first. With a swap pending *and* an asked-for field blank, the person
    // is told about the field: `confirm` cannot buy a request that has no words in it, and a swap that
    // loads a model to send nothing is the worst possible outcome of a bare key press. The recorded
    // server always has the right model resident, so only the live box could find this - and it did.
    h.value("ai.intent", json{{"recipe", "inpaint-replace"}});
    const CommandResult blank_with_swap = h.run("ai.intent", json{{"run", true}});
    CHECK(!blank_with_swap.ok);
    CHECK(blank_with_swap.error.find("'instruction'") != std::string::npos);
    CHECK(blank_with_swap.error.find("confirm") == std::string::npos);
    const CommandResult confirmed_blank = h.run("ai.intent", json{{"run", true}, {"confirm", true}});
    CHECK(!confirmed_blank.ok);
    CHECK(confirmed_blank.error.find("'instruction'") != std::string::npos);
    CHECK_EQ(h.value("job.list", json::object())["jobs"].size(), std::size_t(0));
    h.value("ai.intent", json{{"recipe", "inpaint-remove"}});

    // The confirmed press, then the answer landed the way the panel's Apply lands it.
    const json started = h.value("ai.intent", json{{"run", true}, {"confirm", true}});
    const std::string id = started["job"]["id"].get<std::string>();
    CHECK(!id.empty());
    const json settled = h.value("job.wait", json{{"id", id}, {"timeout", 30}});
    CHECK_EQ(settled["state"].get<std::string>(), std::string("succeeded"));
    // In flight when the press returned, however the worker had scheduled the microsecond.
    CHECK(started["job"]["state"].get<std::string>() == "running" ||
          started["job"]["state"].get<std::string>() == "queued");
    // The job the status line shows is this job: the same record the landing reads.
    CHECK_EQ(h.value("ai.intent", json::object())["job"]["id"].get<std::string>(), id);

    const json applied = h.value("job.apply", json{{"id", id}});
    CHECK(applied["composite"]["composed"].get<bool>());
    CHECK(applied["composite"]["pixels_changed"].get<std::size_t>() > 0);
    CHECK_NE(checksum_of(h), before);
    h.value("edit.undo", json::object());
    CHECK_EQ(checksum_of(h), before);
}

// An option the caller leaves out is not a "no". The composite policy comes from the table, and an
// argument reader that defaults a flag here would switch off the colour correction every edit recipe
// asks for while still reporting a composite that worked - which is exactly how it behaved, on the
// live server, with green unit tests.
// The state a person arrives in when they installed the editor and set nothing: the answer names the
// variable, and the panel knows there is no button that could work.
// Two asks over the same pixels have to be able to give two answers. The seed is what the model
// explores from, so a constant seed makes every press restate the first one - and each press is a GPU
// charge. Entropy arrives through a seam, because a test that drew real bits could not say what it
// proved: the panel pins a number when the number is the point, and lets go.
void test_each_ask_takes_its_own_seed_and_a_pin_holds_one() {
    Harness h;
    h.value("doc.new", json{{"width", 64}, {"height", 48}, {"fill", json::array({30, 40, 50, 255})}});
    h.value("select.rect", json{{"rect", json::array({14, 14, 36, 20})}});
    h.value("server.connect", json::object());

    std::vector<std::uint64_t> drawn{11, 12, 13, 14};
    std::size_t taken = 0;
    h.state.ai.seed_draw = [&drawn, &taken] { return drawn.at(taken++); };

    const json first = h.value("ai.preview", json{{"recipe", "inpaint-remove"}});
    const json second = h.value("ai.preview", json{{"recipe", "inpaint-remove"}});
    CHECK_EQ(first["request"]["seed"].get<std::uint64_t>(), std::uint64_t(11));
    CHECK_EQ(second["request"]["seed"].get<std::uint64_t>(), std::uint64_t(12));
    // The upload is the same bytes both times, so the seed is the only thing that moved: two answers
    // from one region, not two regions.
    CHECK_EQ(first["request"]["blob"].get<std::string>(), second["request"]["blob"].get<std::string>());
    CHECK_EQ(first["seed_source"].get<std::string>(), std::string("random"));

    // A read is not an ask. The panel reads the action every frame and may not burn seeds doing it.
    const std::size_t after_two_asks = taken;
    for (int pass = 0; pass < 5; ++pass) {
        h.value("ai.intent", json::object());
    }
    CHECK_EQ(taken, after_two_asks);

    // A held seed pins every later ask, which is how someone iterates on an answer they liked. A seed on
    // a press is not that: `ai_job_probe` found a verb press quietly holding the session's seed, which
    // made every ask after it the same request - so the two are different arguments.
    h.value("ai.intent", json{{"seed_pin", 777}});
    const json pinned = h.value("ai.preview", json{{"recipe", "inpaint-remove"}});
    CHECK_EQ(pinned["request"]["seed"].get<std::uint64_t>(), std::uint64_t(777));
    CHECK_EQ(pinned["seed_source"].get<std::string>(), std::string("pinned"));
    CHECK(h.value("server.state", json::object())["seed_pinned"].get<bool>());
    CHECK_EQ(h.value("ai.preview", json{{"recipe", "inpaint-remove"}})["request"]["seed"]
                 .get<std::uint64_t>(),
             std::uint64_t(777));
    CHECK_EQ(taken, after_two_asks);

    // A seed named on the call wins over the pin, and takes no draw: the number on the reply is the
    // number that went out, and where it came from travels with it.
    const json named = h.value("ai.preview", json{{"recipe", "inpaint-remove"}, {"seed", 5}});
    CHECK_EQ(named["request"]["seed"].get<std::uint64_t>(), std::uint64_t(5));
    CHECK_EQ(named["seed_source"].get<std::string>(), std::string("named"));
    CHECK_EQ(taken, after_two_asks);

    // Letting go returns to one seed per ask.
    h.value("ai.intent", json{{"seed_pin", nullptr}});
    CHECK(!h.value("server.state", json::object())["seed_pinned"].get<bool>());
    CHECK_EQ(h.value("ai.preview", json{{"recipe", "inpaint-remove"}})["request"]["seed"]
                 .get<std::uint64_t>(),
             std::uint64_t(13));

    // A seed the field cannot carry is refused here rather than sent for the engine to misread: the
    // upload form parses it as a signed 32-bit integer and answers http 400 `must be an integer`.
    // The draw is 64-bit entropy and the field is not, so the mask happens on the way out: whatever the
    // entropy source returns, the number that reaches the engine is one it accepts.
    const CommandResult too_big =
        h.run("ai.preview", json{{"recipe", "inpaint-remove"}, {"seed", 4294967296LL}});
    CHECK(!too_big.ok);
    CHECK(too_big.error.find("seed") != std::string::npos);
    CHECK(too_big.error.find("4294967296") != std::string::npos);
    CHECK(!h.run("ai.preview", json{{"recipe", "inpaint-remove"}, {"seed", -1}}).ok);

    // A press that names a seed leaves nothing behind: the ask after it draws again. This is the exact
    // shape of the bug the probe caught, where the verb's seed became the session's from then on.
    const json pressed = h.value("ai.remove", json{{"seed", 99}});
    CHECK(pressed.contains("job"));
    CHECK(!h.value("server.state", json::object())["seed_pinned"].get<bool>());
    CHECK_EQ(h.value("ai.preview", json{{"recipe", "inpaint-remove"}})["request"]["seed"]
                 .get<std::uint64_t>(),
             std::uint64_t(14));
    h.value("job.cancel", json{{"id", pressed["job"]["id"].get<std::string>()}});

    // The draw is 64-bit entropy and the field is not, so the mask happens on the way out: whatever the
    // entropy source returns, the number that reaches the engine is one it accepts.
    h.state.ai.seed_draw = [] { return 0xFFFFFFFFFFFFFFFFull; };
    CHECK_EQ(h.value("ai.preview", json{{"recipe", "inpaint-remove"}})["request"]["seed"]
                 .get<std::uint64_t>(),
             noise::ai::kMaxSeed);
}

// The size a region is asked for is its own size, so far as the model can take it. A request shrunk to
// a budget the model never stated comes back resampled over the crop: the same picture, softer, which a
// person reads as a Change that did nothing. The ceiling is the model's published `soft_pixel_cap`, and
// passing it is said out loud rather than left to be inferred from blur.
void test_a_region_is_asked_at_its_own_size_and_says_when_it_is_not() {
    const auto asked_size = [](const json& compiled) {
        const std::string size = compiled["request"]["size"].get<std::string>();
        const std::size_t x = size.find('x');
        return std::pair{std::stoi(size.substr(0, x)), std::stoi(size.substr(x + 1))};
    };

    Harness h;
    h.value("doc.new",
            json{{"width", 1440}, {"height", 1080}, {"fill", json::array({30, 40, 50, 255})}});
    h.value("select.rect", json{{"rect", json::array({200, 200, 1000, 700})}});
    h.value("server.connect", json::object());
    const int multiple = h.state.ai.roles.size_policy.multiple;

    // The case that was reported: a mark over a full-HD canvas. Its grown crop is the canvas - 1.56 MP,
    // inside the 2 MP the models on this box publish - so the ask is the crop, bar the grid the engine
    // snaps to anyway, and nothing is thrown away.
    // The ceiling the compile used is on the wire beside the recipe that used it: the model's published
    // `soft_pixel_cap`, not a number the reader has to remember.
    bool reported = false;
    const json recipes_table = h.value("ai.recipes", json::object());
    for (const json& row : recipes_table["recipes"]) {
        if (row["id"].get<std::string>() == "inpaint-remove") {
            reported = true;
            CHECK_EQ(row["region_max_area_px"].get<int>(), 2097152);
        }
    }
    CHECK(reported);

    const json inside = h.value("ai.preview", json{{"recipe", "inpaint-remove"}});
    CHECK(!inside["region"]["resampled"].get<bool>());
    // And the price line says the same thing before the press, not only after it.
    CHECK(!h.value("ai.intent", json{{"recipe", "inpaint-remove"}})["quote"]["resampled"].get<bool>());
    CHECK_NEAR(inside["region"]["scale"].get<double>(), 1.0, 1e-12);
    const int crop_w = inside["region"]["crop"][2].get<int>();
    const int crop_h = inside["region"]["crop"][3].get<int>();
    const auto [asked_w, asked_h] = asked_size(inside);
    CHECK(asked_w >= crop_w && asked_w - crop_w < multiple);
    CHECK(asked_h >= crop_h && asked_h - crop_h < multiple);

    // Past the ceiling the shrink is real, and the plan says so instead of leaving it to be noticed as
    // softness. Scaled by area, so the shape of the region survives the trip.
    Harness wide;
    wide.value("doc.new",
               json{{"width", 2400}, {"height", 1600}, {"fill", json::array({30, 40, 50, 255})}});
    wide.value("select.rect", json{{"rect", json::array({0, 0, 2400, 1600})}});
    wide.value("server.connect", json::object());
    const json over = wide.value("ai.preview", json{{"recipe", "inpaint-remove"}});
    CHECK(over["region"]["resampled"].get<bool>());
    CHECK(wide.value("ai.intent", json{{"recipe", "inpaint-remove"}})["quote"]["resampled"].get<bool>());
    CHECK(over["region"]["scale"].get<double>() < 1.0);
    const auto [shrunk_w, shrunk_h] = asked_size(over);
    CHECK_NEAR(static_cast<double>(shrunk_w) / static_cast<double>(shrunk_h), 1.5, 0.05);
    // The ask stays inside the ceiling rather than being snapped back over it on the grid.
    CHECK(shrunk_w * shrunk_h <= 2097152);

    // And the landing - the path that actually puts the answer on the document, not the preview - says
    // so too. A plan rebuilt from the job record used to come back unscaled, so the sentence about the
    // rescale only appeared when someone composited by hand.
    const json pressed = wide.value("ai.remove", json::object());
    REQUIRE(pressed.contains("job"));
    const std::string job_id = pressed["job"]["id"].get<std::string>();
    const json settled = wide.value("job.wait", json{{"id", job_id}, {"timeout", 30}});
    CHECK_EQ(settled["state"].get<std::string>(), std::string("succeeded"));
    const json record = wide.value("job.get", json{{"id", job_id}});
    CHECK(record["landing"]["resampled"].get<bool>());
    const std::string log_line = wide.value("log.tail", json{{"count", 40}}).dump();
    CHECK(log_line.find("rescaled to") != std::string::npos);
    CHECK(log_line.find("pixel ceiling") != std::string::npos);
}

void test_an_unconfigured_server_names_the_variable() {
    Harness h{Harness::Config::unconfigured};
    const CommandResult refused = h.run("server.connect", json::object());
    CHECK(!refused.ok);
    CHECK(refused.error.find("THENOISE_URL") != std::string::npos);

    const json read = h.value("ai.intent", json{{"recipe", "inpaint-remove"}});
    CHECK(!read["configured"].get<bool>());
    CHECK(!read["connected"].get<bool>());
    // A connect ask in this state reports the same refusal instead of inventing a transport error.
    const json asked = h.value("ai.intent", json{{"connect", true}});
    CHECK(!asked["connected"].get<bool>());
    CHECK(asked["connect_attempted"].get<bool>());
    CHECK(asked["error"].get<std::string>().find("THENOISE_URL") != std::string::npos);
}

void test_an_option_left_out_keeps_the_policy_the_table_asked_for() {
    Harness h;
    h.value("doc.new", json{{"width", 64}, {"height", 48}, {"fill", json::array({20, 60, 100, 255})}});
    // A frame with structure, because a flat frame is a degenerate ring: matching a flat answer to a
    // flat ring reproduces the ring exactly and the composite legitimately changes nothing. Two bands
    // of paint give the correction a mean and a spread to work against, and a frame that a corrected
    // patch still has to land on.
    h.value("tool.stroke", json{{"points", json::array({json::array({0, 5}), json::array({63, 5})})},
                                {"size", 8.0}, {"hardness", 1.0}, {"opacity", 1.0}, {"flow", 1.0},
                                {"spacing", 0.25}, {"color", json::array({0, 210, 40, 255})}});
    h.value("tool.stroke", json{{"points", json::array({json::array({0, 42}), json::array({63, 42})})},
                                {"size", 8.0}, {"hardness", 1.0}, {"opacity", 1.0}, {"flow", 1.0},
                                {"spacing", 0.25}, {"color", json::array({130, 20, 20, 255})}});
    h.value("select.rect", json{{"rect", json::array({14, 14, 36, 20})}});
    std::string error;
    const std::string answer_path = h.temp("cast.png");
    CHECK(io::save_png(answer_path, Image::filled(Extent{160, 120}, 250, 10, 10, 255), error));

    const auto landed = [&h](const json& args) {
        const json reply = h.value("ai.compose", args);
        const Image after = h.state.doc.composite();
        std::uint64_t red = 0;
        std::size_t pixels = 0;
        for (int y = 18; y < 30; ++y) {  // the middle of the write, past the feather
            for (int x = 20; x < 44; ++x) {
                red += after.at(x, y)[0];
                ++pixels;
            }
        }
        h.value("edit.undo", json::object());
        return std::make_pair(reply, red / pixels);
    };

    const auto [left_out, matched] =
        landed(json{{"image", answer_path}, {"apply", true}});
    CHECK(left_out["policy"]["colour_match"].get<bool>());
    CHECK(left_out["composite"]["colour_corrected"].get<bool>());
    CHECK(left_out["composite"]["pixels_changed"].get<std::size_t>() > 0);

    const auto [switched_off, uncorrected] =
        landed(json{{"image", answer_path}, {"apply", true}, {"colour_match", false}});
    CHECK(!switched_off["policy"]["colour_match"].get<bool>());
    CHECK(!switched_off["composite"]["colour_corrected"].get<bool>());
    CHECK(switched_off["composite"]["pixels_changed"].get<std::size_t>() > 0);

    // The answer is (250, 10, 10) and the frame around it is cold paint: matching pulls the landed
    // patch most of the way to the ring, and leaving it alone lands what the model sent.
    CHECK(uncorrected > 240u);
    CHECK(matched < uncorrected - 100u);
    // Same pixels claimed either way: the correction changes colour, not where the write goes.
    CHECK_EQ(left_out["composite"]["coverage_pixels"].get<std::size_t>(),
             switched_off["composite"]["coverage_pixels"].get<std::size_t>());
}

// The flagship sequence as one key press: mark an area, press it, the answer arrives as a layer. The
// verb list is what a window would draw, so this also asserts the surface a person sees - the words on
// the buttons, the keys they answer to, and that nothing has to be typed in between.
// Choosing not to run must cost nothing. The window reads `ai.intent` on every frame - it is the status
// line - so the read path may plan geometry and read tables, but it may not prep a region, resample it
// or encode a PNG. `AppState::upload_encodes` counts the one place that does, so this asserts the cost
// rather than the answer.
void test_choosing_not_to_run_costs_no_upload_encode() {
    Harness h;
    h.value("doc.new", json{{"width", 160}, {"height", 110}, {"fill", json::array({30, 40, 50, 255})}});
    h.value("server.connect", json::object());
    h.value("select.rect", json{{"rect", json::array({20, 12, 36, 24})}});

    const std::uint64_t before = h.state.upload_encodes;
    for (int step = 0; step < 25; ++step) {
        h.value("select.rect", json{{"rect", json::array({20 + step, 12, 36 + step, 24})}});
        for (int reader = 0; reader < 3; ++reader) {
            h.value("ai.intent", json::object());
        }
    }
    CHECK_EQ(h.state.upload_encodes, before);

    // And what it says instead is worth something: the endpoint, the size the region compiler settled
    // on, and the model that would answer.
    const json quote = h.value("ai.intent", json::object())["quote"];
    CHECK_EQ(quote["endpoint"].get<std::string>(), std::string("/images/edits"));
    CHECK(quote["size"][0].get<int>() > 0);
    CHECK(quote["size"][1].get<int>() > 0);

    // Asking for the exact request is what encodes, once. Running encodes once more, no more.
    h.value("ai.preview", json{{"recipe", "inpaint-remove"}});
    CHECK_EQ(h.state.upload_encodes, before + 1);
    h.value("ai.run", json{{"recipe", "inpaint-remove"}, {"seed", 3}});
    CHECK_EQ(h.state.upload_encodes, before + 2);
}

// The aspect is a choice about geometry, so it shows up in the price the status line quotes - and quoting
// it must stay free, because a reader runs every frame. The rectangle's own geometry is `test_region`.
void test_an_aspect_choice_prices_the_frame_it_will_ask_for() {
    Harness h;
    h.value("doc.new", json{{"width", 1024}, {"height", 1024}, {"fill", json::array({30, 40, 50, 255})}});
    h.value("server.connect", json::object());

    const json plain = h.value("ai.intent", json{{"recipe", "new-image"}});
    CHECK_EQ(plain["aspects"].size(), std::size_t(4));
    CHECK(plain["aspect"].is_null());
    CHECK_EQ(plain["quote"]["size"][0].get<int>(), 1024);
    CHECK_EQ(plain["quote"]["size"][1].get<int>(), 1024);

    // `upload_encodes` counts every region prep and PNG encode a run does; a read must not move it.
    const std::uint64_t encodes = h.state.upload_encodes;
    const std::vector<AspectRatio> wanted{AspectRatio{16, 9}, AspectRatio{4, 3},
                                          AspectRatio{3, 4}, AspectRatio{1, 1}};
    for (const AspectRatio& ratio : wanted) {
        const json chosen = h.value("ai.intent", json{{"recipe", "new-image"},
                                                     {"aspect", aspect_name(ratio)}});
        CHECK_EQ(chosen["aspect"].get<std::string>(), aspect_name(ratio));
        const int w = chosen["quote"]["size"][0].get<int>();
        const int h_again = chosen["quote"]["size"][1].get<int>();
        // One axis is the canvas, the other the ratio, and the ratio is what was pressed rather than
        // something the snap invented.
        CHECK(w == 1024 || h_again == 1024);
        CHECK_NEAR(static_cast<double>(w) / static_cast<double>(h_again),
                   static_cast<double>(ratio.w) / static_cast<double>(ratio.h), 0.02);
    }
    CHECK_EQ(h.state.upload_encodes, encodes);

    // Putting the choice back means the canvas again.
    const json back = h.value("ai.intent", json{{"aspect", nullptr}});
    CHECK(back["aspect"].is_null());
    CHECK_EQ(back["quote"]["size"][1].get<int>(), 1024);

    // A frame the recipe does not offer is refused with the frames it does.
    const CommandResult unoffered =
        h.run("ai.intent", json{{"recipe", "new-image"}, {"aspect", "21:9"}});
    CHECK(!unoffered.ok);
    CHECK(unoffered.error.find("1:1, 3:4, 4:3, 16:9") != std::string::npos);
    CHECK(h.value("ai.intent", json::object())["aspect"].is_null());

    // And a recipe that offers no choice at all says so rather than accepting the key and ignoring it.
    const CommandResult elsewhere =
        h.run("ai.intent", json{{"recipe", "inpaint-remove"}, {"aspect", "16:9"}});
    CHECK(!elsewhere.ok);
    CHECK(elsewhere.error.find("offers no aspect") != std::string::npos);
}

// The frame has to reach three places: the size in the request, the region a report describes, and the
// pixels - everything outside it byte for byte what the document had. The pixel-lock claim, on a region
// that covers less than the whole canvas.
void test_a_frame_reaches_the_request_and_leaves_the_canvas_outside_it_alone() {
    Harness h;
    h.value("doc.new", json{{"width", 256}, {"height", 256}, {"fill", json::array({30, 40, 50, 255})}});
    h.value("server.connect", json::object());
    h.value("ai.intent", json{{"recipe", "new-image-anime"},
                              {"vars", {{"prompt", "a red cube on a shelf"}}},
                              {"aspect", "16:9"}});

    // `new-image-anime` because the committed recordings answer a generation for the illustration role
    // (`15-generate-any-size.json`) and the default run opens no socket.
    const json preview = h.value("ai.preview", json{{"recipe", "new-image-anime"},
                                                   {"vars", {{"prompt", "a red cube on a shelf"}}},
                                                   {"aspect", "16:9"}});
    const json& crop = preview["region"]["crop"];
    // 256 wide, so 16:9 is 256x144 centred: the frame is the region, not a letterbox inside it.
    CHECK_EQ(crop[0].get<int>(), 0);
    CHECK_EQ(crop[1].get<int>(), 56);
    CHECK_EQ(crop[2].get<int>(), 256);
    CHECK_EQ(crop[3].get<int>(), 144);
    CHECK(preview["region"]["coverage"].get<std::string>().find("16:9") != std::string::npos);
    // The outgoing `size` is the frame's, and there is no "aspect" field for the policy to send.
    bool size_as_frame = false;
    for (const json& field : preview["request"]["fields"]) {
        if (field[0].get<std::string>() == "size") {
            size_as_frame = field[1].get<std::string>() == "256x144";
        }
        CHECK(field[0].get<std::string>() != "aspect");
    }
    CHECK(size_as_frame);

    const std::string before = checksum_of(h);
    const json run = h.value("ai.run", json{{"recipe", "new-image-anime"},
                                           {"vars", {{"prompt", "a red cube on a shelf"}}},
                                           {"aspect", "16:9"}});
    const std::string id = run["job"]["id"].get<std::string>();
    const json done = h.value("job.wait", json{{"id", id}, {"timeout", 30}});
    CHECK_EQ(done["state"].get<std::string>(), std::string("succeeded"));
    CHECK(done["landing"]["landed"].get<bool>());
    // A layer of its own, named for the job, above the one the region was flattened from.
    const json layers = h.value("layer.list", json::object());
    CHECK_EQ(layers["layers"].size(), std::size_t(2));
    CHECK(layers["layers"][1]["name"].get<std::string>().find(id) != std::string::npos);

    const Rect frame{crop[0].get<int>(), crop[1].get<int>(), crop[2].get<int>(), crop[3].get<int>()};
    const Image scene = Image::filled(Extent{256, 256}, 30, 40, 50, 255);
    const Image landed = h.state.doc.composite();
    std::size_t outside = 0;
    std::size_t inside = 0;
    for (int y = 0; y < landed.size().h; ++y) {
        for (int x = 0; x < landed.size().w; ++x) {
            if (std::memcmp(landed.at(x, y), scene.at(x, y), kChannels) == 0) {
                continue;
            }
            if (frame.contains(x, y)) {
                ++inside;
            } else {
                ++outside;
            }
        }
    }
    CHECK_EQ(outside, std::size_t(0));
    CHECK(inside > 0);

    h.value("edit.undo", json::object());
    CHECK_EQ(checksum_of(h), before);
}

// "New image from an idea" end to end, over the wire: the kind list is the recipe family's rows, the
// chosen kind is the recipe the job runs, and the size typed is the canvas. Create is one command, so
// what the button does and what a driver does cannot be two different products.
void test_the_new_dialog_creates_the_document_and_the_idea_it_holds() {
    Harness h;
    h.value("server.connect", json::object());

    const json form = h.value("new_document", json::object());
    CHECK_EQ(form["kind"].get<std::string>(), std::string("new-image"));
    CHECK(form["kinds"].size() >= 2);
    CHECK_EQ(form["kinds"][0]["id"].get<std::string>(), std::string("new-image"));
    CHECK(!form["kinds"][0]["label"].get<std::string>().empty());

    // A recipe from another family is not a kind of this dialog, and the refusal says which ones are.
    const CommandResult wrong = h.run("new_document", json{{"kind", "inpaint-remove"}});
    CHECK(!wrong.ok);
    CHECK(wrong.error.find("inpaint-remove") != std::string::npos);
    CHECK(wrong.error.find("new-image-anime") != std::string::npos);

    h.value("front_door", json{{"show", "new"}});
    h.value("new_document", json{{"width", 256},
                                 {"height", 256},
                                 {"prompt", "a red cube on a shelf"},
                                 {"kind", "new-image-anime"}});
    const json created = h.value("front_door.create", json::object());
    CHECK_EQ(h.state.doc.width(), 256);
    CHECK_EQ(h.state.doc.height(), 256);
    CHECK(created["asked"].get<bool>());
    CHECK_EQ(h.value("app.state", json::object())["front_door"]["front_door"].get<std::string>(),
             std::string("closed"));
    const std::string id = created["job"]["id"].get<std::string>();
    CHECK_EQ(h.value("job.get", json{{"id", id}})["recipe"].get<std::string>(),
             std::string("new-image-anime"));

    // No words, no request: the dialog still makes the canvas it was given.
    h.value("new_document", json{{"prompt", ""}});
    const json blank = h.value("front_door.create", json::object());
    CHECK_EQ(blank["asked"].get<bool>(), false);
    CHECK(!blank.contains("job"));
    CHECK_EQ(h.state.doc.width(), 256);
}

// What a picked path *does* is a command, so the routing is testable without a desktop: the words a
// dialog answers with and the words a probe sends have to do the same thing, or the button and the
// wire are two doors again.
void test_a_picked_path_does_what_its_file_set_promised() {
    Harness h;
    h.value("doc.new", json{{"width", 24}, {"height", 16}});
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "noise-app-answer-test";
    std::filesystem::create_directories(dir);
    std::string error;
    const std::filesystem::path picture = dir / "lamp.png";
    CHECK(io::save_png(picture.string(), Image::filled(Extent{6, 8}, 210, 20, 60, 255), error));

    // "image" is the Insert button: the picture on a layer of its own, centred, because a dialog answer
    // has no pointer to put it under.
    h.value("file.answer", json{{"for", "image"}, {"path", picture.string()}});
    REQUIRE(h.state.doc.layers().size() == 2);
    CHECK_EQ(h.state.doc.layer(1).name, std::string("lamp"));
    CHECK_EQ(h.state.doc.layer(1).pixels.at(9, 4)[0], std::uint8_t(210));
    CHECK_EQ(h.state.doc.layer(1).pixels.at(8, 4)[3], std::uint8_t(0));

    // "project" is the document's own file, "flat" one flattened picture, "open" a file read back in.
    const std::filesystem::path project = dir / "scene.noise";
    h.value("file.answer", json{{"for", "project"}, {"path", project.string()}});
    CHECK(std::filesystem::exists(project));
    CHECK_EQ(h.state.doc.path(), project.string());

    const std::filesystem::path exported = dir / "flat.png";
    h.value("file.answer", json{{"for", "flat"}, {"path", exported.string()}});
    CHECK(std::filesystem::exists(exported));

    h.value("layer.add", json{{"name", "spare"}});
    h.value("file.answer", json{{"for", "open"}, {"path", project.string()}});
    CHECK_EQ(h.state.doc.path(), project.string());
    CHECK_EQ(h.state.doc.layers().size(), std::size_t(2));

    const CommandResult wrong =
        h.run("file.answer", json{{"for", "socket"}, {"path", project.string()}});
    CHECK(!wrong.ok);
    CHECK(wrong.error.find("project") != std::string::npos);
    CHECK(wrong.error.find("image") != std::string::npos);
}

// A dropped picture is the desktop handing over a path, so the command that answers one is tested the
// same way a click would drive it: the layer takes the file's name, the middle of the picture goes
// where the pointer was, and a drop that fell whole off the canvas is refused rather than parked at 0,0.
void test_a_dropped_picture_lands_where_the_pointer_was() {
    Harness h;
    h.value("doc.new", json{{"width", 32}, {"height", 24}});
    const std::filesystem::path dir =
        std::filesystem::temp_directory_path() / "noise-app-import-test";
    std::filesystem::create_directories(dir);
    const std::filesystem::path file = dir / "streetlight.png";
    const Image source = Image::filled(Extent{8, 10}, 200, 30, 40, 255);
    std::string error;
    CHECK(io::save_png(file.string(), source, error));

    const json landed =
        h.value("layer.import", json{{"path", file.string()}, {"at", json::array({20, 10})}});
    CHECK_EQ(landed["name"].get<std::string>(), std::string("streetlight"));
    CHECK_EQ(landed["placed"][0].get<int>(), 16);
    CHECK_EQ(landed["placed"][1].get<int>(), 5);
    CHECK_EQ(h.state.doc.layers().size(), std::size_t(2));

    const Image& layer = h.state.doc.layer(1).pixels;
    CHECK(std::memcmp(layer.at(16, 5), source.at(0, 0), kChannels) == 0);
    CHECK(std::memcmp(layer.at(23, 14), source.at(7, 9), kChannels) == 0);
    // The rest of the layer is still the transparent nothing a new layer starts as: an imported
    // rectangle is a rectangle, not an opaque sheet over everything under it.
    CHECK_EQ(layer.at(0, 0)[3], std::uint8_t(0));
    CHECK_EQ(layer.at(31, 23)[3], std::uint8_t(0));

    // One undo takes the layer and its picture together, as it does for an answer.
    h.value("edit.undo", json::object());
    CHECK_EQ(h.state.doc.layers().size(), std::size_t(1));

    const CommandResult off =
        h.run("layer.import", json{{"path", file.string()}, {"at", json::array({900, 900})}});
    CHECK(!off.ok);
    CHECK(off.error.find("streetlight.png") != std::string::npos);
    CHECK(off.error.find("outside") != std::string::npos);
    CHECK_EQ(h.state.doc.layers().size(), std::size_t(1));

    const CommandResult not_a_picture = h.run(
        "layer.import", json{{"path", (dir / "nope.png").string()}, {"at", json::array({4, 4})}});
    CHECK(!not_a_picture.ok);
    CHECK(not_a_picture.error.find("nope.png") != std::string::npos);

    // Without the point there is nowhere honest to put it, and the origin is not the answer.
    const CommandResult nowhere = h.run("layer.import", json{{"path", file.string()}});
    CHECK(!nowhere.ok);
    CHECK(nowhere.error.find("'at'") != std::string::npos);
}

// `New` then a prompt: the canvas is empty because nothing has ever been put on it, and the answer is
// the picture the person asked for, so it fills that canvas instead of arriving on a second layer
// above an invisible one. The bytes are what the layer above would have held - there was nothing
// underneath to composite against - and one `Ctrl+Z` gives the blank canvas back.
void test_an_answer_on_a_blank_canvas_becomes_the_canvas() {
    Harness h;
    h.value("doc.new", json{{"width", 256}, {"height", 256}});
    h.value("server.connect", json::object());
    const std::string before = checksum_of(h);
    h.value("ai.intent", json{{"recipe", "new-image-anime"},
                              {"vars", {{"prompt", "a red cube on a shelf"}}}});

    const json run = h.value("ai.run", json{{"recipe", "new-image-anime"},
                                           {"vars", {{"prompt", "a red cube on a shelf"}}}});
    const json done = h.value("job.wait", json{{"id", run["job"]["id"].get<std::string>()},
                                               {"timeout", 30}});
    CHECK_EQ(done["state"].get<std::string>(), std::string("succeeded"));
    CHECK_EQ(h.state.doc.layers().size(), std::size_t(1));

    const Image& base = h.state.doc.layer(0).pixels;
    std::size_t painted = 0;
    for (int y = 0; y < base.size().h; ++y) {
        for (int x = 0; x < base.size().w; ++x) {
            if (base.at(x, y)[3] != 0) {
                ++painted;
            }
        }
    }
    CHECK(painted > 0);

    h.value("edit.undo", json::object());
    CHECK_EQ(checksum_of(h), before);
    CHECK_EQ(h.state.doc.layers().size(), std::size_t(1));
}

// The style list is the recipe table's and the choice is the intent's, so the key has to follow it: a
// panel that offered "illustration" while `Ctrl+G` ran the head recipe would be two doors again.
void test_a_style_choice_moves_which_recipe_the_key_runs() {
    Harness h;
    h.value("doc.new", json{{"width", 128}, {"height", 128}, {"fill", json::array({10, 20, 30, 255})}});
    h.value("server.connect", json::object());

    // Bound the reply, then range: ranging over a subobject of a temporary walks freed memory.
    const auto generate_row = [&h] {
        const json rows = h.value("ai.verbs", json::object());
        for (const json& verb : rows["verbs"]) {
            if (verb["id"].get<std::string>() == "generate") {
                return verb;
            }
        }
        return json::object();
    };

    const json row = generate_row();
    CHECK_EQ(row["variants"].size(), std::size_t(2));
    CHECK_EQ(row["variants"][0]["id"].get<std::string>(), std::string("new-image-photo"));
    CHECK_EQ(row["active"].get<std::string>(), std::string("new-image"));

    // Choosing a style selects it and runs nothing: no job, no encode.
    const std::uint64_t encodes = h.state.upload_encodes;
    const json chosen = h.value("ai.intent", json{{"recipe", "new-image-anime"},
                                                  {"vars", {{"prompt", "a lighthouse at dusk"}}}});
    CHECK_EQ(chosen["recipe"].get<std::string>(), std::string("new-image-anime"));
    CHECK(chosen["job"].is_null());
    CHECK_EQ(h.state.upload_encodes, encodes);
    CHECK_EQ(generate_row()["active"].get<std::string>(), std::string("new-image-anime"));

    // The press carries nothing and still runs the style that is showing.
    const json ran = h.value("ai.generate", json::object());
    REQUIRE(!ran["job"].is_null());
    CHECK_EQ(ran["job"]["recipe"].get<std::string>(), std::string("new-image-anime"));
    CHECK_EQ(ran["verb"].get<std::string>(), std::string("generate"));

    // Back to the head and the press follows it back: the family belongs to the row, not to whichever
    // member was last chosen.
    h.value("ai.intent", json{{"recipe", "new-image"}});
    CHECK_EQ(generate_row()["active"].get<std::string>(), std::string("new-image"));
}

void test_the_remove_key_runs_the_whole_sequence_with_no_words() {
    Harness h;
    h.value("doc.new", json{{"width", 96}, {"height", 64}, {"fill", json::array({30, 40, 50, 255})}});
    h.value("tool.stroke", json{{"points", json::array({json::array({0, 40}), json::array({95, 40})})},
                                {"size", 10.0}, {"hardness", 1.0}, {"opacity", 1.0}, {"flow", 1.0},
                                {"spacing", 0.25}, {"color", json::array({0, 200, 60, 255})}});
    h.value("server.connect", json::object());

    // Nothing marked yet. The three verbs that work on a marked area say so, and the press refuses:
    // a wordless Remove over an unmarked frame is not "remove nothing", it is "remove everything".
    const json unmarked = h.value("ai.verbs", json::object());
    for (const json& row : unmarked["verbs"]) {
        const bool needs_mark = row["scope"].get<std::string>() == "selection";
        CHECK_EQ(row["ready"].get<bool>(), !needs_mark);
        if (needs_mark) {
            CHECK(row["reason"].get<std::string>().find("works on a selection") != std::string::npos);
        }
    }
    const CommandResult nothing_marked = h.run("ai.remove", json{{"seed", 7}});
    CHECK(!nothing_marked.ok);
    CHECK(nothing_marked.error.find("works on a selection") != std::string::npos);
    CHECK_EQ(h.state.doc.layers().size(), std::size_t(1));

    h.value("select.rect", json{{"rect", json::array({20, 12, 36, 24})}});
    const json verbs = h.value("ai.verbs", json::object());
    CHECK(verbs["error"].get<std::string>().empty());
    // The count is a precondition, not a claim about the table's shape: a row that vanished would make
    // every `row_of` below answer null and fail twenty checks at once.
    REQUIRE(verbs["verbs"].size() == std::size_t(6));
    // Rows are named, not counted from the top: the order is what a person meets, and a row added at
    // the front is not a reason for every assertion below to move.
    const auto row_of = [&verbs](const char* id) {
        for (const json& row : verbs["verbs"]) {
            if (row["id"].get<std::string>() == id) {
                return row;
            }
        }
        return json(nullptr);
    };
    const json remove = row_of("remove");
    CHECK_EQ(remove["id"].get<std::string>(), std::string("remove"));
    CHECK_EQ(remove["command"].get<std::string>(), std::string("ai.remove"));
    CHECK_EQ(remove["recipe"].get<std::string>(), std::string("inpaint-remove"));
    CHECK_EQ(remove["label"].get<std::string>(), std::string("Remove"));
    // No words, and the key the table itself reports: the window prints this binding, it does not
    // own one.
    CHECK(remove["ask"].empty());
    CHECK_EQ(h.table.shortcuts_for("ai.remove"), std::vector<std::string>{"Ctrl+Backspace"});
    CHECK_EQ(remove["key"].get<std::string>(), std::string("Ctrl+Backspace"));
    CHECK_EQ(row_of("fill")["ask"], (std::vector<std::string>{"prompt"}));
    CHECK_EQ(row_of("change")["label"].get<std::string>(), std::string("Change"));
    CHECK_EQ(row_of("generate")["scope"].get<std::string>(), std::string("document"));
    // Heal is a verb of the marked area: no words asked, and the row the Fix button mirrors.
    CHECK_EQ(row_of("heal")["recipe"].get<std::string>(), std::string("heal"));
    CHECK(row_of("heal")["ask"].empty());
    CHECK_EQ(h.table.shortcuts_for("ai.heal"), std::vector<std::string>{"Ctrl+H"});
    // *Blend* is the third scope: neither the mark nor the frame, but the active layer's own alpha. It
    // asks for nothing - the whole point is the press with no words - and it is not on the bar, which
    // hangs off a mark it does not read.
    CHECK_EQ(row_of("blend")["recipe"].get<std::string>(), std::string("blend"));
    CHECK_EQ(row_of("blend")["label"].get<std::string>(), std::string("Blend"));
    CHECK(row_of("blend")["ask"].empty());
    CHECK_EQ(row_of("blend")["scope"].get<std::string>(), std::string("layer"));
    CHECK_EQ(row_of("blend")["operation"].get<std::string>(), std::string("in_place"));
    CHECK_EQ(h.table.shortcuts_for("ai.blend"), std::vector<std::string>{"Ctrl+B"});
    // The bar's rule in one number: every row whose recipe reads the marked area is on it, and the one
    // that makes pixels from words is not.
    int on_the_bar = 0;
    for (const json& row : verbs["verbs"]) {
        on_the_bar += row["scope"].get<std::string>() == "selection" ? 1 : 0;
    }
    CHECK_EQ(on_the_bar, 4);
    for (const json& row : verbs["verbs"]) {
        CHECK(row["ready"].get<bool>());
    }

    // The bar hangs off the mark, and has no opinion about geometry the frame has not drawn yet.
    const json bar = h.value("verb.bar", json::object());
    CHECK(bar["shown"].get<bool>());
    CHECK_EQ(bar["mark"], json::array({20, 12, 36, 24}));
    CHECK_EQ(bar["buttons"].size(), std::size_t(4));
    CHECK_EQ(bar["buttons"][0]["command"].get<std::string>(), std::string("ai.heal"));
    CHECK_EQ(bar["screen"], json::array({0, 0, 0, 0}));
    // Only a verb that wants words gets a box: the two silent ones run on the press, and a box for
    // them would be an empty field with a button on it.
    json fields;
    for (const json& button : bar["buttons"]) {
        fields[button["verb"].get<std::string>()] = button["field"];
    }
    CHECK(fields["heal"].is_null());
    CHECK(fields["remove"].is_null());
    CHECK_EQ(fields["fill"].get<std::string>(), std::string("prompt"));
    CHECK_EQ(fields["change"].get<std::string>(), std::string("instruction"));

    // The box opens over the mark and takes the intent with it, so what is typed belongs to the verb
    // the button was pressed for.
    CHECK(bar["ask"].is_null());
    const json opened = h.value("verb.ask", json{{"show", "fill"}});
    CHECK_EQ(opened["verb"].get<std::string>(), std::string("fill"));
    CHECK_EQ(opened["field"].get<std::string>(), std::string("prompt"));
    CHECK_EQ(opened["command"].get<std::string>(), std::string("ai.fill"));
    CHECK_EQ(opened["recipe"].get<std::string>(), std::string("region-fill"));
    CHECK_EQ(h.value("ai.intent", json::object())["recipe"].get<std::string>(),
             std::string("region-fill"));
    CHECK_EQ(h.value("verb.bar", json::object())["ask"]["verb"].get<std::string>(),
             std::string("fill"));
    // Words typed into the box are the intent's, so the press the panel makes and the press the box
    // makes are the same command reading the same value.
    h.value("ai.intent", json{{"recipe", "region-fill"},
                              {"vars", {{"prompt", "a brass door handle"}}}});
    CHECK_EQ(h.value("verb.ask", json{{"show", "fill"}})["value"].get<std::string>(),
             std::string("a brass door handle"));
    CHECK(h.value("verb.ask", json{{"show", "close"}}).is_null());
    CHECK(h.value("verb.bar", json::object())["ask"].is_null());

    // A verb that asks nothing has no box, and the refusal says why rather than opening an empty one.
    const CommandResult silent = h.run("verb.ask", json{{"show", "remove"}});
    CHECK(!silent.ok);
    CHECK(silent.error.find("asks for nothing") != std::string::npos);
    const CommandResult nonsense = h.run("verb.ask", json{{"show", "sideways"}});
    CHECK(!nonsense.ok);
    CHECK(nonsense.error.find("fill") != std::string::npos);

    h.value("select.clear", json::object());
    const json nothing = h.value("verb.bar", json::object());
    CHECK(!nothing["shown"].get<bool>());
    CHECK(nothing["mark"].is_null());
    CHECK(nothing["buttons"].empty());
    h.value("select.rect", json{{"rect", json::array({20, 12, 36, 24})}});

    const std::string before = checksum_of(h);
    const json ran = h.value("ai.remove", json{{"seed", 7}});
    CHECK_EQ(ran["verb"].get<std::string>(), std::string("remove"));
    REQUIRE(!ran["job"].is_null());
    const std::string id = ran["job"]["id"].get<std::string>();
    const json waited = h.value("job.wait", json{{"id", id}, {"timeout", 30}});
    CHECK_EQ(waited["state"].get<std::string>(), std::string("succeeded"));

    // Waiting was the last thing anyone did: the answer is already a layer, named for the verb and the
    // job - the whole interaction, with no box.
    REQUIRE(h.state.doc.layers().size() == 2);
    CHECK_EQ(h.state.doc.layer(1).name, std::string("Remove ") + id);
    CHECK_NE(checksum_of(h), before);
    const std::size_t landed_layers = h.state.doc.layers().size();
    // The door is still there for a driver, and it is idempotent: it adds no layer and says so.
    const json applied = h.value("job.apply", json{{"id", id}});
    CHECK(applied["already_landed"].get<bool>());
    CHECK(applied["composite"]["composed"].get<bool>());
    CHECK_EQ(h.state.doc.layers().size(), landed_layers);

    // The on-canvas bar is now in result-mask editing mode; opening the next edit is explicit.
    CHECK_EQ(h.value("verb.bar", json::object())["mode"].get<std::string>(),
             std::string("editing"));
    h.value("verb.new_edit", json::object());
    // The other verbs keep their one question: the same press on Fill refuses rather than guessing
    // what the marked area should become.
    const CommandResult wordless = h.run("ai.fill", json{{"seed", 7}});
    CHECK(!wordless.ok);
    CHECK(wordless.error.find("region-fill") != std::string::npos);
    CHECK(wordless.error.find("'prompt'") != std::string::npos);
    CHECK_EQ(h.state.doc.layers().size(), landed_layers);

    // An unknown verb is not a command, and the table says so with the ids that are.
    const CommandResult bogus = h.run("ai.bleach", json::object());
    CHECK(!bogus.ok);
    CHECK(bogus.error.find("ai.bleach") != std::string::npos);
}

// A marked-area edit is one deliberate cycle: queue -> locked buttons -> editable result ->
// explicit rearm. The controller follows the actual landing rather than the HTTP result alone, and
// never lets a shortcut silently queue a second edit while the user's brush is aimed at a layer mask.
void test_selection_edit_mode_follows_the_job_and_the_result_mask() {
    Harness h;
    h.value("doc.new", json{{"width", 96}, {"height", 64},
                            {"fill", json::array({30, 40, 50, 255})}});
    h.value("server.connect", json::object());
    h.value("select.rect", json{{"rect", json::array({20, 12, 36, 24})}});
    const json before = h.value("verb.bar", json::object());
    CHECK_EQ(before["mode"].get<std::string>(), std::string("actions"));
    REQUIRE(before["buttons"].size() == std::size_t(4));
    const json started = h.value("ai.remove", json{{"seed", 11}});
    const std::string id = started["job"]["id"].get<std::string>();
    const json busy = h.value("verb.bar", json::object());
    CHECK_EQ(busy["mode"].get<std::string>(), std::string("generating"));
    CHECK_EQ(busy["job"].get<std::string>(), id);
    CHECK(busy["shown"].get<bool>());
    for (const json& row : busy["buttons"]) {
        CHECK(!row["ready"].get<bool>());
        CHECK(row["reason"].get<std::string>().find("generating") != std::string::npos);
    }
    CHECK(!h.run("ai.remove", json::object()).ok);
    CHECK(!h.run("verb.ask", json{{"show", "fill"}}).ok);
    CHECK(!h.run("verb.new_edit", json::object()).ok);
    CHECK_EQ(h.value("job.list", json::object())["jobs"].size(), std::size_t(1));

    const json finished = h.value("job.wait", json{{"id", id}, {"timeout", 30}});
    CHECK_EQ(finished["state"].get<std::string>(), std::string("succeeded"));
    CHECK(finished["landing"]["landed"].get<bool>());
    REQUIRE(h.state.doc.active() == 1);
    REQUIRE(h.state.active_layer().mask.has_value());
    CHECK_EQ(h.value("coverage.target", json::object())["target"].get<std::string>(),
             std::string("mask"));
    const json editing = h.value("verb.bar", json::object());
    CHECK_EQ(editing["mode"].get<std::string>(), std::string("editing"));
    CHECK(editing["shown"].get<bool>());
    CHECK(editing["buttons"].empty());
    CHECK(editing["ask"].is_null());
    CHECK(!h.run("ai.heal", json::object()).ok);
    CHECK(!h.run("verb.ask", json{{"show", "change"}}).ok);
    // Selecting another layer must not quietly rebind the result edit to that layer's mask.
    h.value("layer.select", json{{"index", 0}});
    CHECK_EQ(h.value("verb.bar", json::object())["mode"].get<std::string>(),
             std::string("editing"));
    h.value("layer.select", json{{"index", 1}});

    const std::string unchanged = checksum_of(h);
    const json retained_selection = h.value("select.get", json::object());
    const json new_edit = h.value("verb.new_edit", json::object());
    CHECK_EQ(new_edit["mode"].get<std::string>(), std::string("actions"));
    CHECK_EQ(h.value("coverage.target", json::object())["target"].get<std::string>(),
             std::string("marked"));
    CHECK_EQ(checksum_of(h), unchanged);
    CHECK_EQ(h.value("select.get", json::object()), retained_selection);
    const json rearmed = h.value("verb.bar", json::object());
    CHECK_EQ(rearmed["mode"].get<std::string>(), std::string("actions"));
    CHECK_EQ(rearmed["buttons"].size(), std::size_t(4));
    CHECK(rearmed["buttons"][0]["ready"].get<bool>());
    CHECK(!h.run("verb.new_edit", json::object()).ok);

    // The next edit builds on the result layer, not on a new empty one.
    const json again = h.value("ai.remove", json{{"seed", 12}});
    const json second = h.value("job.wait", json{{"id", again["job"]["id"]},
                                              {"timeout", 30}});
    CHECK_EQ(second["state"].get<std::string>(), std::string("succeeded"));
    REQUIRE(h.state.doc.layers().size() == std::size_t(3));
    CHECK_EQ(h.value("verb.bar", json::object())["mode"].get<std::string>(),
             std::string("editing"));

    // Clearing the selection does not strand New AI edit: the bar anchors to the result mask.
    h.value("select.clear", json::object());
    CHECK(h.value("verb.bar", json::object())["shown"].get<bool>());
    h.value("verb.new_edit", json::object());
    CHECK(!h.value("verb.bar", json::object())["shown"].get<bool>());
    h.value("select.rect", json{{"rect", json::array({5, 5, 12, 10})}});
    CHECK(h.value("verb.bar", json::object())["shown"].get<bool>());

    // A new document invalidates the old edit even when its dimensions are identical.
    h.value("doc.new", json{{"width", 96}, {"height", 64}});
    CHECK_EQ(h.value("verb.bar", json::object())["mode"].get<std::string>(),
             std::string("actions"));

    // Undo removes the result layer, so the stale session must not stay in mask mode. Redo
    // restores its pixels but not the obsolete mode: the next edit has to be deliberate.
    Harness undo;
    undo.value("doc.new", json{{"width", 96}, {"height", 64},
                               {"fill", json::array({30, 40, 50, 255})}});
    undo.value("server.connect", json::object());
    undo.value("select.rect", json{{"rect", json::array({20, 12, 36, 24})}});
    const json to_undo = undo.value("ai.remove", json{{"seed", 13}});
    undo.value("job.wait", json{{"id", to_undo["job"]["id"]}, {"timeout", 30}});
    CHECK_EQ(undo.value("verb.bar", json::object())["mode"].get<std::string>(),
             std::string("editing"));
    undo.value("edit.undo", json::object());
    CHECK_EQ(undo.state.doc.layers().size(), std::size_t(1));
    CHECK_EQ(undo.value("verb.bar", json::object())["mode"].get<std::string>(),
             std::string("actions"));
    CHECK_EQ(undo.value("coverage.target", json::object())["target"].get<std::string>(),
             std::string("marked"));
    undo.value("edit.redo", json::object());
    CHECK_EQ(undo.state.doc.layers().size(), std::size_t(2));
    CHECK_EQ(undo.value("verb.bar", json::object())["mode"].get<std::string>(),
             std::string("actions"));
}

// Fill and Change are the two verbs with a word in them, so their sequence is mark, type, press. What
// is asserted is not that a box can hold text but that the words reach the request the job actually
// sent, that the answer arrives on a layer named for the verb, and that the press of a key which
// carries no arguments at all - the only kind a keyboard can send - runs with the words the intent was
// given. That last one is how the panel works: its box commits and then presses bare.
void test_the_typed_verbs_carry_their_words_to_their_own_layer() {
    struct Typed {
        const char* name;
        const char* recipe;
        const char* field;
        const char* label;
        const char* words;
    };
    // The field each recipe asks for, and the label its answer's layer is named for - both read from
    // the recipe rather than restated, so a rename in the table cannot silently pass.
    const std::vector<Typed> verbs{
        {"fill", "region-fill", "prompt", "Fill", "a brass reading lamp"},
        {"change", "inpaint-replace", "instruction", "Change", "the marker as a waxing moon"},
    };

    Harness h;
    h.value("doc.new", json{{"width", 96}, {"height", 64}, {"fill", json::array({30, 40, 50, 255})}});
    h.value("tool.stroke", json{{"points", json::array({json::array({0, 40}), json::array({95, 40})})},
                                {"size", 10.0}, {"hardness", 1.0}, {"opacity", 1.0}, {"flow", 1.0},
                                {"spacing", 0.25}, {"color", json::array({0, 200, 60, 255})}});
    h.value("server.connect", json::object());
    h.value("select.rect", json{{"rect", json::array({20, 12, 36, 24})}});

    for (const Typed& verb : verbs) {
        const std::string recipe = verb.recipe;
        const std::string field = verb.field;
        const std::string command = std::string("ai.") + verb.name;
        const std::string scene = checksum_of(h);
        const Image before = h.state.doc.composite();
        const std::size_t layers_before = h.state.doc.layers().size();

        // A word given where this recipe never asked for one is no words: the refusal names the field it
        // was handed as well as the one it wanted, so a driver cannot file `prompt` against Change and
        // get an answer to a different question.
        const std::string other = field == "prompt" ? "instruction" : "prompt";
        json misfiled{{other, verb.words}};
        json misfiled_args{{"vars", misfiled}, {"seed", 9}};
        const CommandResult wrong_field = h.run(command, misfiled_args);
        CHECK(!wrong_field.ok);
        CHECK(wrong_field.error.find(other) != std::string::npos);
        CHECK(wrong_field.error.find(recipe) != std::string::npos);
        CHECK_EQ(h.state.doc.layers().size(), layers_before);

        // The panel's door exactly: the box commits the words, then the key - which carries nothing -
        // presses. A commit that selected the recipe without filing the words, or words filed against a
        // recipe the press then refused to run, both fail here.
        json given{{field, verb.words}};
        h.value("ai.intent", json{{"recipe", recipe}, {"vars", given}});
        const json ran = h.value(command, json{{"seed", 9}});
        CHECK_EQ(ran["verb"].get<std::string>(), std::string(verb.name));
        const std::string id = ran["job"]["id"].get<std::string>();
        const json settled = h.value("job.wait", json{{"id", id}, {"timeout", 30}});
        CHECK_EQ(settled["state"].get<std::string>(), std::string("succeeded"));
        CHECK_EQ(settled["recipe"].get<std::string>(), recipe);

        // The words are in the request that went out, not only in the box: the job's own record answers.
        const std::string prompt = settled["request"]["prompt"].get<std::string>();
        CHECK(prompt.find(verb.words) != std::string::npos);
        CHECK(prompt.find("{{") == std::string::npos);
        CHECK_EQ(settled["request"]["path"].get<std::string>(), std::string("/images/edits"));

        // Rule A, per verb: nobody pressed anything after the wait, and the answer is a layer of its own
        // named for what the recipe is called.
        CHECK(settled["landing"]["landed"].get<bool>());
        CHECK_EQ(settled["landing"]["landed_layer"].get<std::string>(),
                 std::string(verb.label) + " " + id);
        REQUIRE(h.state.doc.layers().size() == layers_before + 1);
        CHECK_EQ(h.state.doc.layer(layers_before).name, std::string(verb.label) + " " + id);

        // Nothing reached a pixel the compiled region did not claim, and the layer the region was
        // flattened from kept every byte it had.
        const json& crop = settled["landing"]["report"]["region"]["crop"];
        const Rect box{crop[0].get<int>(), crop[1].get<int>(), crop[2].get<int>(), crop[3].get<int>()};
        const Image after = h.state.doc.composite();
        std::size_t outside = 0;
        for (int y = 0; y < after.size().h; ++y) {
            for (int x = 0; x < after.size().w; ++x) {
                if (std::memcmp(after.at(x, y), before.at(x, y), kChannels) != 0 &&
                    !box.contains(x, y)) {
                    ++outside;
                }
            }
        }
        CHECK_EQ(outside, std::size_t(0));
        CHECK_NE(checksum_of(h), scene);

        h.value("verb.new_edit", json::object());
        // One `Ctrl+Z` takes the answer and the layer holding it together, back to the scene.
        h.value("edit.undo", json::object());
        CHECK_EQ(h.state.doc.layers().size(), layers_before);
        CHECK_EQ(checksum_of(h), scene);
    }
}

// A verb row is a promise about a recipe. The check runs at startup and gates every verb command,
// because the alternative is a button that reads "Remove" while sending something else.
void test_a_verb_that_stops_matching_its_recipe_is_refused_at_startup() {
    Harness h;
    CHECK(h.state.ai.verbs_error.empty());

    // The recipe behind a key stopped being that kind of operation.
    ai::RecipeSet changed = h.state.ai.recipes;
    for (ai::Recipe& recipe : changed.recipes) {
        if (recipe.id == "inpaint-remove") {
            recipe.operation = ai::Operation::in_place;
        }
    }
    const std::string why = app::verb_table_error(changed, app::kVerbs);
    CHECK(why.find("remove") != std::string::npos);
    CHECK(why.find("inpaint-remove") != std::string::npos);
    CHECK(why.find("reconstruct") != std::string::npos);

    // An ask that grew: a Remove that demands a word is not the key that promises zero typing.
    ai::RecipeSet talkative = h.state.ai.recipes;
    for (ai::Recipe& recipe : talkative.recipes) {
        if (recipe.id == "inpaint-remove") {
            recipe.ask.push_back("target");
        }
    }
    CHECK(app::verb_table_error(talkative, app::kVerbs).find("asking for 1 field(s)") != std::string::npos);

    // A recipe that went away.
    ai::RecipeSet emptied;
    CHECK(app::verb_table_error(emptied, app::kVerbs).find("does not define") != std::string::npos);

    // Two keys, one policy.
    const std::vector<app::VerbInfo> twins{
        {app::VerbId::remove, "inpaint-remove", "Ctrl+Backspace"},
        {app::VerbId::fill, "inpaint-remove", "Ctrl+Shift+F"}};
    CHECK(app::verb_table_error(h.state.ai.recipes, twins).find("two keys") != std::string::npos);

    // And the gate itself: a broken table stops a press, with the sentence the table was refused for.
    h.state.ai.verbs_error = "the verb 'remove' drives recipe 'gone', which the recipe table does not define";
    const CommandResult refused = h.run("ai.remove", json::object());
    CHECK(!refused.ok);
    CHECK(refused.error.find("does not define") != std::string::npos);
    const json verbs = h.value("ai.verbs", json::object());
    CHECK_EQ(verbs["error"].get<std::string>(), h.state.ai.verbs_error);
    for (const json& row : verbs["verbs"]) {
        CHECK(!row["ready"].get<bool>());
    }
}

// Words an operation asked for are not optional at the door: an empty instruction is no instruction,
// and the model would invent one on the box's time. Asserted against **every** recipe in the table that
// asks for something, in the field that recipe names, so a recipe added tomorrow cannot run on words it
// made up - and so a recipe whose `ask` was dropped by accident is caught rather than silently sent
// blank. A refusal costs nothing at the door: no job, and no upload encoded to discover it was refused.
void test_every_recipe_that_asks_for_words_refuses_without_them() {
    Harness h;
    h.value("doc.new", json{{"width", 64}, {"height", 48}, {"fill", json::array({20, 40, 60, 255})}});
    h.value("select.rect", json{{"rect", json::array({8, 6, 24, 16})}});
    h.value("server.connect", json::object());

    std::size_t asked_recipes = 0;
    // Hoisted, because `h.value(...)` returns by value: ranging over a subobject of that temporary
    // would walk memory the loop had already freed.
    const json table = h.value("ai.recipes", json::object());
    for (const json& row : table["recipes"]) {
        if (row["ask"].empty()) {
            continue;
        }
        ++asked_recipes;
        const std::string recipe = row["id"].get<std::string>();
        const std::string field = row["ask"][0].get<std::string>();
        const std::string quoted = "'" + field + "'";

        const CommandResult wordless = h.run("ai.run", json{{"recipe", recipe}, {"seed", 3}});
        CHECK(!wordless.ok);
        CHECK(wordless.error.find(recipe) != std::string::npos);
        CHECK(wordless.error.find(quoted) != std::string::npos);

        // A box of spaces is the same refusal: nothing was said.
        json blank_vars{{field, "   "}};
        const CommandResult blank = h.run(
            "ai.run", json{{"recipe", recipe}, {"vars", blank_vars}, {"seed", 3}});
        CHECK(!blank.ok);
        CHECK(blank.error.find("whitespace") != std::string::npos);

        // The refusal happens at the rendering of the prompt, which is before the upload is built, so
        // being turned away costs neither a request nor an encode.
        CHECK_EQ(h.value("job.list", json::object())["jobs"].size(), std::size_t(0));
        CHECK_EQ(h.state.upload_encodes, std::uint64_t(0));
    }
    // A vacuous loop would be the worst kind of green: the shipped table asks in five places.
    CHECK_EQ(asked_recipes, std::size_t(5));

    const CommandResult wordless = h.run("ai.run", json{{"recipe", "region-fill"}, {"seed", 3}});
    CHECK(!wordless.ok);
    CHECK(wordless.error.find("region-fill") != std::string::npos);
    CHECK(wordless.error.find("'prompt'") != std::string::npos);

    const json filled =
        h.value("ai.preview", json{{"recipe", "region-fill"},
                                   {"vars", json{{"prompt", "a brass reading lamp"}}}, {"seed", 3}});
    std::string prompt;
    for (const json& pair : filled["request"]["fields"]) {
        if (pair[0].get<std::string>() == "prompt") {
            prompt = pair[1].get<std::string>();
        }
    }
    CHECK(prompt.find("a brass reading lamp") != std::string::npos);
    CHECK(prompt.find("{{") == std::string::npos);

    // Remove asks for nothing, so the same door runs it with no `vars` at all - the flagship sequence
    // has no step where a person has to type.
    const json wordless_remove = h.value("ai.preview", json{{"recipe", "inpaint-remove"}, {"seed", 3}});
    CHECK(wordless_remove["request"]["path"].get<std::string>() == "/images/edits");
}

// An answer arrives on **its own layer**, above the one the region was flattened from, and the pixels the
// layer under it never had are exactly what it is there to fill. There is no "into the active layer"
// option any more, so the claims that option existed to make are gone and two better ones take their
// place: the write is bounded by the coverage and by nothing else, and one undo removes the answer
// together with the layer it came on.
void test_an_answer_lands_on_its_own_layer_over_another_one() {
    Harness h;
    h.value("doc.new", json{{"width", 48},
                            {"height", 32},
                            {"fill", json::array({30, 40, 50, 255})}});
    h.value("select.rect", json{{"rect", json::array({10, 8, 16, 10})}});
    const std::string base_document = checksum_of(h);
    const int marked = undo_depth(h);  // the mark is a step; the answer must add exactly one more
    const std::size_t layers_before = h.state.doc.layers().size();

    std::string error;
    const std::string answer_path = h.temp("generated.png");
    CHECK(io::save_png(answer_path, Image::filled(Extent{96, 64}, 210, 30, 90, 255), error));

    const json landed = h.value("ai.compose", json{{"image", answer_path}, {"apply", true}});
    CHECK_EQ(landed["layer"].get<int>(), 1);
    // Named for what it is, in the layer list a person reads afterwards.
    CHECK(landed["layer_name"].get<std::string>().rfind("answer ", 0) == 0);
    CHECK_EQ(h.state.doc.layer(1).name, landed["layer_name"].get<std::string>());
    CHECK(landed["composite"]["composed"].get<bool>());
    CHECK(landed["composite"]["pixels_changed"].get<std::size_t>() > 0);
    // A new layer, and the one underneath is exactly what it was: an answer never overwrites a layer.
    REQUIRE(h.state.doc.layers().size() == layers_before + 1);
    const std::uint8_t base_fill[4] = {30, 40, 50, 255};
    CHECK(std::memcmp(h.state.doc.layer(0).pixels.at(14, 12), base_fill, kChannels) == 0);
    // Pixels the document never had, inside the region...
    CHECK_EQ(h.state.doc.layer(1).pixels.at(14, 12)[3], 255);
    // ...and nothing outside it: the feathered coverage is the only thing that bounds the write.
    CHECK_EQ(h.state.doc.layer(1).pixels.at(0, 0)[3], 0);
    CHECK_NE(checksum_of(h), base_document);
    CHECK_EQ(undo_depth(h), marked + 1);  // layer and pixels are one edit

    h.value("edit.undo", json::object());
    CHECK_EQ(h.state.doc.layers().size(), layers_before);
    CHECK_EQ(checksum_of(h), base_document);

    // An argument the door no longer takes is refused, naming what it does take.
    const CommandResult odd =
        h.run("ai.compose", json{{"image", answer_path}, {"extent", "canvas"}});
    CHECK(!odd.ok);
    CHECK(odd.error.find("extent") != std::string::npos);
}
}  // namespace

// The front door is state, not an ImGui flag: whether the app is waiting for a person is a fact the
// wire reports and a probe can put there.
// One key must not throw away work: the ask is held while the document is dirty, the window's answer
// and the wire's are the same command, and only an explicit `force` reaches the host.
void test_unsaved_work_holds_a_quit() {
    Harness h;
    // A picture nobody saved is not a picture to discard quietly: `doc.new` leaves the document dirty,
    // which is exactly the state a stray Escape would otherwise destroy.
    h.value("doc.new", json{{"width", 8}, {"height", 8}});
    CHECK(h.state.doc.dirty());
    const json held = h.value("app.quit", json::object());
    CHECK(!held["quitting"].get<bool>());
    CHECK(held["pending"].get<bool>());
    CHECK(held["dirty"].get<bool>());
    CHECK(held["path"].is_null());
    CHECK_EQ(h.quits, 0);
    CHECK_EQ(h.value("app.state", json::object())["quit_pending"].get<bool>(), true);

    // Asking again withdraws it - the second Escape - and the host is still not reached.
    CHECK(!h.value("app.quit", json::object())["pending"].get<bool>());
    CHECK_EQ(h.quits, 0);

    // `force` is what the window's `Discard` presses, and it is the only way past a dirty document.
    h.value("app.quit", json::object());
    CHECK(h.value("app.quit", json{{"force", true}})["quitting"].get<bool>());
    CHECK_EQ(h.quits, 1);
    CHECK(h.run("app.quit", json{{"force", "yes"}}).error.find("'force' must be true or false") !=
          std::string::npos);

    // Saving ends the question rather than answering it: nothing pending, and the next ask leaves.
    h.value("app.quit", json::object());
    CHECK(h.value("app.state", json::object())["quit_pending"].get<bool>());
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "noise-app-quit-test";
    std::filesystem::create_directories(dir);
    const std::filesystem::path saved = dir / "kept.noise";
    h.value("doc.save", json{{"path", saved.string()}});
    CHECK(!h.state.doc.dirty());
    CHECK(!h.value("app.quit", json::object())["pending"].get<bool>());
    CHECK_EQ(h.quits, 2);
}

// The dialog is a detour; the path it hands back is the action. A pick asked for over the wire has to
// be applied by the same pump a click relies on, or the answer lands in `file.pick.state` and nowhere
// else - which is what happened while the routing lived in the widget that opened the dialog.
void test_a_picked_path_is_applied_whatever_asked_for_the_dialog() {
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "noise-app-pick-test";
    std::filesystem::create_directories(dir);
    const std::filesystem::path target = dir / "picked.noise";
    std::filesystem::remove(target);

    Harness h;
    h.value("doc.new", json{{"width", 8}, {"height", 8}});
    h.value("file.pick", json{{"for", "project"}, {"title", "Save document"}});
    const auto pending = app::take_native_file_pick(h.state);
    REQUIRE(pending.has_value());
    CHECK(pending->kind == platform::PickKind::save);
    platform::PickResult answer;
    answer.paths = {target.string()};
    app::finish_native_file_pick(h.state, std::move(answer));
    app::pump_picked_files(h.state, h.table);
    CHECK_EQ(h.state.doc.path(), target.string());
    CHECK(!h.state.doc.dirty());
    CHECK(std::filesystem::exists(target));
    CHECK(h.state.pick_note.empty());
    // Routed once: the pump does not save the same path again on the next frame.
    const std::string saved = h.value("doc.get", json::object())["checksum"].get<std::string>();
    app::pump_picked_files(h.state, h.table);
    CHECK_EQ(h.value("doc.get", json::object())["checksum"].get<std::string>(), saved);
}

// The project dialog's only format is the project format, so a name without the extension means the
// same file. Refusing it left a person pressing Save a second time to get what the dialog asked for.
void test_a_save_name_without_an_extension_gets_the_project_one() {
    Harness h;
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "noise-app-savename-test";
    std::filesystem::create_directories(dir);
    h.value("doc.new", json{{"width", 8}, {"height", 8}});
    const std::filesystem::path bare = dir / "typed-a-name";
    std::filesystem::remove(bare);
    std::filesystem::remove(bare.string() + ".noise");
    h.value("doc.save", json{{"path", bare.string()}});
    CHECK(std::filesystem::exists(bare.string() + ".noise"));
    CHECK_EQ(h.state.doc.path(), bare.string() + ".noise");

    // A different format is another command, and the refusal says which one.
    const CommandResult wrong = h.run("doc.save", json{{"path", (dir / "wrong.png").string()}});
    CHECK(!wrong.ok);
    CHECK(wrong.error.find("doc.export") != std::string::npos);
}

// The wash is a view choice with three answers and one door, so a probe can ask for the picture with
// the coverage visible and get it. The pixel claim belongs to `coverage_view_probe`; this is the claim
// that a wrong word is refused by listing the right ones.
void test_how_coverage_is_drawn_is_a_choice_the_wire_can_make() {
    Harness h;
    CHECK(std::string(app::coverage_view_name(h.state.coverage_view)) == "both");
    h.value("view.coverage", json{{"mode", "overlay"}});
    CHECK(h.state.coverage_view == app::CoverageView::overlay);
    CHECK_EQ(h.value("view.coverage", json::object())["coverage"], std::string("overlay"));

    const CommandResult bad = h.run("view.coverage", json{{"mode", "xray"}});
    CHECK(!bad.ok);
    CHECK(bad.error.find("ants") != std::string::npos);
    CHECK(bad.error.find("overlay") != std::string::npos);
    CHECK(h.state.coverage_view == app::CoverageView::overlay);
}

// The pointer overlay is a view of state, so the rules that matter are checkable without a window: the
// ring is the brush's diameter at the zoom in force, the OS cursor stands down exactly when the ring is
// on screen to replace it, and a probe can put the ring where no pointer is.
void test_the_pointer_says_which_tool_it_is_and_hows_the_brush() {
    Harness h;
    h.value("doc.new", json{{"width", 64}, {"height", 48}});
    const std::optional<Vec2> over = Vec2{10, 10};

    h.value("tool.select", json::object());
    const app::PointerView marking = app::resolve_pointer(h.state, over, std::nullopt);
    CHECK(marking.shape == PointerShape::crosshair);
    // A marquee has no size to show, and a ring around it would be a lie about a future dab.
    CHECK(!marking.drawn);
    CHECK(app::pointer_request(marking) == app::PointerRequest::crosshair);

    h.value("tool.brush", json::object());
    const double scale = h.state.view.scale;
    h.state.brush.size = 4.0;
    const app::PointerView small = app::resolve_pointer(h.state, over, std::nullopt);
    CHECK(std::abs(small.footprint_px - 4.0 * scale) < 1e-9);
    CHECK(small.drawn);
    CHECK(app::pointer_request(small) == app::PointerRequest::hidden);

    h.state.brush.size = 64.0;
    const app::PointerView large = app::resolve_pointer(h.state, over, std::nullopt);
    CHECK(std::abs(large.footprint_px / small.footprint_px - 16.0) < 1e-9);
    // The hardness core never exceeds the rim: it is the part painted at full strength.
    CHECK(large.core_px <= large.footprint_px);
    h.state.brush.hardness = 1.0;
    CHECK(std::abs(app::resolve_pointer(h.state, over, std::nullopt).core_px -
                   large.footprint_px) < 1e-9);
    h.state.brush.hardness = 0.5;

    // A ring can be forced on where no dab tool is, and off where one is - and off must not leave the
    // person without a marker for where the paint lands.
    h.value("tool.move", json::object());
    CHECK(app::resolve_pointer(h.state, over, true).drawn);
    const app::PointerView unhinted = app::resolve_pointer(h.state, over, std::nullopt);
    CHECK(!unhinted.drawn);
    CHECK(app::pointer_request(unhinted) == app::PointerRequest::move);
    h.value("tool.brush", json::object());
    CHECK(app::pointer_request(app::resolve_pointer(h.state, over, false)) ==
          app::PointerRequest::crosshair);

    // Off the canvas the panels keep whatever cursor ImGui asked for: a text field's I-beam is not the
    // canvas's business.
    CHECK(app::pointer_request(app::resolve_pointer(h.state, std::nullopt, true)) ==
          app::PointerRequest::leave_alone);

    const CommandResult bad = h.run("pointer.footprint", json{{"show", "yes"}});
    CHECK(!bad.ok);
    CHECK(bad.error.find("show") != std::string::npos);
    h.value("pointer.footprint", json{{"show", true}});
    CHECK_EQ(h.value("pointer.footprint", json::object())["forced"], true);
    h.value("pointer.footprint", json{{"show", nullptr}});
    CHECK(h.value("pointer.footprint", json::object())["forced"].is_null());
}

void test_the_front_door_is_a_state_the_wire_can_open_and_shut() {
    Harness harness;
    CHECK_EQ(harness.value("app.state", json::object())["front_door"]["front_door"].get<std::string>(),
             std::string("closed"));
    CHECK_EQ(harness.value("front_door", json{{"show", "welcome"}})["front_door"].get<std::string>(),
             std::string("welcome"));
    // File > New asks the same question as the launch dialog, minus the file half, and the command a
    // menu item fires is the one a driver fires.
    CHECK_EQ(harness.value("file.new", json::object())["front_door"].get<std::string>(),
             std::string("new"));
    const CommandResult nonsense = harness.run("front_door", json{{"show", "sideways"}});
    CHECK(!nonsense.ok);
    CHECK(nonsense.error.find("sideways") != std::string::npos);
    CHECK_EQ(harness.value("front_door", json{{"show", "close"}})["front_door"].get<std::string>(),
             std::string("closed"));

    // A file opened through the door shuts it: the door is the way in, and its job ends when a
    // document is in hand. The welcome screen staying up over a loaded picture is the bug.
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "noise-app-door-test";
    std::filesystem::create_directories(dir);
    const std::filesystem::path project = dir / "shut.noise";
    harness.value("doc.new", json{{"width", 8}, {"height", 8}});
    harness.value("doc.save", json{{"path", project.string()}});
    harness.value("front_door", json{{"show", "welcome"}});
    harness.value("doc.open", json{{"path", project.string()}});
    CHECK_EQ(harness.value("app.state", json::object())["front_door"]["front_door"].get<std::string>(),
             std::string("closed"));
}

void test_the_new_document_form_offers_the_sizes_and_refuses_the_impossible() {
    Harness harness;
    const json form = harness.value("new_document", json::object());
    CHECK_EQ(form["width"].get<int>(), noise::app::kNewDocumentDefault.width);
    CHECK_EQ(form["height"].get<int>(), noise::app::kNewDocumentDefault.height);
    CHECK(form["presets"].size() >= 3);
    CHECK_EQ(harness.value("new_document", json{{"width", 640}, {"height", 480},
                                                {"prompt", "a red cube"}})["width"].get<int>(),
             640);
    CHECK(!harness.run("new_document", json{{"width", 0}}).ok);
    const CommandResult huge = harness.run("new_document", json{{"height", 99999}});
    CHECK(!huge.ok);
    CHECK(huge.error.find(std::to_string(noise::app::kNewDocumentMaxEdge)) != std::string::npos);
    // A refused write leaves the form where it was: a dialog that kept the rejected number would
    // Create a document nobody approved.
    const json after = harness.value("new_document", json::object());
    CHECK_EQ(after["width"].get<int>(), 640);
    CHECK_EQ(after["height"].get<int>(), 480);
    CHECK_EQ(after["prompt"].get<std::string>(), std::string("a red cube"));
}

void test_the_recent_files_are_the_documents_this_app_touched() {
    Harness harness;
    // The config file is the app's own; a test that wrote to a person's real one would be a side
    // effect the suite has no business making.
    const auto config = std::filesystem::temp_directory_path() /
                        ("noise-config-" + std::to_string(platform::pid()));
    std::filesystem::remove_all(config);
    std::filesystem::create_directories(config);
    set_test_env("NOISE_CONFIG_DIR", config.string());

    harness.value("doc.new", json{{"width", 8}, {"height", 6}});
    const std::string first = harness.temp("first.noise");
    const std::string second = harness.temp("second.noise");
    harness.value("doc.save", json{{"path", first}});
    harness.value("doc.save", json{{"path", second}});
    json files = harness.value("file.recent", json::object())["files"];
    REQUIRE(files.size() >= 2);
    CHECK_EQ(files[0].get<std::string>(), std::filesystem::canonical(second).string());

    // One document, one row: saving it again moves it to the front rather than listing it twice.
    harness.value("doc.save", json{{"path", first}});
    files = harness.value("file.recent", json::object())["files"];
    int occurrences = 0;
    for (const json& entry : files) {
        if (entry.get<std::string>() == std::filesystem::canonical(first).string()) {
            ++occurrences;
        }
    }
    CHECK_EQ(occurrences, 1);
    // And the row is a way back in, which is the only reason the list exists.
    harness.value("doc.open", json{{"path", files[0].get<std::string>()}});
    CHECK_EQ(harness.value("doc.get", json::object())["path"].get<std::string>(),
             std::filesystem::canonical(first).string());
    unset_test_env("NOISE_CONFIG_DIR");
}

void test_theme_switch_persists_without_changing_the_document() {
    const auto config = std::filesystem::temp_directory_path() /
                        ("noise-theme-config-" + std::to_string(platform::pid()));
    std::filesystem::remove_all(config);
    std::filesystem::create_directories(config);
    const std::optional<std::string> previous = platform::env("NOISE_CONFIG_DIR");
    set_test_env("NOISE_CONFIG_DIR", config.string());

    Harness harness;
    CHECK_EQ(harness.value("ui.theme", json::object())["theme"].get<std::string>(),
             std::string("dark"));
    CHECK_EQ(io::load_theme(), io::Theme::dark);
    harness.value("doc.new", json{{"width", 12}, {"height", 9},
                                  {"fill", json::array({16, 32, 64, 255})}});
    const json before = harness.value("doc.get", json::object());
    const CommandResult changed = harness.run("ui.theme", json{{"theme", "light"}});
    CHECK_OK(changed);
    CHECK_EQ(changed.value["theme"].get<std::string>(), std::string("light"));
    CHECK_EQ(harness.value("ui.theme", json::object())["theme"].get<std::string>(),
             std::string("light"));
    CHECK_EQ(harness.value("app.state", json::object())["ui_theme"].get<std::string>(),
             std::string("light"));

    const json after = harness.value("doc.get", json::object());
    CHECK_EQ(after["checksum"].get<std::string>(), before["checksum"].get<std::string>());
    CHECK_EQ(after["revision"].get<std::uint64_t>(), before["revision"].get<std::uint64_t>());
    CHECK_EQ(after["history"]["undo_depth"].get<int>(),
             before["history"]["undo_depth"].get<int>());
    CHECK_EQ(io::load_theme(), io::Theme::light);

    io::remember_recent_file(harness.temp("recent.png"));
    CHECK_EQ(io::load_theme(), io::Theme::light);
    const CommandResult invalid = harness.run("ui.theme", json{{"theme", "sepia"}});
    CHECK(!invalid.ok);
    CHECK(invalid.error.find("sepia") != std::string::npos);
    CHECK_EQ(io::load_theme(), io::Theme::light);
    CHECK_EQ(harness.state.ui_theme, io::Theme::light);

    if (previous.has_value()) {
        set_test_env("NOISE_CONFIG_DIR", *previous);
    } else {
        unset_test_env("NOISE_CONFIG_DIR");
    }
    std::filesystem::remove_all(config);
}

// The command wire is the editor surface: every canvas operation and edge tweak must be
// reachable here, and an undo/redo changing geometry must invalidate queued AI landings.
void test_canvas_commands_and_editable_edges_use_the_document_timeline() {
    Harness h;
    h.value("doc.new", json{{"width", 8}, {"height", 6},
                             {"fill", json::array({25, 60, 90, 255})}});
    const std::uint64_t first_epoch = h.state.document_epoch;
    const std::string first_checksum = h.state.doc.checksum();
    CHECK(!h.run("doc.resize", json{{"width", 8193}, {"height", 8193}}).ok);
    CHECK_EQ(h.state.document_epoch, first_epoch);
    CHECK_EQ(h.state.doc.checksum(), first_checksum);

    const json resized = h.value("doc.resize", json{{"width", 12}, {"height", 10},
                                                       {"offset_x", 2}, {"offset_y", 1}});
    CHECK_EQ(resized["marked_added"].get<bool>(), true);
    CHECK(h.state.doc.size() == (Extent{12, 10}));
    REQUIRE(h.state.doc.selection().has_value());
    CHECK_EQ(h.state.doc.selection()->covered(), 72u); // 120 pixels minus 8*6 old pixels
    CHECK_EQ(h.state.doc.selection()->coverage(2, 1), kMaskOut);
    CHECK_EQ(h.state.doc.selection()->coverage(1, 1), kMaskIn);
    CHECK_EQ(h.state.document_epoch, first_epoch + 1);
    CHECK_EQ(h.state.doc.history().undo_depth, 1);

    h.value("mask.from_selection", json::object());
    REQUIRE(h.state.doc.layer(0).mask.has_value());
    CHECK_EQ(h.state.doc.layer(0).mask->coverage(2, 2), kMaskOut);
    h.value("mask.edge", json{{"shift", 1}, {"hard", true}});
    REQUIRE(h.state.doc.layer(0).mask.has_value());
    CHECK_EQ(h.state.doc.layer(0).mask->coverage(2, 2), kMaskIn);
    CHECK_EQ(h.state.doc.layer(0).mask->coverage(5, 4), kMaskOut);
    const std::uint64_t marked_epoch = h.state.document_epoch;
    CHECK(!h.run("mask.edge", json{{"feather", 65}}).ok);
    CHECK_EQ(h.state.document_epoch, marked_epoch);

    h.value("edit.undo", json::object()); // the edge
    CHECK_EQ(h.state.doc.layer(0).mask->coverage(2, 2), kMaskOut);
    h.value("edit.undo", json::object()); // the mask arrival
    CHECK(!h.state.doc.layer(0).mask.has_value());
    h.value("edit.undo", json::object()); // the canvas geometry
    CHECK(h.state.doc.size() == (Extent{8, 6}));
    CHECK_EQ(h.state.doc.checksum(), first_checksum);
    CHECK_EQ(h.state.document_epoch, marked_epoch + 1);
    h.value("edit.redo", json::object());
    CHECK(h.state.doc.size() == (Extent{12, 10}));
    CHECK_EQ(h.state.document_epoch, marked_epoch + 2);

    h.value("doc.crop", json{{"rect", json::array({2, 1, 8, 6})}});
    CHECK(h.state.doc.size() == (Extent{8, 6}));
    CHECK_EQ(h.state.document_epoch, marked_epoch + 3);
    CHECK(!h.run("doc.crop", json{{"rect", json::array({-2, 0, 5, 5})}}).ok);
    h.value("edit.undo", json::object());
    CHECK(h.state.doc.size() == (Extent{12, 10}));
    CHECK_EQ(h.state.document_epoch, marked_epoch + 4);
}

int main() {
    log::set_level(log::Level::Error);
    test_m0_command_surface_is_registered();
    test_canvas_commands_and_editable_edges_use_the_document_timeline();
    test_blend_takes_the_layer_for_its_region();
    test_ai_reference_commands_keep_images_separate_from_document_layers();
    test_ai_references_relink_after_undo_but_detach_across_documents();
    test_reference_inclusion_defaults_and_preview_match_the_operation();
    test_multi_image_edit_uses_an_ordered_snapshot_and_keeps_the_existing_landing();
    test_a_multi_image_backend_failure_leaves_the_document_untouched();
    test_a_recipe_runs_against_the_recorded_server_and_lands();
    test_an_answer_lands_itself_into_the_document_that_asked_for_it();
    test_an_answer_lands_on_the_document_and_undoes_completely();
    test_the_selection_panel_runs_its_intent_through_one_command();
    test_each_ask_takes_its_own_seed_and_a_pin_holds_one();
    test_a_region_is_asked_at_its_own_size_and_says_when_it_is_not();
    test_an_unconfigured_server_names_the_variable();
    test_an_option_left_out_keeps_the_policy_the_table_asked_for();
    test_the_remove_key_runs_the_whole_sequence_with_no_words();
    test_selection_edit_mode_follows_the_job_and_the_result_mask();
    test_the_typed_verbs_carry_their_words_to_their_own_layer();
    test_choosing_not_to_run_costs_no_upload_encode();
    test_an_aspect_choice_prices_the_frame_it_will_ask_for();
    test_a_frame_reaches_the_request_and_leaves_the_canvas_outside_it_alone();
    test_the_new_dialog_creates_the_document_and_the_idea_it_holds();
    test_a_picked_path_does_what_its_file_set_promised();
    test_a_dropped_picture_lands_where_the_pointer_was();
    test_an_answer_on_a_blank_canvas_becomes_the_canvas();
    test_a_style_choice_moves_which_recipe_the_key_runs();
    test_a_verb_that_stops_matching_its_recipe_is_refused_at_startup();
    test_every_recipe_that_asks_for_words_refuses_without_them();
    test_an_answer_lands_on_its_own_layer_over_another_one();
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
    test_the_coverage_target_decides_where_a_stroke_lands();
    test_the_marquee_lands_on_whichever_coverage_is_named();
    test_a_marquee_gesture_is_one_step_in_whichever_coverage();
    test_lasso_command_combines_coverage_and_never_changes_pixels();
    test_lasso_pointer_commits_once_and_undo_redo_restore_exact_coverage();
    test_lasso_edits_a_layer_mask_through_its_provisional_transaction();
    test_lasso_mask_replace_discards_previous_visible_pixels();
    test_a_selection_limits_a_mask_stroke();
    test_the_mask_controls_name_the_layer_they_refused();
    test_a_selection_becomes_a_mask_and_a_mask_becomes_a_selection();
    test_a_layer_mask_is_one_step_and_reaches_the_file();
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
    test_unsaved_work_holds_a_quit();
    test_a_picked_path_is_applied_whatever_asked_for_the_dialog();
    test_a_save_name_without_an_extension_gets_the_project_one();
    test_how_coverage_is_drawn_is_a_choice_the_wire_can_make();
    test_the_pointer_says_which_tool_it_is_and_hows_the_brush();
    test_the_front_door_is_a_state_the_wire_can_open_and_shut();
    test_the_new_document_form_offers_the_sizes_and_refuses_the_impossible();
    test_the_recent_files_are_the_documents_this_app_touched();
    test_theme_switch_persists_without_changing_the_document();
    return noise::test::summary(__FILE__);
}
