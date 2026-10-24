// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <optional>
#include <string>
#include <vector>

namespace noise::platform {

// Asking the desktop for a path. One seam, three possible answers: a path, a cancel, or a failure
// that says what could not be done. Everything in and out is UTF-8.
enum class PickKind {
    open,  // "which file do you want" — may answer with several
    save,  // "where should this go" — the tool warns before it overwrites
};

struct FileFilter {
    std::string label;                     // "Images"
    std::vector<std::string> extensions;   // {"png","webp"} — no dots: the codec table has none.
                                           // {"*"} is the one pattern that means every file.
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

// Runs the desktop's own picker and waits for the answer. It blocks for as long as the person takes,
// so a caller that has to keep drawing — the editor, and the wire that answers while a dialog is open —
// runs it on a worker thread.
//
// Linux talks to `zenity`, and `$NOISE_PICKER_PROGRAM` replaces that program: it is how a test drives a
// stand-in through the same argv, and how a KDE or Flatpak host points at its own equivalent. macOS
// (`NSOpenPanel`) and Windows (`GetOpenFileNameW`) are seams that no one has written yet, and
// `pick_path` says so in `error` rather than reporting a cancel that never happened.
PickResult pick_path(const PickRequest& request);

#ifdef __EMSCRIPTEN__
void begin_browser_pick(const PickRequest& request);
std::optional<PickResult> poll_browser_pick();
void receive_browser_pick(PickResult result);
#endif

// The command line `pick_path` hands the picker, argv[0] first. Split out because what the dialog is
// *asked* is the part that breaks quietly: a filter that never reaches the tool is a dialog full of the
// wrong files, and the return value looks exactly the same.
std::vector<std::string> picker_argv(const std::string& program, const PickRequest& request);

}  // namespace noise::platform
