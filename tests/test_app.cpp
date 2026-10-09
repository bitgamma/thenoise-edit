// SPDX-License-Identifier: Apache-2.0
#include "test.h"

#include "app/commands.h"
#include "app/state.h"
#include "core/image.h"
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

void test_a_stroke_that_cannot_run_names_the_field_that_is_wrong() {
    Harness h;
    h.value("doc.new", json{{"width", 16}, {"height", 16}});
    const std::string before = checksum_of(h);
    const json valid = full_stroke();

    for (const char* missing : {"size", "hardness", "opacity", "flow", "spacing", "color"}) {
        json args = valid;
        args.erase(missing);
        const CommandResult result = h.run("tool.stroke", args);
        CHECK(!result.ok);
        CHECK(result.error.find(missing) != std::string::npos);
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
    CHECK(!h.run("edit.redo", json::object()).ok);
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

void test_document_round_trip_through_disk() {
    Harness h;
    const std::string path = h.temp("roundtrip.png");
    // Nothing to save yet, and no path to save it to: both are named in the error.
    CHECK(h.run("doc.save", json{{"path", path}}).error.find("no document") != std::string::npos);
    h.value("doc.new", json{{"width", 37},
                            {"height", 19},
                            {"fill", json::array({10, 200, 30, 255})}});
    CHECK(h.run("doc.save", json::object()).error.find("no path") != std::string::npos);
    const std::string created = h.value("doc.get", json::object())["checksum"].get<std::string>();

    CHECK_OK(h.run("doc.save", json{{"path", path}}));
    h.value("doc.new", json{{"width", 3}, {"height", 3}});
    CHECK(h.value("doc.get", json::object())["checksum"] != created);

    CHECK_OK(h.run("doc.open", json{{"path", path}}));
    const json reopened = h.value("doc.get", json::object());
    CHECK_EQ(reopened["checksum"].get<std::string>(), created);
    CHECK_EQ(reopened["path"].get<std::string>(), path);
    CHECK_EQ(reopened["width"].get<int>(), 37);
    // replace() must bump the revision, or the GPU texture keeps showing the old pixels.
    CHECK(reopened["revision"].get<std::uint64_t>() > 1u);

    const CommandResult missing = h.run("doc.open", json{{"path", h.temp("absent.png")}});
    CHECK(!missing.ok);
    CHECK(missing.error.find("absent.png") != std::string::npos);
    std::filesystem::remove(path);
}

void test_image_get_returns_the_documents_own_pixels() {
    Harness h;
    h.value("doc.new", json{{"width", 40}, {"height", 30}});
    const json got = h.value("image.get", json{{"region", json::array({4, 6, 10, 8})}});
    CHECK_EQ(got["width"].get<int>(), 10);
    CHECK_EQ(got["height"].get<int>(), 8);

    std::string error;
    const Image written = Image::load_png(got["png_path"].get<std::string>(), error);
    CHECK(!written.empty());
    const Image expected = h.state.doc.pixels().region(Rect{4, 6, 10, 8});
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
    CHECK(a.save_png(path_a, error));
    CHECK(b.save_png(path_b, error));

    const json same = h.value("pixels.compare", json{{"a", path_a}, {"b", path_a}});
    CHECK_EQ(same["equal"].get<bool>(), true);
    CHECK_EQ(same["differing_bytes"].get<std::size_t>(), std::size_t(0));

    // The whole-image diff M4's pixel-lock invariant leans on: one touched channel is
    // one differing byte, and the delta is reported.
    const json diff = h.value("pixels.compare", json{{"a", path_a}, {"b", path_b}});
    CHECK_EQ(diff["equal"].get<bool>(), false);
    CHECK_EQ(diff["differing_bytes"].get<std::size_t>(), std::size_t(1));
    CHECK_EQ(diff["max_channel_delta"].get<int>(), 1);

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
    const Image loaded = Image::load_png(shot["path"].get<std::string>(), error);
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

}

int main() {
    log::set_level(log::Level::Error);
    test_m0_command_surface_is_registered();
    test_one_stroke_is_one_step_that_puts_every_byte_back();
    test_a_stroke_that_cannot_run_names_the_field_that_is_wrong();
    test_a_stroke_that_misses_the_document_is_not_a_step();
    test_the_keymap_is_reported_with_the_commands();
    test_app_quit_reaches_the_host();
    test_command_invoke_dispatches_and_names_failures();
    test_bad_arguments_name_the_command();
    test_malformed_fields_name_their_field();
    test_document_round_trip_through_disk();
    test_image_get_returns_the_documents_own_pixels();
    test_pixels_compare_finds_a_single_changed_byte();
    test_a_fit_waits_for_the_canvas_area_to_be_published();
    test_screenshot_returns_exactly_what_the_capture_gave();
    test_a_shorter_image_stays_centred_under_panning();
    test_view_navigation_keeps_the_image_reachable();
    return test::summary("app");
}
