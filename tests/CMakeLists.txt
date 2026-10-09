# SPDX-License-Identifier: Apache-2.0

add_executable(test_hash test_hash.cpp)
add_executable(test_geom test_geom.cpp)
add_executable(test_image test_image.cpp)
add_executable(test_command test_command.cpp)
add_executable(test_wire test_wire.cpp)
add_executable(test_app test_app.cpp)
add_executable(test_shortcut test_shortcut.cpp)
add_executable(test_brush test_brush.cpp)
add_executable(test_history test_history.cpp)
add_executable(test_zip test_zip.cpp)
add_executable(test_project test_project.cpp)
add_executable(test_document test_document.cpp)

foreach(target test_hash test_geom test_command test_wire test_brush test_history)
    target_link_libraries(${target} PRIVATE noise_core noise_dbg)
    target_include_directories(${target} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_options(${target} PRIVATE -Wall -Wextra -Werror -Wshadow)
    add_test(NAME ${target} COMMAND ${target})
endforeach()

# The document model: what a stack composites to, and the invariants a stack keeps.
target_link_libraries(test_document PRIVATE noise_core noise_dbg)
target_include_directories(test_document PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(test_document PRIVATE -Wall -Wextra -Werror -Wshadow -Wconversion -Wsign-conversion)
add_test(NAME test_document COMMAND test_document)

# The container reader is a parser for files from elsewhere, so it gets the hostile-input
# tests: crc, truncation, and a directory that lies.
target_link_libraries(test_zip PRIVATE noise_core noise_io ZLIB::ZLIB)
target_include_directories(test_zip PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(test_zip PRIVATE -Wall -Wextra -Werror -Wshadow -Wconversion -Wsign-conversion)
add_test(NAME test_zip COMMAND test_zip)

# The container: what a .noise file must guarantee, checked against files this app writes and
# files it would never write.
target_link_libraries(test_project PRIVATE noise_core noise_io noise_platform)
target_include_directories(test_project PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(test_project PRIVATE -Wall -Wextra -Werror -Wshadow -Wconversion -Wsign-conversion)
add_test(NAME test_project COMMAND test_project)

# Pixels plus the codec that reads and writes them.
target_link_libraries(test_image PRIVATE noise_core noise_io noise_dbg)
target_include_directories(test_image PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(test_image PRIVATE -Wall -Wextra -Werror -Wshadow)
add_test(NAME test_image COMMAND test_image)

# The command table is windowless, so the app's own behaviour is testable here. The
# harness injects its own runtime dir, so the test runs standalone as well as under ctest.
target_link_libraries(test_app PRIVATE noise_app noise_core noise_dbg)
target_include_directories(test_app PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(test_app PRIVATE -Wall -Wextra -Werror -Wshadow)
add_test(NAME test_app COMMAND test_app)

# Shortcut text, table bindings and the platform key translation: sokol headers are needed
# to build a sapp_event, but nothing is initialized, so it stays a windowless test.
target_link_libraries(test_shortcut PRIVATE noise_app noise_ui noise_core noise_dbg sokol imgui)
target_include_directories(test_shortcut PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(test_shortcut PRIVATE -Wall -Wextra -Werror -Wshadow -Wconversion -Wsign-conversion)
add_test(NAME test_shortcut COMMAND test_shortcut)

find_package(Python3 COMPONENTS Interpreter)
if(Python3_Interpreter_FOUND)
    # M0 acceptance: drive pan+zoom over the wire against the running app, capture the
    # canvas, diff against the golden. Skips loudly when it cannot render (no DISPLAY,
    # no xvfb-run) rather than going red on a headless box.
    # Injected key and mouse events: the only way to check that a gesture reaches the
    # command table. Skips when it can neither inject input nor make a display.
    add_test(NAME test_input_gestures
             COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/wire_input.py
                     --bin $<TARGET_FILE:noise-edit>)
    set_tests_properties(test_input_gestures PROPERTIES
                         SKIP_REGULAR_EXPRESSION "SKIP:"
                         TIMEOUT 120
                         DEPENDS noise-edit)

    # M1 acceptance: a 5000-dab stroke over 2048^2 with the app still drivable, and undo
    # returning every byte. Records timings; asserts only wide behavioural bounds.
    add_test(NAME test_stroke_probe
             COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/stroke_probe.py
                     --bin $<TARGET_FILE:noise-edit>)
    set_tests_properties(test_stroke_probe PROPERTIES
                         SKIP_REGULAR_EXPRESSION "SKIP:"
                         TIMEOUT 180
                         DEPENDS noise-edit)

    # The container read by an implementation that knows nothing about us: our reader agreeing
    # with our own writer proves almost nothing about the bytes on disk.
    add_test(NAME test_container_interop
             COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/container_probe.py
                     --bin $<TARGET_FILE:noise-edit>)
    set_tests_properties(test_container_interop PROPERTIES
                         SKIP_REGULAR_EXPRESSION "SKIP:"
                         TIMEOUT 120
                         DEPENDS noise-edit)

    add_test(NAME test_view_golden
             COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/wire_drive.py
                     --bin $<TARGET_FILE:noise-edit>
                     --golden ${CMAKE_SOURCE_DIR}/tests/golden/view_pan_zoom.png)
    set_tests_properties(test_view_golden PROPERTIES
                         SKIP_REGULAR_EXPRESSION "SKIP:"
                         TIMEOUT 180
                         DEPENDS noise-edit)
endif()

# Live-tier tests (tagged `live`) run only when THENOISE_URL and THENOISE_API_KEY
# are exported; see AGENTS.md §2. They are Python drivers over the app's debug
# wire plus recorded fixtures, and land with M3.
