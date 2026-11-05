// SPDX-License-Identifier: Apache-2.0
#include "platform/platform.h"

#include <ctime>
#include <random>
#include <unistd.h>
#include <cstdlib>
#include <filesystem>
#include <cstring>
#include <system_error>
#include <thread>

#if defined(__APPLE__)
#include <Carbon/Carbon.h>
#include <mach-o/dyld.h>
#elif defined(__linux__)
#include <X11/XKBlib.h>
#include <X11/keysym.h>
#endif

#if !defined(__linux__) && !defined(__APPLE__)
#error "noise has no platform implementation for this target (see docs/ARCHITECTURE.md)"
#endif

namespace noise::platform {

namespace {

std::string home_dir() {
    if (const auto home = env("HOME"); home && !home->empty()) {
        return *home;
    }
    return ".";
}

std::filesystem::path ensure_dir(const std::filesystem::path& p) {
    std::error_code ec;
    std::filesystem::create_directories(p, ec);
    return p;
}

}

std::string config_dir() {
    if (const auto override_dir = env("NOISE_CONFIG_DIR"); override_dir && !override_dir->empty()) {
        return ensure_dir(*override_dir).string();
    }
    const auto base = env("XDG_CONFIG_HOME");
    const auto root = (base && !base->empty()) ? std::filesystem::path(*base)
                                               : std::filesystem::path(home_dir()) / ".config";
    return ensure_dir(root / "noise-edit").string();
}

std::string runtime_dir() {
    if (const auto override_dir = env("NOISE_RUNTIME_DIR"); override_dir && !override_dir->empty()) {
        return ensure_dir(*override_dir).string();
    }
    const auto base = env("XDG_RUNTIME_DIR");
    const auto root = (base && !base->empty()) ? std::filesystem::path(*base)
                                               : std::filesystem::path(home_dir()) / ".cache";
    return ensure_dir(root / "noise-edit").string();
}

std::string executable_path() {
#if defined(__linux__)
    std::error_code error;
    return std::filesystem::read_symlink("/proc/self/exe", error).string();
#elif defined(__APPLE__)
    std::uint32_t size = 0;
    static_cast<void>(_NSGetExecutablePath(nullptr, &size));
    std::string path(size, '\0');
    if (_NSGetExecutablePath(path.data(), &size) != 0) {
        return {};
    }
    path.resize(std::strlen(path.c_str()));
    return path;
#endif
}

std::optional<std::string> env(const std::string& name) {
    if (const char* raw = std::getenv(name.c_str())) {
        return std::string(raw);
    }
    return std::nullopt;
}

double now_seconds() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<double>(ts.tv_sec) + static_cast<double>(ts.tv_nsec) * 1e-9;
}

std::int64_t now_unix_ms() {
    timespec ts{};
    clock_gettime(CLOCK_REALTIME, &ts);
    return static_cast<std::int64_t>(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000;
}

void sleep_ms(int ms) {
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

std::int64_t pid() {
    return static_cast<std::int64_t>(::getpid());
}

std::uint64_t random_seed() {
    // Two draws: libstdc++ hands out 32 bits at a time, so one call would leave the upper half of the
    // return value constant.
    std::random_device device;
    return (static_cast<std::uint64_t>(device()) << 32) | static_cast<std::uint64_t>(device());
}

std::optional<char> localized_shortcut_letter(char physical_letter) {
    if (physical_letter < 'A' || physical_letter > 'Z') {
        return std::nullopt;
    }
#if defined(__linux__)
    // Sokol uses physical XKB key positions; resolve those against the *active* layout/group.
    struct Position { char letter; const char* xkb_name; };
    constexpr Position positions[] = {
        {'Q', "AD01"}, {'W', "AD02"}, {'E', "AD03"}, {'R', "AD04"}, {'T', "AD05"},
        {'Y', "AD06"}, {'U', "AD07"}, {'I', "AD08"}, {'O', "AD09"}, {'P', "AD10"},
        {'A', "AC01"}, {'S', "AC02"}, {'D', "AC03"}, {'F', "AC04"}, {'G', "AC05"},
        {'H', "AC06"}, {'J', "AC07"}, {'K', "AC08"}, {'L', "AC09"},
        {'Z', "AB01"}, {'X', "AB02"}, {'C', "AB03"}, {'V', "AB04"}, {'B', "AB05"},
        {'N', "AB06"}, {'M', "AB07"},
    };
    const char* position = nullptr;
    for (const Position& item : positions) {
        if (item.letter == physical_letter) {
            position = item.xkb_name;
            break;
        }
    }
    Display* display = XOpenDisplay(nullptr);
    if (display == nullptr) {
        return std::nullopt;
    }
    XkbDescPtr keyboard = XkbGetKeyboard(display, XkbAllComponentsMask, XkbUseCoreKbd);
    std::optional<char> result;
    if (keyboard != nullptr && keyboard->names != nullptr && keyboard->names->keys != nullptr) {
        XkbStateRec state{};
        const unsigned group = XkbGetState(display, XkbUseCoreKbd, &state) == Success
                                   ? state.group : 0u;
        for (int code = keyboard->min_key_code; code <= keyboard->max_key_code; ++code) {
            if (std::memcmp(keyboard->names->keys[code].name, position, XkbKeyNameLength) != 0) {
                continue;
            }
            KeySym symbol = XkbKeycodeToKeysym(display, static_cast<KeyCode>(code),
                                               static_cast<int>(group), 0);
            if (symbol >= XK_a && symbol <= XK_z) {
                result = static_cast<char>('A' + symbol - XK_a);
            } else if (symbol >= XK_A && symbol <= XK_Z) {
                result = static_cast<char>('A' + symbol - XK_A);
            }
            break;
        }
    }
    if (keyboard != nullptr) {
        XkbFreeKeyboard(keyboard, XkbAllComponentsMask, True);
    }
    XCloseDisplay(display);
    return result;
#elif defined(__APPLE__)
    constexpr UInt16 scan_codes[] = {
        kVK_ANSI_A, kVK_ANSI_B, kVK_ANSI_C, kVK_ANSI_D, kVK_ANSI_E,
        kVK_ANSI_F, kVK_ANSI_G, kVK_ANSI_H, kVK_ANSI_I, kVK_ANSI_J,
        kVK_ANSI_K, kVK_ANSI_L, kVK_ANSI_M, kVK_ANSI_N, kVK_ANSI_O,
        kVK_ANSI_P, kVK_ANSI_Q, kVK_ANSI_R, kVK_ANSI_S, kVK_ANSI_T,
        kVK_ANSI_U, kVK_ANSI_V, kVK_ANSI_W, kVK_ANSI_X, kVK_ANSI_Y,
        kVK_ANSI_Z,
    };
    TISInputSourceRef source = TISCopyCurrentKeyboardLayoutInputSource();
    if (source == nullptr) {
        return std::nullopt;
    }
    const auto* data = static_cast<CFDataRef>(TISGetInputSourceProperty(
        source, kTISPropertyUnicodeKeyLayoutData));
    std::optional<char> result;
    if (data != nullptr) {
        const auto* layout = reinterpret_cast<const UCKeyboardLayout*>(CFDataGetBytePtr(data));
        UInt32 dead_key = 0;
        UniChar chars[4]{};
        UniCharCount length = 0;
        if (UCKeyTranslate(layout, scan_codes[physical_letter - 'A'], kUCKeyActionDown, 0,
                           LMGetKbdType(), kUCKeyTranslateNoDeadKeysBit, &dead_key,
                           4, &length, chars) == noErr && length == 1 &&
            chars[0] >= 'a' && chars[0] <= 'z') {
            result = static_cast<char>('A' + chars[0] - 'a');
        } else if (length == 1 && chars[0] >= 'A' && chars[0] <= 'Z') {
            result = static_cast<char>(chars[0]);
        }
    }
    CFRelease(source);
    return result;
#endif
}

}  // namespace noise::platform
