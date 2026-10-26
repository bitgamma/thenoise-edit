// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "app/new_document.h"
#include "app/state.h"
#include "platform/file_dialog.h"

#include <nlohmann/json.hpp>

#include <optional>
#include <string>
#include <vector>

namespace noise::app {

using nlohmann::json;

// Which formats a dialog is offering. One table for the whole app: a save dialog that lists something the
// codecs cannot write is a lie, and `io` is what knows.
enum class FileSet {
    open,     // anything this app reads: flat images and `.noise`
    project,  // the document itself
    flat,     // one flattened image — one filter per format, so the extension follows the choice
    image,    // one picture to put *into* the document, which is `open`'s formats minus `.noise`
    reference,  // independent flat images supplied to AI, never inserted into the document
};

std::optional<FileSet> file_set_from_name(const std::string& name);
std::string file_set_name(FileSet set);
// What the dialog is called and what it asks for: "Save document" / save, "Open" / open.
std::string file_set_label(FileSet set);
// The set names, built from the table: an error that lists formats is a promise about them.
std::string file_set_names();
platform::PickKind pick_kind(FileSet set);
std::vector<platform::FileFilter> file_filters(FileSet set);

// The door and the form, as one JSON shape, so the status line, `app.state` and a probe all read the
// same words for it.
json front_door_json(const AppState& state);

// Apply a finished file pick to the file set it was asked for, once. The host calls it each frame so
// that the answer is routed whoever asked for the dialog.
void pump_picked_files(AppState& state, CommandTable& table);

#ifndef __EMSCRIPTEN__
// The host consumes a pending native request. Native dialogs must not be opened in the command
// callback, which can run on the debug wire.
std::optional<platform::PickRequest> take_native_file_pick(AppState& state);
void finish_native_file_pick(AppState& state, platform::PickResult result);
#endif

}  // namespace noise::app
