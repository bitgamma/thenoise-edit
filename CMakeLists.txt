# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.24)
project(noise_edit CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "" FORCE)
endif()

option(NOISE_WERROR "Treat warnings as errors in noise targets" ON)
option(NOISE_BUILD_TESTS "Build tests" ON)

find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBPNG REQUIRED libpng16)
pkg_check_modules(LIBJPEG REQUIRED libjpeg)
pkg_check_modules(LIBWEBP REQUIRED libwebp)
find_package(ZLIB REQUIRED)  # crc32 and inflate for the .noise container (libpng already needs it)

include(FetchContent)

# Pinned: sokol master 2026-10-07 (resource API rewrite in progress -> must stay pinned).
FetchContent_Declare(sokol
    GIT_REPOSITORY https://github.com/floooh/sokol
    GIT_TAG        401f21f8b7039258c35fef75c11d9a8a0e616771
    GIT_SHALLOW    TRUE)
# Dear ImGui, docking branch.
FetchContent_Declare(imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui
    GIT_TAG        778992face67ecefaa5c2932db4b2101545ea4df
    GIT_SHALLOW    TRUE)
FetchContent_Declare(json
    GIT_REPOSITORY https://github.com/nlohmann/json
    GIT_TAG        v3.11.3
    GIT_SHALLOW    TRUE
    SYSTEM         TRUE)
FetchContent_MakeAvailable(sokol imgui json)

find_package(OpenGL REQUIRED)
find_package(X11 REQUIRED COMPONENTS Xcursor Xi)
find_package(Threads REQUIRED)

add_library(sokol STATIC vendor/sokol_impl.cpp)
target_compile_definitions(sokol PUBLIC SOKOL_GLCORE=1 SOKOL_NO_ENTRY=1)
target_include_directories(sokol SYSTEM PUBLIC ${sokol_SOURCE_DIR})
target_link_libraries(sokol PUBLIC OpenGL::GL X11::X11 ${X11_Xcursor_LIB} ${X11_Xi_LIB} m Threads::Threads dl)

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
    target_compile_options(${tgt} PRIVATE -w)
endforeach()

add_library(noise_platform STATIC
    src/platform/posix.cpp
    src/platform/posix_net.cpp)
target_include_directories(noise_platform PUBLIC src)

add_library(noise_core STATIC
    src/core/log.cpp
    src/core/hash.cpp
    src/core/base64.cpp
    src/core/blend.cpp
    src/core/command.cpp
    src/core/brush.cpp
    src/core/document.cpp
    src/core/geom.cpp
    src/core/history.cpp
    src/core/image.cpp
    src/core/layer.cpp
    src/core/mask.cpp
    src/core/maskops.cpp
    src/core/shortcut.cpp)
target_include_directories(noise_core PUBLIC src)
target_link_libraries(noise_core PUBLIC noise_platform nlohmann_json::nlohmann_json Threads::Threads)

# File formats live here, not in core: core is pixels and maths with no IO, which is what
# keeps it testable headless and the codecs swappable.
add_library(noise_io STATIC
    src/io/file.cpp
    src/io/flat.cpp
    src/io/jpeg.cpp
    src/io/png.cpp
    src/io/project.cpp
    src/io/webp.cpp
    src/io/zip.cpp)
target_include_directories(noise_io PUBLIC src)
target_link_libraries(noise_io PUBLIC noise_core)
target_include_directories(noise_io PUBLIC ${LIBPNG_INCLUDE_DIRS} ${LIBJPEG_INCLUDE_DIRS}
                                     ${LIBWEBP_INCLUDE_DIRS})
target_link_libraries(noise_io PUBLIC ${LIBPNG_LIBRARIES} ${LIBJPEG_LIBRARIES}
                        ${LIBWEBP_LIBRARIES} ZLIB::ZLIB)
target_compile_options(noise_io PUBLIC ${LIBPNG_CFLAGS_OTHER})

# The AI layer: the region compiler and the result compositor are pixels and maths, so they are
# headless and testable like core; the transport that speaks HTTP lives beside them and links curl.
add_library(noise_ai STATIC
    src/ai/region.cpp)
target_include_directories(noise_ai PUBLIC src)
target_link_libraries(noise_ai PUBLIC noise_core)

add_library(noise_gfx STATIC
    src/gfx/checker.cpp
    src/gfx/renderer.cpp)
target_include_directories(noise_gfx PUBLIC src)
target_link_libraries(noise_gfx PUBLIC noise_core sokol imgui OpenGL::GL)

add_library(noise_dbg STATIC
    src/dbg/wire.cpp)
target_include_directories(noise_dbg PUBLIC src)
target_link_libraries(noise_dbg PUBLIC noise_core)

# The command table holds the app's whole behaviour surface and needs no GPU, so
# it is its own target: tests drive real commands through it.
add_library(noise_app STATIC
    src/app/command_args.cpp
    src/app/commands.cpp
    src/app/document_commands.cpp
    src/app/layer_commands.cpp
    src/app/paint_commands.cpp
    src/app/ants.cpp
    src/app/select_commands.cpp
    src/app/mask_commands.cpp
    src/app/tools/brush.cpp
    src/app/tools/move.cpp
    src/app/tools/select.cpp
    src/app/view_commands.cpp)
target_include_directories(noise_app PUBLIC src)
target_link_libraries(noise_app PUBLIC noise_dbg noise_core noise_io noise_platform)

# The host side of the UI: it needs sokol's event and keycode types, so the key-event
# translation builds with this target and not with the windowless command table.
add_library(noise_ui STATIC
    src/app/shortcuts.cpp
    src/app/ui.cpp)
target_include_directories(noise_ui PUBLIC src)
target_link_libraries(noise_ui PUBLIC noise_app noise_gfx imgui sokol)

add_executable(noise-edit
    src/main.cpp)
target_link_libraries(noise-edit PRIVATE noise_ui noise_gfx noise_dbg noise_core noise_platform)

if(NOISE_WERROR)
    set(NOISE_WARNINGS -Wall -Wextra -Werror -Wshadow -Wconversion -Wsign-conversion)
    foreach(tgt noise_platform noise_core noise_io noise_gfx noise_dbg noise_app noise_ui
                    noise-edit)
        target_compile_options(${tgt} PRIVATE ${NOISE_WARNINGS})
    endforeach()
endif()

if(NOISE_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
