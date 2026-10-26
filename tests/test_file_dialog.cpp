// SPDX-License-Identifier: Apache-2.0
#include "test.h"

#include "app/commands.h"
#include "app/file_commands.h"
#include "dbg/wire.h"
#include "io/flat.h"
#include "io/project.h"
#include "platform/file_dialog.h"
#include "platform/platform.h"

#include <filesystem>
#include <string>
#include <utility>
#include <vector>

using namespace noise;
using nlohmann::json;

namespace {

platform::PickRequest request(platform::PickKind kind) {
    platform::PickRequest out{};
    out.kind = kind;
    out.title = "Pick something";
    out.filters = {{"Images", {"png", "webp"}}, {"All files", {"*"}}};
    out.initial_path = "/tmp/here";
    out.multiple = false;
    return out;
}

void test_native_filters() {
    auto open = request(platform::PickKind::open);
    auto open_filters = platform::native_file_filters(open);
    REQUIRE(open_filters.size() == 1);
    CHECK_EQ(open_filters.front().label, std::string("Images"));
    CHECK_EQ(open_filters.front().extensions, std::string("png,webp"));
    auto save = request(platform::PickKind::save);
    auto save_filters = platform::native_file_filters(save);
    REQUIRE(save_filters.size() == 1);
    CHECK_EQ(save_filters.front().extensions, std::string("png,webp"));
    save.filters = {{"noise document", {"noise"}}};
    save_filters = platform::native_file_filters(save);
    REQUIRE(save_filters.size() == 1);
    CHECK_EQ(save_filters.front().extensions, std::string("noise"));
    save.filters = {{"png", {"png"}}, {"jpeg", {"jpg"}}, {"All files", {"*"}}};
    save_filters = platform::native_file_filters(save);
    REQUIRE(save_filters.size() == 2);
    CHECK_EQ(save_filters[0].extensions, std::string("png"));
    CHECK_EQ(save_filters[1].extensions, std::string("jpg"));
    CHECK_EQ(platform::complete_native_save_path(save, "/tmp/image"),
             std::string("/tmp/image.png"));
    CHECK_EQ(platform::complete_native_save_path(save, "/tmp/image.jpg"),
             std::string("/tmp/image.jpg"));
    CHECK_EQ(platform::complete_native_save_path(save, "/tmp/image."),
             std::string("/tmp/image.png"));
    auto project = request(platform::PickKind::save);
    project.filters = {{"noise document", {"noise"}}};
    CHECK_EQ(platform::complete_native_save_path(project, "/tmp/document"),
             std::string("/tmp/document.noise"));
}

void test_filters_match_supported_codecs() {
    const std::vector<std::string> images = io::flat_export_extension_list();
    REQUIRE(!images.empty());
    const auto flat = app::file_filters(app::FileSet::flat);
    CHECK_EQ(flat.size(), images.size() + 1);
    for (std::size_t i = 0; i < images.size(); ++i) {
        CHECK_EQ(flat[i].extensions.front(), images[i]);
    }
    const auto save_filters = platform::native_file_filters(request(platform::PickKind::save));
    CHECK_EQ(save_filters.size(), 1U);
    CHECK(save_filters.front().extensions.find('*') == std::string::npos);
    const auto insertion = app::file_filters(app::FileSet::image);
    CHECK(insertion.front().extensions == images);
    CHECK(app::pick_kind(app::FileSet::image) == platform::PickKind::open);
    CHECK(app::pick_kind(app::FileSet::project) == platform::PickKind::save);
    CHECK(app::pick_kind(app::FileSet::reference) == platform::PickKind::open);
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
            ("noise-file-pick-" + std::to_string(platform::pid()))).string();
        std::filesystem::create_directories(state.runtime_dir);
        state.ai.resource_dir = NOISE_TEST_ASSETS_DIR;
        const app::HostHooks hooks{&blank_frame, [] {}};
        app::register_commands(state, table, wire, hooks);
    }
    CommandResult run(const char* command, const json& args = json::object()) {
        const auto result = table.invoke(command, args);
        CHECK_OK(result);
        return result;
    }
};

void test_picker_posts_request_without_opening_on_command_thread() {
    Harness h;
    auto started = h.run("file.pick", {{"for", "project"}, {"title", "Save document"}});
    REQUIRE(started.ok);
    CHECK(started.value.at("started").get<bool>());
    CHECK(started.value.at("filters").dump().find("noise") != std::string::npos);
    CHECK(h.run("file.pick.state").value.at("running").get<bool>());
    const auto queued = app::take_native_file_pick(h.state);
    REQUIRE(queued.has_value());
    CHECK(queued->title == "Save document");
    CHECK(queued->kind == platform::PickKind::save);
    CHECK(queued->filters.front().extensions.front() == io::kProjectExtension);
    CHECK(!app::take_native_file_pick(h.state).has_value());
    const auto twice = h.table.invoke("file.pick", {{"for", "open"}});
    CHECK(!twice.ok);
    CHECK(twice.error.find("already open") != std::string::npos);
    platform::PickResult cancel;
    cancel.cancelled = true;
    app::finish_native_file_pick(h.state, std::move(cancel));
    CHECK(!h.run("file.pick.state").value.at("running").get<bool>());
    app::pump_picked_files(h.state, h.table);
    CHECK(h.state.pick_note.empty());
}

void test_file_pick_routes_selected_path_once() {
    Harness h;
    const auto dir = std::filesystem::temp_directory_path() /
        ("noise-igfd-picker-route-" + std::to_string(platform::pid()));
    std::filesystem::create_directories(dir);
    const auto target = dir / "saved.noise";
    std::filesystem::remove(target);
    h.run("doc.new", {{"width", 8}, {"height", 8}});
    h.run("file.pick", {{"for", "project"}});
    REQUIRE(app::take_native_file_pick(h.state).has_value());
    platform::PickResult accepted;
    accepted.paths = {target.string()};
    app::finish_native_file_pick(h.state, std::move(accepted));
    const auto state = h.run("file.pick.state");
    CHECK_EQ(state.value.at("paths").dump(), json({target.string()}).dump());
    app::pump_picked_files(h.state, h.table);
    CHECK(std::filesystem::exists(target));
    CHECK_EQ(h.state.doc.path(), target.string());
    CHECK(h.state.pick_note.empty());
    app::pump_picked_files(h.state, h.table); // routed exactly once
    CHECK(h.state.pick_note.empty());
}

void test_multi_reference_request_and_errors() {
    Harness h;
    auto started = h.run("file.pick", {{"for", "reference"}, {"multiple", true}});
    CHECK(started.value.at("started").get<bool>());
    auto pending = app::take_native_file_pick(h.state);
    REQUIRE(pending.has_value());
    CHECK(pending->multiple);
    CHECK(pending->kind == platform::PickKind::open);
    platform::PickResult error;
    error.error = "simulated native dialog error";
    app::finish_native_file_pick(h.state, std::move(error));
    app::pump_picked_files(h.state, h.table);
    CHECK(h.state.pick_note.find("simulated native dialog error") != std::string::npos);
    CHECK(!h.table.invoke("file.pick", {{"for", "flat"}, {"multiple", true}}).ok);
    CHECK(!h.table.invoke("file.pick", {{"for", "nonsense"}}).ok);
}

} // namespace

int main() {
    test_native_filters();
    test_filters_match_supported_codecs();
    test_picker_posts_request_without_opening_on_command_thread();
    test_file_pick_routes_selected_path_once();
    test_multi_reference_request_and_errors();
    return noise::test::summary(__FILE__);
}
