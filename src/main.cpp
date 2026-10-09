// SPDX-License-Identifier: Apache-2.0
#include "app/commands.h"
#include "app/shortcuts.h"
#include "app/state.h"
#include "app/ui.h"
#include "core/log.h"
#include "dbg/wire.h"
#include "gfx/checker.h"
#include "gfx/renderer.h"
#include "platform/platform.h"

#include "imgui.h"
#include "sokol_app.h"
#include "sokol_gfx.h"
#include "sokol_glue.h"
#include "sokol_imgui.h"
#include "sokol_log.h"

#include <nlohmann/json.hpp>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>

namespace {

using namespace noise;
using nlohmann::json;

constexpr int kDefaultWidth = 1280;
constexpr int kDefaultHeight = 800;
constexpr const char* kWireAddress = "127.0.0.1";
std::optional<std::string> value_after(const std::string& arg, const std::string& prefix) {
    if (arg.rfind(prefix, 0) != 0) {
        return std::nullopt;
    }
    return arg.substr(prefix.size());
}

struct Args {
    int width = kDefaultWidth;
    int height = kDefaultHeight;
    std::uint16_t wire_port = 0;
    bool wire_enabled = true;
    std::string open_path;
};

[[noreturn]] void usage(int exit_code) {
    std::printf(
        "usage: noise-edit [options] [file.png]\n"
        "  --width=N          window width  (default %d)\n"
        "  --height=N         window height (default %d)\n"
        "  --debug-wire=N     debug wire port, 0 picks a free one (default 0)\n"
        "  --no-debug-wire    do not open the debug wire\n",
        kDefaultWidth, kDefaultHeight);
    std::exit(exit_code);
}

Args parse_args(int argc, char** argv) {
    Args args;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--help") {
            usage(0);
        } else if (arg == "--no-debug-wire") {
            args.wire_enabled = false;
        } else if (const auto port = value_after(arg, "--debug-wire=")) {
            args.wire_port = static_cast<std::uint16_t>(std::atoi(port->c_str()));
        } else if (const auto width = value_after(arg, "--width=")) {
            args.width = std::atoi(width->c_str());
        } else if (const auto height = value_after(arg, "--height=")) {
            args.height = std::atoi(height->c_str());
        } else if (!arg.empty() && arg[0] != '-') {
            args.open_path = arg;
        } else {
            std::fprintf(stderr, "unknown argument: %s\n", arg.c_str());
            usage(2);
        }
    }
    return args;
}

struct Runtime {
    AppState state;
    gfx::Texture canvas_texture;
    gfx::Checker checker;
    CommandTable commands;
    dbg::DebugWire wire{commands};
    Args args;
};

Runtime g;

void publish_endpoint() {
    const std::filesystem::path file = std::filesystem::path(g.state.runtime_dir) / "wire-port";
    std::ofstream out(file, std::ios::trunc);
    if (!out) {
        log::error("cannot publish debug wire port to " + file.string() +
                   "; drivers must read it from the log");
        return;
    }
    out << g.wire.port() << "\n";
    log::info("debug wire endpoint written to " + file.string());
}

dbg::WireConfig config_for(std::uint16_t port) {
    dbg::WireConfig config;
    config.bind_address = kWireAddress;
    config.port = port;
    return config;
}

// ImGui stores the pointer rather than the text, so the string must outlive setup.
std::string& ini_path() {
    static std::string path;
    return path;
}

void init(void) {
    gfx::init();

    const sg_swapchain swapchain = sglue_swapchain();
    g.checker.init();

    simgui_desc_t simgui{};
    simgui.max_vertices = app::kUiVertexLimit;
    simgui.color_format = swapchain.color_format;
    simgui.depth_format = swapchain.depth_format;
    simgui.sample_count = swapchain.sample_count;
    ini_path() = (std::filesystem::path(g.state.runtime_dir) / "imgui.ini").string();
    simgui.ini_filename = ini_path().c_str();
    simgui.logger.func = slog_func;
    simgui_setup(&simgui);
    ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_DockingEnable;

    g.state.framebuffer = Extent{sapp_width(), sapp_height()};
    g.state.viewport = g.state.framebuffer;
    g.state.ui_vertex_limit = app::kUiVertexLimit;

    if (g.args.wire_enabled) {
        std::string error;
        if (!g.wire.start(config_for(g.args.wire_port), error)) {
            log::error("debug wire failed to start: " + error);
        } else {
            g.state.wire_port = g.wire.port();
            publish_endpoint();
        }
    }
    const app::HostHooks hooks{&gfx::read_framebuffer, &sapp_quit};
    app::register_commands(g.state, g.commands, g.wire, hooks);

    // The same path the UI and the wire take, so an image named on the command line
    // gets the view fit, the log line and the error mapping like any other open.
    if (!g.args.open_path.empty()) {
        const CommandResult opened =
            g.commands.invoke("doc.open", json{{"path", g.args.open_path}});
        if (!opened.ok) {
            log::error("startup load failed: " + opened.error);
        }
    }
}

void frame(void) {
    g.state.framebuffer = Extent{sapp_width(), sapp_height()};
    ++g.state.frame;
    g.state.frame_ms = sapp_frame_duration() * 1000.0;


    simgui_frame_desc_t frame_desc{};
    frame_desc.width = sapp_width();
    frame_desc.height = sapp_height();
    frame_desc.delta_time = sapp_frame_duration();
    frame_desc.dpi_scale = sapp_dpi_scale();
    simgui_new_frame(&frame_desc);

    app::draw_ui(g.state, g.commands, g.canvas_texture, g.checker);
    // The UI has now measured the canvas for this frame, so a fit that was waiting for a
    // real viewport can be applied before the commands this frame's wire poll serves.
    g.state.viewport_published = true;
    app::apply_pending_fit(g.state);
    // flush() must run outside the UI pass; draw() inside it.
    simgui_flush();
    g.state.ui_vertices = static_cast<std::uint64_t>(ImGui::GetDrawData()->TotalVtxCount);

    sg_pass_action action{};
    action.colors[0].load_action = SG_LOADACTION_CLEAR;
    action.colors[0].clear_value = {0.10f, 0.10f, 0.11f, 1.0f};
    sg_pass pass{};
    pass.action = action;
    pass.swapchain = sglue_swapchain();
    sg_begin_pass(&pass);
    simgui_draw();

    // Polled inside the pass so capture commands read a fully drawn framebuffer.
    g.wire.poll();

    sg_end_pass();
    sg_commit();
}

// Commands run through the table here too, so a key that reaches a command which cannot
// run - quit with no hook, save with no path - is a log line and not a silence.
void invoke_command(const std::string& id, const json& args, const std::string& why) {
    const CommandResult result = g.commands.invoke(id, args);
    if (!result.ok) {
        log::error(why + " (" + id + "): " + result.error);
    }
}

void event(const sapp_event* ev) {
    // ImGui is offered every event, but its return value mixes keyboard and mouse capture
    // and the pointer is always over UI chrome, so acting on it would drop every key
    // before a shortcut could see it.
    simgui_handle_event(ev);
    if (ev->type == SAPP_EVENTTYPE_QUIT_REQUESTED) {
        invoke_command("app.quit", json::object(), "quit request");
        return;
    }
    // A widget that owns the keyboard (text entry, later) keeps the key: a shortcut fired
    // through it would move the canvas while the user types.
    if (ImGui::GetIO().WantCaptureKeyboard) {
        return;
    }
    const std::optional<std::string> shortcut = app::shortcut_of(*ev);
    if (!shortcut) {
        return;
    }
    const std::string id = g.commands.command_for_shortcut(*shortcut);
    if (id.empty()) {
        return;
    }
    invoke_command(id, json::object(), "shortcut " + *shortcut);
}

void cleanup(void) {
    g.wire.stop();
    g.canvas_texture.discard();
    g.checker.discard();
    simgui_shutdown();
    gfx::shutdown();
}

}

int main(int argc, char** argv) {
    g.args = parse_args(argc, argv);
    g.state.runtime_dir = platform::runtime_dir();

    sapp_desc app{};
    app.init_cb = init;
    app.frame_cb = frame;
    app.event_cb = event;
    app.cleanup_cb = cleanup;
    app.width = g.args.width;
    app.height = g.args.height;
    app.window_title = kWindowTitle;
    app.high_dpi = false;
    app.sample_count = 1;
    app.swap_interval = 1;
    app.enable_clipboard = true;
    app.logger.func = slog_func;
    sapp_run(&app);
    return 0;
}
