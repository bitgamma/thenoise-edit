// SPDX-License-Identifier: Apache-2.0
#include "platform/file_dialog.h"

#include "nfd.h"
#include "platform/platform.h"
#include "sokol_app.h"

#include <algorithm>
#include <filesystem>
#include <limits>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace noise::platform {
namespace {

bool initialized = false;

std::filesystem::path path_from_utf8(const std::string& value) {
    std::u8string encoded;
    encoded.reserve(value.size());
    for (const char character : value) {
        encoded.push_back(static_cast<char8_t>(static_cast<unsigned char>(character)));
    }
    return std::filesystem::path(encoded);
}

std::string utf8_path(const std::filesystem::path& path) {
    const std::u8string encoded = path.u8string();
    return {reinterpret_cast<const char*>(encoded.data()), encoded.size()};
}

struct InitialLocation {
    std::string directory;
    std::string name;
};

std::string& last_directory() {
    static std::string directory;
    return directory;
}

InitialLocation initial_location(const PickRequest& request) {
    if (request.initial_path.empty()) {
        if (!last_directory().empty()) {
            return {last_directory(), {}};
        }
        if (const auto home = env("HOME"); home && !home->empty()) {
            return {*home, {}};
        }
        return {".", {}};
    }

    const std::filesystem::path requested = path_from_utf8(request.initial_path);
    std::error_code error;
    if (std::filesystem::is_directory(requested, error)) {
        return {utf8_path(requested), {}};
    }
    return {requested.has_parent_path() ? utf8_path(requested.parent_path()) : std::string{},
            utf8_path(requested.filename())};
}

nfdwindowhandle_t parent_window() {
    nfdwindowhandle_t parent{};
#if defined(_WIN32)
    parent.type = NFD_WINDOW_HANDLE_TYPE_WINDOWS;
    parent.handle = const_cast<void*>(sapp_win32_get_hwnd());
#elif defined(__APPLE__)
    parent.type = NFD_WINDOW_HANDLE_TYPE_COCOA;
    parent.handle = const_cast<void*>(sapp_macos_get_window());
#elif defined(__linux__)
    parent.type = NFD_WINDOW_HANDLE_TYPE_X11;
    parent.handle = const_cast<void*>(sapp_x11_get_window());
#endif
    return parent;
}

std::vector<nfdu8filteritem_t> api_filters(const std::vector<DialogFilter>& filters) {
    std::vector<nfdu8filteritem_t> out;
    out.reserve(filters.size());
    for (const DialogFilter& filter : filters) {
        out.push_back({filter.label.c_str(), filter.extensions.c_str()});
    }
    return out;
}

std::string nfd_error() {
    const char* error = NFD_GetError();
    return error != nullptr && error[0] != '\0' ? error : "native file dialog failed";
}

bool ensure_initialized(PickResult& result) {
    if (initialized) {
        return true;
    }
    if (NFD_Init() != NFD_OKAY) {
        result.error = "NFD_Init: " + nfd_error();
        return false;
    }
    initialized = true;
    return true;
}

void remember_directory(const std::string& path) {
    const std::filesystem::path selected = path_from_utf8(path);
    if (selected.has_parent_path()) {
        last_directory() = utf8_path(selected.parent_path());
    }
}

void read_path_set(const nfdpathset_t* paths, PickResult& result) {
    nfdpathsetsize_t count = 0;
    if (NFD_PathSet_GetCount(paths, &count) != NFD_OKAY) {
        result.error = "NFD_PathSet_GetCount: " + nfd_error();
        return;
    }
    for (nfdpathsetsize_t i = 0; i < count; ++i) {
        nfdu8char_t* path = nullptr;
        if (NFD_PathSet_GetPathU8(paths, i, &path) != NFD_OKAY || path == nullptr) {
            result.paths.clear();
            result.error = "NFD_PathSet_GetPathU8: " + nfd_error();
            return;
        }
        result.paths.emplace_back(path);
        NFD_PathSet_FreePathU8(path);
    }
    if (result.paths.empty()) {
        result.error = "native file dialog returned no paths";
        return;
    }
    std::sort(result.paths.begin(), result.paths.end());
    remember_directory(result.paths.front());
}

nfdresult_t save_file(const PickRequest& request, const nfdu8filteritem_t* filter_data,
                      nfdfiltersize_t filter_count, const InitialLocation& location,
                      const nfdwindowhandle_t& parent, PickResult& result) {
    nfdsavedialogu8args_t args{};
    args.filterList = filter_data;
    args.filterCount = filter_count;
    args.defaultPath = location.directory.empty() ? nullptr : location.directory.c_str();
    args.defaultName = location.name.empty() ? nullptr : location.name.c_str();
    args.parentWindow = parent;
    args.title = request.title.c_str();
    nfdu8char_t* path = nullptr;
    const nfdresult_t status = NFD_SaveDialogU8_With(&path, &args);
    if (status == NFD_OKAY && path != nullptr) {
        result.paths.push_back(complete_native_save_path(request, path));
        remember_directory(result.paths.front());
        NFD_FreePathU8(path);
    }
    return status;
}

nfdresult_t open_files(const PickRequest& request, const nfdu8filteritem_t* filter_data,
                       nfdfiltersize_t filter_count, const InitialLocation& location,
                       const nfdwindowhandle_t& parent, PickResult& result) {
    nfdopendialogu8args_t args{};
    args.filterList = filter_data;
    args.filterCount = filter_count;
    args.defaultPath = location.directory.empty() ? nullptr : location.directory.c_str();
    args.parentWindow = parent;
    args.title = request.title.c_str();
    const nfdpathset_t* paths = nullptr;
    const nfdresult_t status = NFD_OpenDialogMultipleU8_With(&paths, &args);
    if (status == NFD_OKAY && paths != nullptr) {
        read_path_set(paths, result);
        NFD_PathSet_Free(paths);
    }
    return status;
}

nfdresult_t open_file(const PickRequest& request, const nfdu8filteritem_t* filter_data,
                      nfdfiltersize_t filter_count, const InitialLocation& location,
                      const nfdwindowhandle_t& parent, PickResult& result) {
    nfdopendialogu8args_t args{};
    args.filterList = filter_data;
    args.filterCount = filter_count;
    args.defaultPath = location.directory.empty() ? nullptr : location.directory.c_str();
    args.parentWindow = parent;
    args.title = request.title.c_str();
    nfdu8char_t* path = nullptr;
    const nfdresult_t status = NFD_OpenDialogU8_With(&path, &args);
    if (status == NFD_OKAY && path != nullptr) {
        result.paths.emplace_back(path);
        remember_directory(result.paths.front());
        NFD_FreePathU8(path);
    }
    return status;
}

}  // namespace

PickResult choose_native_file(const PickRequest& request) {
    PickResult result;
    if (!ensure_initialized(result)) {
        return result;
    }
    const std::vector<DialogFilter> filters = native_file_filters(request);
    if (filters.size() > std::numeric_limits<nfdfiltersize_t>::max()) {
        result.error = "native file dialog has too many filters";
        return result;
    }
    const std::vector<nfdu8filteritem_t> native_filters = api_filters(filters);
    const nfdu8filteritem_t* filter_data = native_filters.empty() ? nullptr : native_filters.data();
    const auto filter_count = static_cast<nfdfiltersize_t>(native_filters.size());
    const InitialLocation location = initial_location(request);
    const nfdwindowhandle_t parent = parent_window();
    nfdresult_t status;
    if (request.kind == PickKind::save) {
        status = save_file(request, filter_data, filter_count, location, parent, result);
    } else if (request.multiple) {
        status = open_files(request, filter_data, filter_count, location, parent, result);
    } else {
        status = open_file(request, filter_data, filter_count, location, parent, result);
    }

    if (status == NFD_CANCEL) {
        result.cancelled = true;
    } else if (status == NFD_ERROR) {
        result.error = nfd_error();
    } else if (result.paths.empty() && result.error.empty()) {
        result.error = "native file dialog returned no paths";
    }
    return result;
}

void shutdown_native_file_dialog() {
    if (initialized) {
        NFD_Quit();
        initialized = false;
    }
}

}  // namespace noise::platform
