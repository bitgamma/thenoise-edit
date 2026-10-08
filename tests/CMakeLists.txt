# SPDX-License-Identifier: Apache-2.0

add_executable(test_hash test_hash.cpp)
add_executable(test_geom test_geom.cpp)
add_executable(test_image test_image.cpp)
add_executable(test_command test_command.cpp)
add_executable(test_wire test_wire.cpp)
add_executable(test_app test_app.cpp)

foreach(target test_hash test_geom test_image test_command test_wire)
    target_link_libraries(${target} PRIVATE noise_core noise_dbg)
    target_include_directories(${target} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_options(${target} PRIVATE -Wall -Wextra -Werror -Wshadow)
    add_test(NAME ${target} COMMAND ${target})
endforeach()

# The command table is windowless, so the app's own behaviour is testable here. The
# harness injects its own runtime dir, so the test runs standalone as well as under ctest.
target_link_libraries(test_app PRIVATE noise_app noise_core noise_dbg)
target_include_directories(test_app PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(test_app PRIVATE -Wall -Wextra -Werror -Wshadow)
add_test(NAME test_app COMMAND test_app)

find_package(Python3 COMPONENTS Interpreter)
if(Python3_Interpreter_FOUND)
    # M0 acceptance: drive pan+zoom over the wire against the running app, capture the
    # canvas, diff against the golden. Skips loudly when it cannot render (no DISPLAY,
    # no xvfb-run) rather than going red on a headless box.
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
