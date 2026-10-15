// SPDX-License-Identifier: Apache-2.0
#include "platform/file_dialog.h"

namespace noise::platform {

std::vector<std::string> picker_argv(const std::string& program, const PickRequest& request) {
    // One path per line out of the tool: `|` is zenity's default and a path may contain it.
    std::vector<std::string> argv{program, "--file-selection", "--separator=\n"};
    if (!request.title.empty()) {
        argv.push_back("--title=" + request.title);
    }
    if (request.kind == PickKind::open) {
        if (request.multiple) {
            argv.push_back("--multiple");
        }
    } else {
        argv.push_back("--save");
        argv.push_back("--confirm-overwrite");
    }
    if (!request.initial_path.empty()) {
        argv.push_back("--filename=" + request.initial_path);
    }
    for (const FileFilter& filter : request.filters) {
        if (filter.extensions.empty()) {
            continue;
        }
        std::string entry = filter.label + " |";
        for (const std::string& extension : filter.extensions) {
            entry += extension == "*" ? " *" : " *." + extension;
        }
        argv.push_back("--file-filter=" + entry);
    }
    return argv;
}

}  // namespace noise::platform
