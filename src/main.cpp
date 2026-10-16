// SPDX-License-Identifier: Apache-2.0
#include "app/ai_session.h"
#include "app/commands.h"
#include "app/file_commands.h"
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
    bool start_dialog = true;
    std::string open_path;
};

[[noreturn]] void usage(int exit_code) {
    std::printf(
        "usage: noise-edit [options] [file.png]\n"
        "  --width=N          window width  (default %d)\n"
        "  --height=N         window height (default %d)\n"
        "  --debug-wire=N     debug wire port, 0 picks a free one (default 0)\n"
        "  --no-debug-wire    do not open the debug wire\n"
        "  --no-start-dialog  launch into an empty canvas instead of asking for a file\n",
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
        } else if (arg == "--no-start-dialog") {
            args.start_dialog = false;
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
    app::CoverageTint coverage_tint;
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
    // A person who opened the editor with an idea, and no file, is asked what they want rather than
    // handed an empty canvas and left to find the door. A file on the command line answers the same
    // question, and `--no-start-dialog` is how a driver says it already did.
    if (g.args.open_path.empty() && g.args.start_dialog) {
        g.state.front_door = app::FrontDoor::welcome;
    }
}

// Which file, and whether it has unsaved work: the question a person asks the title bar just before
// they close the window.
inline constexpr int kMaxDroppedFiles = 8;
inline constexpr int kDroppedPathBytes = 4096;

std::string window_title() {
    std::string name = "untitled";
    if (!g.state.doc.path().empty()) {
        const std::filesystem::path file(g.state.doc.path());
        name = file.filename().string();
    }
    if (!g.state.doc.empty() && g.state.doc.dirty()) {
        name += " *";
    }
    return name + " - " + kWindowTitle;
}

void frame(void) {
    g.state.framebuffer = Extent{sapp_width(), sapp_height()};
    static std::string title;
    if (const std::string wanted = window_title(); wanted != title) {
        title = wanted;
        sapp_set_window_title(title.c_str());
    }
    ++g.state.frame;
    g.state.frame_ms = sapp_frame_duration() * 1000.0;


    simgui_frame_desc_t frame_desc{};
    g.state.now = platform::now_seconds();
    frame_desc.width = sapp_width();
    frame_desc.height = sapp_height();
    frame_desc.delta_time = sapp_frame_duration();
    frame_desc.dpi_scale = sapp_dpi_scale();
    simgui_new_frame(&frame_desc);

    // A desktop dialog answers whenever the person finishes with it, on its own thread; this is where
    // its answer becomes an editing command.
    app::pump_picked_files(g.state, g.commands);
    app::draw_ui(g.state, g.commands, g.canvas_texture, g.checker, g.coverage_tint);
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

    // The handshake `$THENOISE_URL` configures, once, after a frame has been presented so the window is
    // on screen before anything can block on a socket. The host owns it rather than the panel, because
    // which dock tab happens to be front is not an answer to "is there a server", and one attempt is the
    // whole cost when the machine is switched off - `server.connect` records that it was asked.
    if (!g.state.ai.connect_attempted) {
        const CommandResult handshake = g.commands.invoke("server.connect", json::object());
        if (!handshake.ok) {
            log::warn("startup handshake: " + handshake.error);
        }
    }
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
    // A file dropped on the window is the desktop handing over a path, and the only honest answer is
    // to put the picture under the pointer. XDND is sokol's job; the placement and the layer are the
    // same command a wire client or a future picker calls.
    if (ev->type == SAPP_EVENTTYPE_FILES_DROPPED) {
        const Vec2 at = g.state.view.view_to_world(
            {static_cast<double>(ev->mouse_x) - g.state.canvas_origin.x,
             static_cast<double>(ev->mouse_y) - g.state.canvas_origin.y});
        for (int i = 0; i < sapp_get_num_dropped_files(); ++i) {
            invoke_command("layer.import",
                           json{{"path", sapp_get_dropped_file_path(i)},
                                {"at", json::array({at.x, at.y})}},
                           "dropped file");
        }
        return;
    }
    if (ev->type == SAPP_EVENTTYPE_QUIT_REQUESTED) {
        const CommandResult asked = g.commands.invoke("app.quit", json::object());
        if (!asked.ok) {
            log::error("quit request (app.quit): " + asked.error);
        }
        // sokol quits unless the app tells it otherwise: `QUIT_REQUESTED` only asks, and if the guard
        // held the answer back, the host has to be cancelled or the window closes with the question
        // still on the screen.
        if (!asked.ok || !asked.value.value("quitting", false)) {
            sapp_cancel_quit();
        }
        return;
    }
    // Escape answers the dialog in front of the person rather than the application: at the front door
    // it means "never mind", and `app.quit` on the same key would throw away the run.
    if (ev->type == SAPP_EVENTTYPE_KEY_DOWN && ev->key_code == SAPP_KEYCODE_ESCAPE) {
        if (g.state.ask_verb) {
            invoke_command("verb.ask", json{{"show", "close"}}, "close the phrase box");
            return;
        }
        if (g.state.quit_pending) {
            // The question on the screen gets the safe answer from the same key that asked it: the
            // modal has the input by then, so this is the only route by which Escape can mean
            // "never mind" rather than nothing.
            invoke_command("app.quit", json::object(), "withdraw the quit");
            return;
        }
        if (g.state.front_door != app::FrontDoor::closed) {
            invoke_command("front_door", json{{"show", "close"}}, "close the front door");
            return;
        }
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
    // The runtime directory is scratch: nothing in it is read back, and the only thing that has to
    // outlive a run is the document the person saved. Job blobs and captures therefore go at startup -
    // a directory that grows by a few megabytes every run, forever, is a bug with a slow fuse.
    for (const char* scratch_subdir : {"ai", "captures"}) {
        std::error_code scratch_ec;
        std::filesystem::remove_all(std::filesystem::path(g.state.runtime_dir) / scratch_subdir,
                                    scratch_ec);
    }
    // The AI tables and the transport, opened once here so no command reaches for a directory or a
    // server of its own. A table that will not load is recorded and reported by the AI commands; the
    // editor itself starts either way.
    app::open_ai_session(g.state);

    sapp_desc app{};
    app.init_cb = init;
    app.frame_cb = frame;
    app.event_cb = event;
    app.cleanup_cb = cleanup;
    app.width = g.args.width;
    app.height = g.args.height;
    app.window_title = kWindowTitle;
    // sokol asks for the drop support up front and caps what it will hand over; the cap is here
    // because a stack of forty imports would be an undo history nobody asked for.
    app.enable_dragndrop = true;
    app.max_dropped_files = kMaxDroppedFiles;
    app.max_dropped_file_path_length = kDroppedPathBytes;
    app.high_dpi = false;
    app.sample_count = 1;
    app.swap_interval = 1;
    app.enable_clipboard = true;
    app.logger.func = slog_func;
    sapp_run(&app);
    return 0;
}
