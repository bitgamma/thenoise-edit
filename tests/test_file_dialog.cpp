// SPDX-License-Identifier: Apache-2.0
#include "test.h"

#include "app/commands.h"
#include "app/file_commands.h"
#include "dbg/wire.h"
#include "io/flat.h"
#include "io/project.h"
#include "platform/file_dialog.h"
#include "platform/platform.h"

#include <sys/stat.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <algorithm>
#include <string>
#include <thread>
#include <vector>

using namespace noise;
using nlohmann::json;

namespace {

// `$NOISE_PICKER_PROGRAM` replaces zenity inside the platform seam, so a test drives a shell script
// through the same argv, the same pipe and the same exit codes as the real dialog. What is *not*
// tested is zenity itself: no hermetic run may depend on a desktop.
std::filesystem::path script(const std::string& name, const std::string& body) {
    static int counter = 0;
    const auto dir = std::filesystem::temp_directory_path() /
                     ("noise-pick-test-" + std::to_string(platform::pid()));
    std::filesystem::create_directories(dir);
    const auto path = dir / (name + "-" + std::to_string(counter++) + ".sh");
    std::ofstream out(path);
    out << "#!/bin/sh\n" << body;
    out.close();
    ::chmod(path.c_str(), 0755);
    return path;
}

bool carries(const std::vector<std::string>& argv, const std::string& word) {
    return std::find(argv.begin(), argv.end(), word) != argv.end();
}

platform::PickRequest request(platform::PickKind kind) {
    platform::PickRequest out{};
    out.kind = kind;
    out.title = "Pick something";
    out.filters = {{"Images", {"png", "webp"}}};
    out.initial_path = "/tmp/here";
    out.multiple = false;
    return out;
}

void test_the_picker_is_asked_for_the_title_the_kind_and_the_filters() {
    const std::vector<std::string> open =
        platform::picker_argv("picker-under-test", request(platform::PickKind::open));
    CHECK_EQ(open.front(), std::string("picker-under-test"));
    CHECK(carries(open, "--file-selection"));
    CHECK(carries(open, "--title=Pick something"));
    CHECK(carries(open, "--filename=/tmp/here"));
    CHECK(carries(open, "--file-filter=Images | *.png *.webp"));
    CHECK(!carries(open, "--save"));
    CHECK(!carries(open, "--multiple"));

    platform::PickRequest several = request(platform::PickKind::open);
    several.multiple = true;
    CHECK(carries(platform::picker_argv("picker-under-test", several), "--multiple"));

    // A save has to warn before it overwrites: without this the app can eat a file with one Enter.
    const std::vector<std::string> save =
        platform::picker_argv("picker-under-test", request(platform::PickKind::save));
    CHECK(carries(save, "--save"));
    CHECK(carries(save, "--confirm-overwrite"));
    CHECK(!carries(save, "--multiple"));

    // A separator of newline, because `|` is zenity's default and a path may contain it.
    CHECK(carries(open, std::string("--separator=\n")));
}

void test_the_filters_are_the_formats_the_codecs_write() {
    // A dialog that offers a format the codecs cannot write is a promise the export then breaks, so the
    // filters are read from the codec table rather than typed out again here.
    const std::vector<std::string> images = io::flat_export_extension_list();
    REQUIRE(!images.empty());
    const std::vector<platform::FileFilter> flat = app::file_filters(app::FileSet::flat);
    CHECK_EQ(static_cast<int>(flat.size()), static_cast<int>(images.size()) + 1);
    for (std::size_t i = 0; i < images.size(); ++i) {
        CHECK_EQ(flat[i].extensions.size(), static_cast<std::size_t>(1));
        if (!flat[i].extensions.empty()) {
            CHECK_EQ(flat[i].extensions.front(), images[i]);
        }
    }
    const std::vector<platform::FileFilter> project = app::file_filters(app::FileSet::project);
    REQUIRE(project.size() == 1);
    CHECK_EQ(project[0].extensions.front(), std::string(io::kProjectExtension));
    // Inserting a picture asks for the reader's formats and nothing else: a `.noise` offered here would
    // be answered by an import that can only fail.
    const std::vector<platform::FileFilter> insert = app::file_filters(app::FileSet::image);
    REQUIRE(!insert.empty());
    CHECK_EQ(insert[0].extensions, images);
    for (const platform::FileFilter& filter : insert) {
        CHECK(std::find(filter.extensions.begin(), filter.extensions.end(),
                        std::string(io::kProjectExtension)) == filter.extensions.end());
    }
    CHECK(app::pick_kind(app::FileSet::image) == platform::PickKind::open);

    const std::vector<platform::FileFilter> references = app::file_filters(app::FileSet::reference);
    REQUIRE(!references.empty());
    CHECK_EQ(references.front().extensions, images);
    CHECK(app::pick_kind(app::FileSet::reference) == platform::PickKind::open);
    CHECK(app::file_set_from_name("reference") == app::FileSet::reference);
    CHECK(app::file_set_name(app::FileSet::reference) == std::string("reference"));
    CHECK(app::file_set_label(app::FileSet::reference).find("AI reference") != std::string::npos);

    CHECK(app::file_set_from_name("nonsense").has_value() == false);
    CHECK(app::file_set_from_name("flat").has_value());
    CHECK(app::pick_kind(app::FileSet::open) == platform::PickKind::open);
    CHECK(app::pick_kind(app::FileSet::flat) == platform::PickKind::save);
}

void test_a_pick_answers_with_the_path_the_picker_named() {
    const auto answers = script("answers", "printf '%s\\n' \"$@\" > \"$NOISE_PICK_ARGV\"\n"
                                           "printf '%s\\n' \"$NOISE_PICK_ANSWER\"\n");
    const auto recorded = answers.string() + ".argv";
    ::setenv("NOISE_PICK_ANSWER", "/tmp/a photo.png", 1);
    ::setenv("NOISE_PICK_ARGV", recorded.c_str(), 1);
    ::setenv("NOISE_PICKER_PROGRAM", answers.c_str(), 1);

    const platform::PickResult picked = platform::pick_path(request(platform::PickKind::open));
    CHECK_EQ(picked.error, std::string(""));
    CHECK(!picked.cancelled);
    REQUIRE(picked.paths.size() == 1);
    CHECK_EQ(picked.paths.front(), std::string("/tmp/a photo.png"));

    // The flags reached the process, not just the argv the builder returned: a filter that is dropped
    // between the two is invisible in every other observation the app makes.
    std::ifstream log(recorded);
    std::string line;
    std::vector<std::string> seen;
    while (std::getline(log, line)) {
        seen.push_back(line);
    }
    CHECK_EQ(seen.size(), platform::picker_argv(answers.string(), request(platform::PickKind::open)).size());
    CHECK(carries(seen, "--file-filter=Images | *.png *.webp"));
    CHECK(carries(seen, "--title=Pick something"));

    ::unsetenv("NOISE_PICKER_PROGRAM");
    ::unsetenv("NOISE_PICK_ANSWER");
    ::unsetenv("NOISE_PICK_ARGV");
}

void test_a_cancelled_pick_is_a_cancel_and_not_an_error() {
    // zenity's 1 is "the person pressed Cancel": ordinary, and not something to apologise for.
    const auto no = script("cancel", "exit 1\n");
    ::setenv("NOISE_PICKER_PROGRAM", no.c_str(), 1);
    const platform::PickResult picked = platform::pick_path(request(platform::PickKind::save));
    CHECK(picked.cancelled);
    CHECK_EQ(picked.error, std::string(""));
    CHECK(picked.paths.empty());
    ::unsetenv("NOISE_PICKER_PROGRAM");
}

void test_a_picker_that_is_not_installed_is_named_in_the_error() {
    ::setenv("NOISE_PICKER_PROGRAM", "/nonexistent/noise-picker-under-test", 1);
    const platform::PickResult picked = platform::pick_path(request(platform::PickKind::open));
    CHECK(!picked.cancelled);
    CHECK(picked.error.find("noise-picker-under-test") != std::string::npos);
    ::unsetenv("NOISE_PICKER_PROGRAM");
}

Image blank_frame(int width, int height) {
    return Image::filled(Extent{width, height}, 0, 0, 0, 255);
}

struct Harness {
    AppState state;
    CommandTable table;
    dbg::DebugWire wire{table};

    Harness() {
        state.runtime_dir = (std::filesystem::temp_directory_path() /
                             ("noise-pick-cmd-" + std::to_string(platform::pid())))
                                .string();
        std::filesystem::remove_all(state.runtime_dir);
        std::filesystem::create_directories(state.runtime_dir);
        state.ai.resource_dir = NOISE_TEST_ASSETS_DIR;
        const app::HostHooks hooks{&blank_frame, [] {}};
        app::register_commands(state, table, wire, hooks);
    }
};

json wait_for_answer(Harness& harness) {
    for (int i = 0; i < 200; ++i) {
        const CommandResult seen = harness.table.invoke("file.pick.state", json::object());
        CHECK_OK(seen);
        if (seen.ok && !seen.value.at("running").get<bool>()) {
            return seen.value;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    CHECK(!"file.pick never answered");
    return json::object();
}

void test_a_pick_over_the_wire_offers_its_filters_and_returns_its_answer() {
    Harness harness;
    const auto answers = script("wire-answers", "printf '%s\\n' \"$NOISE_PICK_ANSWER\"\n");
    ::setenv("NOISE_PICK_ANSWER", "/tmp/final.noise", 1);
    ::setenv("NOISE_PICKER_PROGRAM", answers.c_str(), 1);

    const CommandResult started = harness.table.invoke("file.pick", {{"for", "project"}});
    CHECK_OK(started);
    REQUIRE(started.ok);
    CHECK(started.value.at("started").get<bool>());
    // What the dialog will offer, so a driver can assert the document filter without a mouse.
    const std::string filters = started.value.at("filters").dump();
    CHECK(filters.find(io::kProjectExtension) != std::string::npos);

    const json answer = wait_for_answer(harness);
    CHECK_EQ(answer.at("paths").dump(), json({"/tmp/final.noise"}).dump());
    CHECK_EQ(answer.at("cancelled").get<bool>(), false);

    // A second dialog while one is open is refused, not queued: nobody would see the second one.
    const auto slow = script("slow", "sleep 2\nprintf '/tmp/late.png\\n'\n");
    ::setenv("NOISE_PICKER_PROGRAM", slow.c_str(), 1);
    CHECK_OK(harness.table.invoke("file.pick", {{"for", "open"}}));
    const CommandResult twice = harness.table.invoke("file.pick", {{"for", "open"}});
    CHECK(!twice.ok);
    CHECK(twice.error.find("already open") != std::string::npos);
    wait_for_answer(harness);

    // `multiple` on a save is a contradiction the command says so about, rather than quietly
    // saving one of the paths.
    ::setenv("NOISE_PICKER_PROGRAM", answers.c_str(), 1);
    const CommandResult bad = harness.table.invoke("file.pick", {{"for", "flat"},
                                                                {"multiple", true}});
    CHECK(!bad.ok);
    CHECK(bad.error.find("multiple") != std::string::npos);
    CHECK(!harness.table.invoke("file.pick", {{"for", "nothing"}}).ok);

    ::unsetenv("NOISE_PICKER_PROGRAM");
    ::unsetenv("NOISE_PICK_ANSWER");
}

}  // namespace

int main() {
    test_the_picker_is_asked_for_the_title_the_kind_and_the_filters();
    test_the_filters_are_the_formats_the_codecs_write();
    test_a_pick_answers_with_the_path_the_picker_named();
    test_a_cancelled_pick_is_a_cancel_and_not_an_error();
    test_a_picker_that_is_not_installed_is_named_in_the_error();
    test_a_pick_over_the_wire_offers_its_filters_and_returns_its_answer();
    return noise::test::summary(__FILE__);
}
