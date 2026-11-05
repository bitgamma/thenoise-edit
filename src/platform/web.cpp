// SPDX-License-Identifier: Apache-2.0
#include "platform/platform.h"
#include "platform/web.h"

#include <emscripten/emscripten.h>
#include <emscripten/html5.h>

#include <cstdlib>
#include <filesystem>

namespace noise::platform {

namespace {

EM_JS(char*, read_config_js, (), {
    let value = '{}';
    try { value = localStorage.getItem('thenoise.config') || '{}'; } catch (_) {}
    const ptr = _malloc(lengthBytesUTF8(value) + 1);
    stringToUTF8(value, ptr, lengthBytesUTF8(value) + 1);
    return ptr;
});

EM_JS(int, save_config_js, (const char* text), {
    try {
        localStorage.setItem('thenoise.config', UTF8ToString(text));
        return 1;
    } catch (_) { return 0; }
});

EM_JS(int, download_js, (const char* path), {
    return Module['noiseDownload'](UTF8ToString(path)) ? 1 : 0;
});

EM_JS(void, enable_drop_js, (), {
    Module['noiseEnableDrop']();
});

EM_JS(void, set_theme_js, (int light), {
    document.documentElement.dataset.theme = light ? 'light' : 'dark';
});

EM_JS(void, fill_random_js, (unsigned char* bytes), {
    crypto.getRandomValues(HEAPU8.subarray(bytes, bytes + 8));
});

EM_JS(int, layout_letter_js, (int physical), {
    const last = Module['noiseShortcutKey'];
    if (!last || last.code !== 'Key' + String.fromCharCode(physical)) return 0;
    const key = last.key;
    return /^[a-z]$/i.test(key) ? key.toUpperCase().charCodeAt(0) : 0;
});

}  // namespace

std::string config_dir() { return "/config"; }
std::string runtime_dir() {
    std::filesystem::create_directories("/tmp/noise-edit");
    std::filesystem::create_directories("/downloads");
    std::filesystem::create_directories("/uploads");
    return "/tmp/noise-edit";
}
std::string executable_path() { return {}; }
std::optional<std::string> env(const std::string& name) {
    if (const char* value = std::getenv(name.c_str())) { return value; }
    return std::nullopt;
}
double now_seconds() { return emscripten_get_now() * 0.001; }
std::int64_t now_unix_ms() {
    return static_cast<std::int64_t>(emscripten_date_now());
}
std::uint64_t random_seed() {
    std::uint64_t seed = 0;
    fill_random_js(reinterpret_cast<unsigned char*>(&seed));
    return seed;
}
void sleep_ms(int) {}  // A browser's render thread must never sleep.
std::int64_t pid() { return 0; }
std::optional<char> localized_shortcut_letter(char physical_letter) {
    const int letter = layout_letter_js(static_cast<int>(physical_letter));
    return letter >= 'A' && letter <= 'Z' ? std::optional<char>(static_cast<char>(letter))
                                         : std::nullopt;
}

std::string browser_config() {
    char* text = read_config_js();
    if (text == nullptr) { return "{}"; }
    std::string value(text);
    std::free(text);
    return value;
}

bool save_browser_config(const std::string& text, std::string& error) {
    if (!save_config_js(text.c_str())) {
        error = "browser storage is unavailable (localStorage blocked or full)";
        return false;
    }
    return true;
}

bool download_file(const std::string& path, std::string& error) {
    if (!download_js(path.c_str())) {
        error = "browser download failed for " + path;
        return false;
    }
    return true;
}
void enable_browser_drop() { enable_drop_js(); }
void set_browser_theme(bool light) { set_theme_js(light ? 1 : 0); }

}  // namespace noise::platform
