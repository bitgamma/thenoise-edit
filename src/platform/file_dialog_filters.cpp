// SPDX-License-Identifier: Apache-2.0
#include "platform/file_dialog.h"

#ifndef __EMSCRIPTEN__

#include <utility>

namespace noise::platform {
namespace {

bool is_wildcard(const FileFilter& filter) {
    return filter.extensions.size() == 1 && filter.extensions.front() == "*";
}

std::string extension_spec(const FileFilter& filter) {
    std::string spec;
    for (const std::string& extension : filter.extensions) {
        if (extension == "*") {
            continue;
        }
        if (!spec.empty()) {
            spec += ',';
        }
        spec += extension;
    }
    return spec;
}

}  // namespace

std::vector<DialogFilter> native_file_filters(const PickRequest& request) {
    std::vector<DialogFilter> filters;
    for (const FileFilter& filter : request.filters) {
        if (request.kind == PickKind::save && is_wildcard(filter)) {
            continue;
        }
        std::string spec = extension_spec(filter);
        if (spec.empty()) {
            continue;
        }
        filters.push_back({filter.label.empty() ? spec : filter.label, std::move(spec)});
    }
    return filters;
}

std::string complete_native_save_path(const PickRequest& request, const std::string& path) {
    if (request.kind != PickKind::save || path.empty()) {
        return path;
    }
    const std::vector<DialogFilter> filters = native_file_filters(request);
    if (filters.empty() || filters.front().extensions.empty()) {
        return path;
    }
    const std::string& extensions = filters.front().extensions;
    const std::size_t separator = extensions.find(',');
    const std::string extension = extensions.substr(0, separator);
    const std::size_t filename_start = path.find_last_of("/\\");
    const std::size_t start = filename_start == std::string::npos ? 0 : filename_start + 1;
    const std::size_t dot = path.find_last_of('.');
    if (dot == std::string::npos || dot < start || dot == start) {
        return path + "." + extension;
    }
    if (dot == path.size() - 1) {
        return path.substr(0, dot) + "." + extension;
    }
    return path;
}

}  // namespace noise::platform

#endif
