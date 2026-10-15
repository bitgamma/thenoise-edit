// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "platform/file_dialog.h"

#include <optional>
#include <string>
#include <vector>

namespace noise::app {

// Which formats a dialog is offering. One table for the whole app: a save dialog that lists something the
// codecs cannot write is a lie, and `io` is what knows.
enum class FileSet {
    open,     // anything this app reads: flat images and `.noise`
    project,  // the document itself
    flat,     // one flattened image — one filter per format, so the extension follows the choice
};

std::optional<FileSet> file_set_from_name(const std::string& name);
std::string file_set_name(FileSet set);
// What the dialog is called and what it asks for: "Save document" / save, "Open" / open.
std::string file_set_label(FileSet set);
platform::PickKind pick_kind(FileSet set);
std::vector<platform::FileFilter> file_filters(FileSet set);

}  // namespace noise::app
