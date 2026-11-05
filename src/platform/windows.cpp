// SPDX-License-Identifier: Apache-2.0
#include "platform/platform.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <random>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

namespace noise::platform {
namespace {

std::string utf8(const std::wstring& wide) {
    if (wide.empty()) {
        return {};
    }
    const int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(),
                                        static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
    if (size <= 0) {
        return {};
    }
    std::string result(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(),
                        static_cast<int>(wide.size()), result.data(), size, nullptr, nullptr);
    return result;
}

std::wstring wide(const std::string& text) {
    if (text.empty()) {
        return {};
    }
    const int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                                         static_cast<int>(text.size()), nullptr, 0);
    if (size <= 0) {
        return {};
    }
    std::wstring result(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                         static_cast<int>(text.size()), result.data(), size);
    return result;
}

std::filesystem::path from_utf8(const std::string& path) {
    return std::filesystem::path(wide(path));
}

std::string path_utf8(const std::filesystem::path& path) {
    return utf8(path.native());
}

std::string ensure_dir(const std::filesystem::path& path) {
    std::error_code error;
    std::filesystem::create_directories(path, error);
    return path_utf8(path);
}

std::string app_dir(const char* variable, const char* fallback) {
    if (const auto base = env(variable); base && !base->empty()) {
        return *base;
    }
    if (const auto base = env(fallback); base && !base->empty()) {
        return *base;
    }
    return ".";
}

}  // namespace

std::string config_dir() {
    if (const auto override_dir = env("NOISE_CONFIG_DIR"); override_dir && !override_dir->empty()) {
        return ensure_dir(from_utf8(*override_dir));
    }
    return ensure_dir(from_utf8(app_dir("APPDATA", "USERPROFILE")) / "noise-edit");
}

std::string runtime_dir() {
    if (const auto override_dir = env("NOISE_RUNTIME_DIR"); override_dir && !override_dir->empty()) {
        return ensure_dir(from_utf8(*override_dir));
    }
    return ensure_dir(from_utf8(app_dir("LOCALAPPDATA", "TEMP")) / "noise-edit");
}

std::string executable_path() {
    std::vector<wchar_t> buffer(MAX_PATH);
    for (;;) {
        const DWORD count = GetModuleFileNameW(nullptr, buffer.data(),
                                               static_cast<DWORD>(buffer.size()));
        if (count == 0) {
            return {};
        }
        if (static_cast<std::size_t>(count) < buffer.size()) {
            return utf8(std::wstring(buffer.data(), count));
        }
        if (buffer.size() >= 32768) {
            return {};
        }
        buffer.resize(buffer.size() * 2);
    }
}

std::optional<std::string> env(const std::string& name) {
    const std::wstring key = wide(name);
    if (key.empty()) {
        return std::nullopt;
    }
    SetLastError(ERROR_SUCCESS);
    const DWORD wanted = GetEnvironmentVariableW(key.c_str(), nullptr, 0);
    if (wanted == 0) {
        return std::nullopt;
    }
    std::wstring value(wanted, L'\0');
    const DWORD copied = GetEnvironmentVariableW(key.c_str(), value.data(), wanted);
    if (copied == 0 || copied >= wanted) {
        return std::nullopt;
    }
    value.resize(copied);
    return utf8(value);
}

double now_seconds() {
    const auto elapsed = std::chrono::steady_clock::now().time_since_epoch();
    return std::chrono::duration<double>(elapsed).count();
}

std::int64_t now_unix_ms() {
    const auto elapsed = std::chrono::system_clock::now().time_since_epoch();
    return std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();
}

std::uint64_t random_seed() {
    std::random_device device;
    return (static_cast<std::uint64_t>(device()) << 32) | static_cast<std::uint64_t>(device());
}

void sleep_ms(int ms) {
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

std::int64_t pid() {
    return static_cast<std::int64_t>(GetCurrentProcessId());
}



std::optional<char> localized_shortcut_letter(char physical_letter) {
    if (physical_letter < 'A' || physical_letter > 'Z') {
        return std::nullopt;
    }
    // Set-1 scan codes for Sokol's physical A..Z key locations.
    constexpr UINT scans[] = {
        0x1E, 0x30, 0x2E, 0x20, 0x12, 0x21, 0x22, 0x23, 0x17,
        0x24, 0x25, 0x26, 0x32, 0x31, 0x18, 0x19, 0x10, 0x13,
        0x1F, 0x14, 0x16, 0x2F, 0x11, 0x2D, 0x15, 0x2C,
    };
    const UINT vk = MapVirtualKeyExW(scans[physical_letter - 'A'], MAPVK_VSC_TO_VK_EX,
                                     GetKeyboardLayout(0));
    if (vk >= 'A' && vk <= 'Z') {
        return static_cast<char>(vk);
    }
    return std::nullopt;
}

}  // namespace noise::platform
