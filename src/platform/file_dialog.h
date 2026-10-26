// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <optional>
#include <string>
#include <vector>

namespace noise::platform {

// Asking the user for a path. One seam, three possible answers: a path, a cancel, or an error.
enum class PickKind {
    open,  // "which file do you want" — may answer with several
    save,  // "where should this go" — the tool warns before it overwrites
};

struct FileFilter {
    std::string label;                     // "Images"
    std::vector<std::string> extensions;   // {"png","webp"} — no dots: the codec table has none.
                                           // {"*"} is the one pattern that means every file.
};

struct DialogFilter {
    std::string label;
    std::string extensions;
};

struct PickRequest {
    PickKind kind;
    std::string title;
    std::vector<FileFilter> filters;
    // A directory to open in, or the name a save starts from. Empty leaves it to the tool.
    std::string initial_path;
    // Open only: a save takes one path whatever this says.
    bool multiple;
};

struct PickResult {
    // The person said no. That is an ordinary answer rather than a failure, so `error` stays empty and
    // the caller closes its dialog without apologising.
    bool cancelled = false;
    std::vector<std::string> paths;
    std::string error;
};

#ifndef __EMSCRIPTEN__
std::vector<DialogFilter> native_file_filters(const PickRequest& request);
std::string complete_native_save_path(const PickRequest& request, const std::string& path);
PickResult choose_native_file(const PickRequest& request);
void shutdown_native_file_dialog();
#endif

#ifdef __EMSCRIPTEN__
void begin_browser_pick(const PickRequest& request);
std::optional<PickResult> poll_browser_pick();
void receive_browser_pick(PickResult result);
#endif


}  // namespace noise::platform
