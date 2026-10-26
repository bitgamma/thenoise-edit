// SPDX-License-Identifier: Apache-2.0
#include "platform/platform.h"

#include <ctime>
#include <random>
#include <unistd.h>
#include <cstdlib>
#include <filesystem>
#include <system_error>
#include <thread>

#if defined(__APPLE__)
#include <mach-o/dyld.h>
#include <cstring>
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

}
