# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.24)
project(noise_edit CXX)

# The vcpkg static Windows triplet uses /MT. All project and FetchContent targets
# must use the same CRT, or MSVC fails with LNK2038 RuntimeLibrary mismatches.
if(MSVC AND VCPKG_TARGET_TRIPLET MATCHES "-static$")
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

include(GNUInstallDirs)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "" FORCE)
endif()

if(MSVC)
    add_compile_options(/utf-8)
    set(NOISE_DEFAULT_WERROR OFF)
else()
    set(NOISE_DEFAULT_WERROR ON)
endif()
option(NOISE_WERROR "Treat warnings as errors in noise targets" ${NOISE_DEFAULT_WERROR})
if(EMSCRIPTEN)
    set(NOISE_DEFAULT_TESTS OFF)
else()
    set(NOISE_DEFAULT_TESTS ON)
endif()
option(NOISE_BUILD_TESTS "Build tests" ${NOISE_DEFAULT_TESTS})
if(EMSCRIPTEN AND NOISE_BUILD_TESTS)
    message(FATAL_ERROR "Browser builds do not run the native test targets; configure with -DNOISE_BUILD_TESTS=OFF")
endif()

if(EMSCRIPTEN)
    # Use Emscripten's codec ports, not host libraries from pkg-config.
    set(NOISE_WEB_CODECS -sUSE_LIBPNG=1 -sUSE_LIBJPEG=1 -sUSE_ZLIB=1
                         --use-port=contrib.libwebp)
elseif(WIN32)
    # Use vcpkg-provided CMake targets, not pkg-config or Unix sonames.
    find_package(PNG REQUIRED)
    find_package(JPEG REQUIRED)
    find_package(WebP CONFIG REQUIRED)
    find_package(CURL REQUIRED)
    find_package(ZLIB REQUIRED)
else()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(LIBPNG REQUIRED libpng16)
    pkg_check_modules(LIBJPEG REQUIRED libjpeg)
    pkg_check_modules(LIBWEBP REQUIRED libwebp)
    find_package(ZLIB REQUIRED)
endif()

include(FetchContent)

# Pinned: sokol master 2026-10-07 (resource API rewrite in progress -> must stay pinned).
FetchContent_Declare(sokol
    GIT_REPOSITORY https://github.com/floooh/sokol
    GIT_TAG        401f21f8b7039258c35fef75c11d9a8a0e616771)
# Dear ImGui, docking branch.
FetchContent_Declare(imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui
    GIT_TAG        v1.93.0-docking
    GIT_SHALLOW    TRUE)
FetchContent_Declare(json
    GIT_REPOSITORY https://github.com/nlohmann/json
    GIT_TAG        v3.11.3
    GIT_SHALLOW    TRUE
    SYSTEM         TRUE)
FetchContent_MakeAvailable(sokol imgui json)

if(NOT EMSCRIPTEN)
    set(NFD_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(NFD_INSTALL OFF CACHE BOOL "" FORCE)
    if(NOT NFD_PORTAL)
        set(NFD_APPEND_EXTENSION ON CACHE BOOL "" FORCE)
    endif()
    set(NFD_WAYLAND OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(nfd
        GIT_REPOSITORY https://github.com/btzy/nativefiledialog-extended.git
        GIT_TAG        v1.4.1
        GIT_SHALLOW    TRUE)
    FetchContent_MakeAvailable(nfd)
endif()

if(NOT EMSCRIPTEN)
    find_package(OpenGL REQUIRED)
    find_package(Threads REQUIRED)
    if(UNIX AND NOT APPLE)
        find_package(X11 REQUIRED COMPONENTS Xcursor Xi)
        pkg_check_modules(LIBCURL REQUIRED IMPORTED_TARGET libcurl)
    endif()
endif()

add_library(sokol STATIC vendor/sokol_impl.cpp)
if(EMSCRIPTEN)
    target_compile_definitions(sokol PUBLIC SOKOL_GLES3=1 SOKOL_NO_ENTRY=1)
elseif(WIN32)
    target_compile_definitions(sokol PUBLIC SOKOL_GLCORE=1 SOKOL_NO_ENTRY=1)
    target_link_libraries(sokol PUBLIC OpenGL::GL user32 gdi32 shell32)
else()
    target_compile_definitions(sokol PUBLIC SOKOL_GLCORE=1 SOKOL_NO_ENTRY=1)
    target_link_libraries(sokol PUBLIC OpenGL::GL X11::X11 ${X11_Xcursor_LIB} ${X11_Xi_LIB} m Threads::Threads dl)
endif()
target_include_directories(sokol SYSTEM PUBLIC ${sokol_SOURCE_DIR})

add_library(imgui STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/misc/cpp/imgui_stdlib.cpp
    vendor/sokol_imgui_impl.cpp)
target_include_directories(imgui SYSTEM PUBLIC
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/misc/cpp
    ${sokol_SOURCE_DIR}
    ${sokol_SOURCE_DIR}/util)
target_link_libraries(imgui PUBLIC sokol)

foreach(tgt sokol imgui)
    if(MSVC)
        target_compile_options(${tgt} PRIVATE /W0)
    else()
        target_compile_options(${tgt} PRIVATE -w)
    endif()
endforeach()

if(EMSCRIPTEN)
    add_library(noise_platform STATIC
        src/platform/web.cpp
        src/platform/net_web.cpp
        src/platform/file_dialog_web.cpp)
elseif(WIN32)
    add_library(noise_platform STATIC
        src/platform/windows.cpp
        src/platform/windows_net.cpp
        src/platform/file_dialog_filters.cpp)
    target_link_libraries(noise_platform PUBLIC ws2_32)
else()
    add_library(noise_platform STATIC
        src/platform/posix.cpp
        src/platform/posix_net.cpp
        src/platform/file_dialog_filters.cpp)
endif()
target_include_directories(noise_platform PUBLIC src)
if(EMSCRIPTEN)
    target_link_libraries(noise_platform PRIVATE nlohmann_json::nlohmann_json)
endif()

add_library(noise_core STATIC
    src/core/log.cpp
    src/core/hash.cpp
    src/core/base64.cpp
    src/core/blend.cpp
    src/core/blur.cpp
    src/core/command.cpp
    src/core/brush.cpp
    src/core/document.cpp
    src/core/geom.cpp
    src/core/history.cpp
    src/core/image.cpp
    src/core/layer.cpp
    src/core/mask.cpp
    src/core/maskops.cpp
    src/core/resample.cpp
    src/core/shortcut.cpp)
target_include_directories(noise_core PUBLIC src)
target_link_libraries(noise_core PUBLIC noise_platform nlohmann_json::nlohmann_json)
if(NOT EMSCRIPTEN)
    target_link_libraries(noise_core PUBLIC Threads::Threads)
endif()

# File formats live here, not in core: core is pixels and maths with no IO, which is what
# keeps it testable headless and the codecs swappable.
add_library(noise_io STATIC
    src/io/file.cpp
    src/io/flat.cpp
    src/io/config.cpp
    src/io/jpeg.cpp
    src/io/png.cpp
    src/io/project.cpp
    src/io/webp.cpp
    src/io/zip.cpp)
target_include_directories(noise_io PUBLIC src)
target_link_libraries(noise_io PUBLIC noise_core)
if(EMSCRIPTEN)
    target_compile_options(noise_io PRIVATE ${NOISE_WEB_CODECS})
elseif(WIN32)
    target_link_libraries(noise_io PUBLIC PNG::PNG JPEG::JPEG WebP::webp ZLIB::ZLIB)
else()
    target_include_directories(noise_io PUBLIC ${LIBPNG_INCLUDE_DIRS} ${LIBJPEG_INCLUDE_DIRS}
                                         ${LIBWEBP_INCLUDE_DIRS})
    target_link_libraries(noise_io PUBLIC ${LIBPNG_LIBRARIES} ${LIBJPEG_LIBRARIES}
                            ${LIBWEBP_LIBRARIES} ZLIB::ZLIB)
    target_compile_options(noise_io PUBLIC ${LIBPNG_CFLAGS_OTHER})
endif()

# The AI layer: the region compiler and the result compositor are pixels and maths, so they are
# headless and testable like core; the transport that speaks HTTP lives beside them and links curl.
add_library(noise_ai STATIC
    src/ai/colour_match.cpp
    src/ai/compose.cpp
    src/ai/http.cpp
    src/ai/profile.cpp
    src/ai/provider.cpp
    src/ai/queue.cpp
    src/ai/recipes.cpp
    src/ai/region.cpp
    src/ai/recorded.cpp
    src/ai/registry.cpp
    src/ai/server.cpp)
target_include_directories(noise_ai PUBLIC src)
target_link_libraries(noise_ai PUBLIC noise_core noise_io)
if(NOT EMSCRIPTEN)
    target_sources(noise_ai PRIVATE src/ai/http_curl.cpp)
    if(WIN32)
        target_link_libraries(noise_ai PUBLIC CURL::libcurl)
    else()
        target_link_libraries(noise_ai PUBLIC PkgConfig::LIBCURL)
    endif()
endif()

add_library(noise_gfx STATIC
    src/gfx/checker.cpp
    src/gfx/renderer.cpp)
target_include_directories(noise_gfx PUBLIC src)
target_link_libraries(noise_gfx PUBLIC noise_core sokol imgui)
if(NOT EMSCRIPTEN)
    target_link_libraries(noise_gfx PUBLIC OpenGL::GL)
endif()

add_library(noise_dbg STATIC
    src/dbg/wire.cpp)
target_include_directories(noise_dbg PUBLIC src)
target_link_libraries(noise_dbg PUBLIC noise_core)

# The command table holds the app's whole behaviour surface and needs no GPU, so
# it is its own target: tests drive real commands through it.
add_library(noise_app STATIC
    src/app/ai_commands.cpp
    src/app/ai_job_commands.cpp
    src/app/ai_intent.cpp
    src/app/ai_landing.cpp
    src/app/ai_references.cpp
    src/app/ai_session.cpp
    src/app/ai_verbs.cpp
    src/app/command_args.cpp
    src/app/commands.cpp
    src/app/document_commands.cpp
    src/app/file_commands.cpp
    src/app/layer_commands.cpp
    src/app/paint_commands.cpp
    src/app/ants.cpp
    src/app/select_commands.cpp
    src/app/mask_commands.cpp
    src/app/coverage.cpp
    src/app/coverage_commands.cpp
    src/app/verb_bar.cpp
    src/app/verb_fields.cpp
    src/app/tools/brush.cpp
    src/app/tools/cover.cpp
    src/app/tools/move.cpp
    src/app/tools/select.cpp
    src/app/view_commands.cpp
    src/app/pointer.cpp)
target_include_directories(noise_app PUBLIC src)
target_link_libraries(noise_app PUBLIC noise_ai noise_dbg noise_core noise_io noise_platform)
if(EMSCRIPTEN)
    set(NOISE_INSTALL_ASSET_RELATIVE "/assets")
else()
    file(RELATIVE_PATH NOISE_INSTALL_ASSET_RELATIVE
        "${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_BINDIR}"
        "${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_DATADIR}/thenoise-edit")
endif()
target_compile_definitions(noise_app PRIVATE
    "NOISE_INSTALL_ASSET_RELATIVE=\"${NOISE_INSTALL_ASSET_RELATIVE}\"")

# The host side of the UI: it needs sokol's event and keycode types, so the key-event
# translation builds with this target and not with the windowless command table.
add_library(noise_ui STATIC
    src/app/shortcuts.cpp
    src/app/ui.cpp
    src/app/ui_canvas.cpp
    src/app/ui_front_door.cpp
    src/app/ui_layers_panel.cpp
    src/app/ui_theme.cpp
    src/app/ui_tool_panel.cpp
    src/app/ui_verbs.cpp
    src/app/ui_references.cpp
    src/app/verb_bar_draw.cpp
    src/app/coverage_tint.cpp
    src/app/layer_thumbs.cpp
    src/app/reference_thumbs.cpp)
target_include_directories(noise_ui PUBLIC src)
target_link_libraries(noise_ui PUBLIC noise_app noise_gfx imgui sokol)
if(NOT EMSCRIPTEN)
    target_sources(noise_ui PRIVATE src/app/ui_file_dialog.cpp src/platform/file_dialog_nfd.cpp)
    target_link_libraries(noise_ui PUBLIC nfd)
endif()

add_executable(noise-edit
    src/main.cpp)
target_link_libraries(noise-edit PRIVATE noise_ui noise_gfx noise_dbg noise_core noise_platform)

if(EMSCRIPTEN)
    set_target_properties(noise-edit PROPERTIES SUFFIX ".html")
    target_link_options(noise-edit PRIVATE
        ${NOISE_WEB_CODECS}
        -sUSE_WEBGL2=1
        -sMIN_WEBGL_VERSION=2
        -sMAX_WEBGL_VERSION=2
        -sALLOW_MEMORY_GROWTH=1
        -sFORCE_FILESYSTEM=1
        -sEXPORTED_RUNTIME_METHODS=FS,ccall
        -sEXPORTED_FUNCTIONS=_main,_malloc,_free
        -sENVIRONMENT=web
        --pre-js "${CMAKE_SOURCE_DIR}/web/bridge.js"
        --shell-file "${CMAKE_SOURCE_DIR}/web/shell.html"
        --preload-file "${CMAKE_SOURCE_DIR}/assets@/assets")
else()
    install(TARGETS noise-edit RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
    install(DIRECTORY assets/ DESTINATION ${CMAKE_INSTALL_DATADIR}/thenoise-edit)
    install(FILES "${nfd_SOURCE_DIR}/LICENSE"
            DESTINATION "${CMAKE_INSTALL_DATADIR}/licenses/thenoise-edit"
            RENAME NativeFileDialog-Extended-LICENSE)
endif()

if(EMSCRIPTEN)
    # JSON parsing and file decoders rely on catchable C++ errors.
    foreach(tgt noise_platform noise_core noise_io noise_ai noise_gfx noise_dbg
                noise_app noise_ui noise-edit)
        target_compile_options(${tgt} PRIVATE -fwasm-exceptions)
    endforeach()
    target_link_options(noise-edit PRIVATE -fwasm-exceptions)
endif()

if(MSVC)
    # MSVC's diagnostic flags are not GCC flags. Keep /WX opt-in on a new platform
    # until the native Windows build has been exercised in CI.
    if(NOISE_WERROR)
        message(STATUS "NOISE_WERROR enabled: MSVC diagnostics are errors")
        set(NOISE_WARNINGS /W4 /WX)
    else()
        set(NOISE_WARNINGS /W4)
    endif()
else()
    set(NOISE_WARNINGS -Wall -Wextra -Werror -Wshadow -Wconversion -Wsign-conversion)
endif()
if(MSVC OR NOISE_WERROR)
    foreach(tgt noise_platform noise_core noise_io noise_gfx noise_dbg noise_app noise_ui
                    noise-edit)
        target_compile_options(${tgt} PRIVATE ${NOISE_WARNINGS})
    endforeach()
endif()

if(NOISE_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
