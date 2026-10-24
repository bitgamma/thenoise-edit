// SPDX-License-Identifier: Apache-2.0
#include "platform/file_dialog.h"

#include <emscripten/emscripten.h>
#include <nlohmann/json.hpp>

#include <optional>
#include <utility>

namespace noise::platform {
namespace {

std::optional<PickResult> ready;

EM_JS(void, pick_js, (int kind, const char* title, const char* initial,
                      const char* accept, int multiple), {
    Module['noisePick'](kind === 0 ? 'open' : 'save', UTF8ToString(title),
                        UTF8ToString(initial), UTF8ToString(accept), !!multiple);
});

}  // namespace

// The picker is asynchronous: results are delivered on the next frame, never from a worker thread.
void begin_browser_pick(const PickRequest& request) {
    ready.reset();
    std::string accepts;
    for (const FileFilter& filter : request.filters) {
        for (const std::string& ext : filter.extensions) {
            if (ext == "*") { continue; }
            if (!accepts.empty()) { accepts += ","; }
            accepts += "." + ext;
        }
    }
    pick_js(request.kind == PickKind::open ? 0 : 1, request.title.c_str(),
            request.initial_path.c_str(), accepts.c_str(), request.multiple ? 1 : 0);
}

void receive_browser_pick(PickResult result) { ready = std::move(result); }

std::optional<PickResult> poll_browser_pick() {
    auto answer = std::move(ready);
    ready.reset();
    return answer;
}

}  // namespace noise::platform

extern "C" EMSCRIPTEN_KEEPALIVE
void noise_web_pick_callback(const char* paths, const char* error, int cancelled) {
    noise::platform::PickResult result;
    result.cancelled = cancelled != 0;
    result.error = error;
    if (!result.cancelled && result.error.empty()) {
        const auto parsed = nlohmann::json::parse(paths, nullptr, false);
        if (!parsed.is_array()) {
            result.error = "browser picker returned invalid file list";
        } else {
            for (const auto& path : parsed) {
                if (path.is_string()) { result.paths.push_back(path.get<std::string>()); }
            }
        }
    }
    noise::platform::receive_browser_pick(std::move(result));
}
