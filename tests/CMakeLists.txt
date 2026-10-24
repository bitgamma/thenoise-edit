# SPDX-License-Identifier: Apache-2.0

add_executable(test_hash test_hash.cpp)
add_executable(test_geom test_geom.cpp)
add_executable(test_image test_image.cpp)
add_executable(test_command test_command.cpp)
add_executable(test_wire test_wire.cpp)
add_executable(test_app test_app.cpp)
add_executable(test_file_dialog test_file_dialog.cpp)
add_executable(test_shortcut test_shortcut.cpp)
add_executable(test_brush test_brush.cpp)
add_executable(test_history test_history.cpp)
add_executable(test_zip test_zip.cpp)
add_executable(test_project test_project.cpp)
add_executable(test_document test_document.cpp)
add_executable(test_blend test_blend.cpp)
add_executable(test_mask test_mask.cpp)
add_executable(test_ants test_ants.cpp)
add_executable(test_maskops test_maskops.cpp)
add_executable(test_region test_region.cpp)
add_executable(test_blur test_blur.cpp)
add_executable(test_resample test_resample.cpp)
add_executable(test_compose test_compose.cpp)
add_executable(test_roles test_roles.cpp)
add_executable(test_recipes test_recipes.cpp)
add_executable(test_provider test_provider.cpp)
add_executable(test_backend test_backend.cpp)

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

# The blend arithmetic, checked against the definitions written out again in the test rather than
# against the implementation: a mode that agrees with a shared helper agrees with nothing.
target_link_libraries(test_blend PRIVATE noise_core noise_dbg)
target_include_directories(test_blend PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(test_blend PRIVATE -Wall -Wextra -Werror -Wshadow -Wconversion -Wsign-conversion)
add_test(NAME test_blend COMMAND test_blend)

# What a coverage plane does to a write, checked against source-over written out again in the test:
# the claim that the document can restrict a write after the fact rests on the two being the same.
target_link_libraries(test_maskops PRIVATE noise_core noise_dbg)
target_include_directories(test_maskops PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(test_maskops PRIVATE -Wall -Wextra -Werror -Wshadow -Wconversion -Wsign-conversion)
add_test(NAME test_maskops COMMAND test_maskops)

target_link_libraries(test_mask PRIVATE noise_core noise_dbg)
target_include_directories(test_mask PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(test_mask PRIVATE -Wall -Wextra -Werror -Wshadow -Wconversion -Wsign-conversion)
add_test(NAME test_mask COMMAND test_mask)

target_link_libraries(test_ants PRIVATE noise_app noise_core noise_dbg)
target_include_directories(test_ants PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(test_ants PRIVATE -Wall -Wextra -Werror -Wshadow -Wconversion -Wsign-conversion)
add_test(NAME test_ants COMMAND test_ants)

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
# The harness loads the shipped tables from the tree: roles and recipes are data the app is expected to
# read, and a test that stubbed them would not be testing the resolution.
target_compile_definitions(test_app
    PRIVATE NOISE_TEST_ASSETS_DIR="${CMAKE_SOURCE_DIR}/assets"
            NOISE_TEST_FIXTURES_DIR="${CMAKE_SOURCE_DIR}/tests/fixtures/http")
target_compile_options(test_app PRIVATE -Wall -Wextra -Werror -Wshadow)
add_test(NAME test_app COMMAND test_app)

# Asking the desktop for a path: the argv the dialog is given, and the answer that comes back through
# the pipe, driven by a stand-in program so no hermetic run depends on a desktop being installed.
target_link_libraries(test_file_dialog PRIVATE noise_app noise_core noise_dbg)
target_include_directories(test_file_dialog PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(test_file_dialog
    PRIVATE NOISE_TEST_ASSETS_DIR="${CMAKE_SOURCE_DIR}/assets")
target_compile_options(test_file_dialog PRIVATE -Wall -Wextra -Werror -Wshadow -Wconversion -Wsign-conversion)
add_test(NAME test_file_dialog COMMAND test_file_dialog)

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

    # The whole AI path on the running app, against the recordings: discovery, role resolution, the
    # compiled request, the queue, the landing, and the pixels the report claims.
    add_test(NAME test_ai_job_probe
             COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/ai_job_probe.py
                     --bin $<TARGET_FILE:noise-edit> --generate)
    set_tests_properties(test_ai_job_probe PROPERTIES
                         SKIP_REGULAR_EXPRESSION "SKIP:"
                         TIMEOUT 600)

    # What the model is actually shown, whole-image: an in-place change uploads the region untouched and
    # a reconstruction destroys only the selected area. This is the claim that was false when Replace
    # blurred the whole crop, and only the running app's compile produces these bytes.
    add_test(NAME test_prep_proof
             COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/prep_proof.py
                     --bin $<TARGET_FILE:noise-edit>)
    set_tests_properties(test_prep_proof PROPERTIES
                         SKIP_REGULAR_EXPRESSION "SKIP:"
                         TIMEOUT 300
                         DEPENDS noise-edit)

    # The panel's own door: injected keys into a verb's words box, then that verb's key. Every other

    # The two dropdowns the generate row grew: a click pair must change the aspect the intent holds and
    # the size quoted for it, then the style, then the key must run the recipe the style selected. The
    # commands are tested hermetically; only this can see that the widgets commit.
    add_test(NAME test_panel_aspect_probe
             COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/panel_aspect_probe.py
                     --bin $<TARGET_FILE:noise-edit>)
    set_tests_properties(test_panel_aspect_probe PROPERTIES
                         SKIP_REGULAR_EXPRESSION "SKIP:"
                         TIMEOUT 420
                         DEPENDS noise-edit)

    # The bar that comes to a marked area: where it drew itself, what it offered, and whether a click
    # on one of its buttons is a press of that verb. Only the window can answer this, and `verb.bar`
    # exists so the probe presses the bar rather than a remembered coordinate.
    add_test(NAME test_verb_bar_probe
             COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/verb_bar_probe.py
                     --bin $<TARGET_FILE:noise-edit>)
    set_tests_properties(test_verb_bar_probe PROPERTIES
                         SKIP_REGULAR_EXPRESSION "SKIP:"
                         TIMEOUT 420
                         DEPENDS noise-edit)

    # That marked coverage is a wash over the covered area and nowhere else: a haze over the whole
    # document, or a wash that misses the part of the mark away from the origin, both fail here, and
    # only a capture can say.
    add_test(NAME test_coverage_view_probe
             COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/coverage_view_probe.py
                     --bin $<TARGET_FILE:noise-edit>)
    set_tests_properties(test_coverage_view_probe PROPERTIES
                         SKIP_REGULAR_EXPRESSION "SKIP:"
                         TIMEOUT 420
                         DEPENDS noise-edit)

    # That the boundary walk, windowed to the selection's own bounds, still outlines all four sides of a
    # selection that sits far from the origin of a large document. `test_ants` checks the geometry; only a
    # capture says the window drew what the geometry described.
    add_test(NAME test_ants_window_probe
             COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/ants_window_probe.py
                     --bin $<TARGET_FILE:noise-edit>)
    set_tests_properties(test_ants_window_probe PROPERTIES
                         SKIP_REGULAR_EXPRESSION "SKIP:"
                         TIMEOUT 420
                         DEPENDS noise-edit)

    # That each layer has its own surface in the panel and its own picture on it: the thumbnail is the
    # layer's content, an opacity change leaves it byte-identical, and a stroke into the layer changes it.
    add_test(NAME test_layer_panel_probe
             COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/layer_panel_probe.py
                     --bin $<TARGET_FILE:noise-edit>)
    set_tests_properties(test_layer_panel_probe PROPERTIES
                         SKIP_REGULAR_EXPRESSION "SKIP:"
                         TIMEOUT 420
                         DEPENDS noise-edit)

    # That the ring on the canvas is the brush: measured off a capture against the size and zoom the app
    # reports, because a cursor that cannot show its own size is a tool nobody can aim.
    add_test(NAME test_pointer_probe
             COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/pointer_probe.py
                     --bin $<TARGET_FILE:noise-edit>)
    set_tests_properties(test_pointer_probe PROPERTIES
                         SKIP_REGULAR_EXPRESSION "SKIP:"
                         TIMEOUT 420
                         DEPENDS noise-edit)

    # Whether the desktop will hand this window a file at all, and whether a driver can do what a drop
    # does. The drag itself needs a second program and a real pointer, so these are the two halves that
    # are observable headless.
    add_test(NAME test_drop_target_probe
             COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/drop_target_probe.py
                     --bin $<TARGET_FILE:noise-edit>)
    set_tests_properties(test_drop_target_probe PROPERTIES
                         SKIP_REGULAR_EXPRESSION "SKIP:"
                         TIMEOUT 180
                         DEPENDS noise-edit)

    add_test(NAME test_ai_compose_probe
             COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/ai_compose_probe.py
                     --bin $<TARGET_FILE:noise-edit>)
    set_tests_properties(test_ai_compose_probe PROPERTIES
                         SKIP_REGULAR_EXPRESSION "SKIP:"
                         TIMEOUT 120
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

    # Our encoder agreeing with our decoder proves the pair is self-consistent, nothing more: a
    # header wrong on both sides keeps every in-house test green. So the flat formats cross to
    # Pillow and come back, both ways, through the running app.
    add_test(NAME test_image_interop
             COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/image_interop_probe.py
                     --bin $<TARGET_FILE:noise-edit>)
    set_tests_properties(test_image_interop PROPERTIES
                         SKIP_REGULAR_EXPRESSION "SKIP:"
                         TIMEOUT 180
                         DEPENDS noise-edit)

    add_test(NAME test_view_golden
             COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/wire_drive.py
                     --bin $<TARGET_FILE:noise-edit>
                     --theme light
                     --golden ${CMAKE_SOURCE_DIR}/tests/golden/view_pan_zoom.png)
    set_tests_properties(test_view_golden PROPERTIES
                         SKIP_REGULAR_EXPRESSION "SKIP:"
                         TIMEOUT 180
                         DEPENDS noise-edit)
endif()

# Live-tier tests (label `live`) spend requests on the dev box, so they are registered only
# when configured with -DNOISE_LIVE_TESTS=ON: `ctest --test-dir build` stays hermetic even in a
# shell that happens to export the credentials. Run them with
# `cmake -B build -DNOISE_LIVE_TESTS=ON && ctest --test-dir build -L live`. Each still skips
# loudly without THENOISE_URL / THENOISE_API_KEY, so registering them is intent and the env is
# permission (AGENTS.md §2).
option(NOISE_LIVE_TESTS "Register tests that make requests against the dev server" OFF)
if(NOISE_LIVE_TESTS AND Python3_Interpreter_FOUND)
    # What the real server's contract actually is. Asserts discovery, explicit size, the shape
    # of a refusal and the 16px snap; records timings. --max-images caps the spend.
    # The same probe against the dev box: contract and pixel-lock only, timings printed for
    # docs/probes/, and a per-run image cap because the box is shared.
    add_test(NAME ai_job_probe_live
             COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/ai_job_probe.py
                     --bin $<TARGET_FILE:noise-edit> --live --max-images 2)
    set_tests_properties(ai_job_probe_live PROPERTIES
                         LABELS live
                         SKIP_REGULAR_EXPRESSION "SKIP:"
                         TIMEOUT 1800)

    # The aspect presets and the style family against the dev box: the frames the recipe offers, each
    # asked for and each checked for the promise a frame makes - the answer inside it, nothing outside it.
    add_test(NAME ai_job_probe_live_generate
             COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/ai_job_probe.py
                     --bin $<TARGET_FILE:noise-edit> --live --only generate --generate --max-images 5)
    set_tests_properties(ai_job_probe_live_generate PROPERTIES
                         LABELS live
                         SKIP_REGULAR_EXPRESSION "SKIP:"
                         TIMEOUT 1800)

    add_test(NAME tn_probe_live
             COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/tn_probe.py
                     --max-images 10 --upscale)
    set_tests_properties(tn_probe_live PROPERTIES
                         LABELS live
                         SKIP_REGULAR_EXPRESSION "SKIP:"
                         TIMEOUT 1800)
endif()

# The region compiler: geometry that must satisfy a size policy it did not choose, and preps measured
# against the statistics each one claims to keep or destroy.
target_link_libraries(test_region PRIVATE noise_ai noise_core noise_dbg)
target_include_directories(test_region PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(test_region PRIVATE -Wall -Wextra -Werror -Wshadow -Wconversion -Wsign-conversion)
add_test(NAME test_region COMMAND test_region)

# The two pixel operations the compositor is built out of: a softening that must never invent
# coverage, and a resample that must never bleed black out of a transparent pixel.
foreach(target test_blur test_resample)
    target_link_libraries(${target} PRIVATE noise_core noise_dbg)
    target_include_directories(${target} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_options(${target} PRIVATE -Wall -Wextra -Werror -Wshadow -Wconversion -Wsign-conversion)
    add_test(NAME ${target} COMMAND ${target})
endforeach()

# The result compositor and the invariant it exists for: every byte outside the feathered mask is
# the byte that was there, diffed over the whole image.
target_link_libraries(test_compose PRIVATE noise_ai noise_core noise_dbg)
target_include_directories(test_compose PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(test_compose PRIVATE -Wall -Wextra -Werror -Wshadow -Wconversion -Wsign-conversion)
add_test(NAME test_compose COMMAND test_compose)

# The role table and resolution order: which model a command gets, and what it says when the answer is
# none. The shipped `roles.json` is checked here, so its policy numbers - the ones every region compile
# hands the engine - cannot change without a test noticing.
target_link_libraries(test_roles PRIVATE noise_ai noise_core noise_io noise_dbg)
target_include_directories(test_roles PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(test_roles PRIVATE NOISE_TEST_ASSETS_DIR="${CMAKE_SOURCE_DIR}/assets")
target_compile_options(test_roles PRIVATE -Wall -Wextra -Werror -Wshadow -Wconversion -Wsign-conversion)
add_test(NAME test_roles COMMAND test_roles)

# Recipes are the product's policy: prompts, prep, region scope, composite policy. A malformed one is a
# refusal that names the key, and the shipped set has to agree with the role table it names.
target_link_libraries(test_recipes PRIVATE noise_ai noise_core noise_io noise_dbg)
target_include_directories(test_recipes PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(test_recipes PRIVATE NOISE_TEST_ASSETS_DIR="${CMAKE_SOURCE_DIR}/assets")
target_compile_options(test_recipes PRIVATE -Wall -Wextra -Werror -Wshadow -Wconversion -Wsign-conversion)
add_test(NAME test_recipes COMMAND test_recipes)

# The field policy, asserted on the bytes a request would go out as: the whitelist, the values the
# builder refuses because the server would not, and the cache key's stability.
target_link_libraries(test_provider PRIVATE noise_ai noise_core noise_io noise_dbg)
target_include_directories(test_provider PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(test_provider PRIVATE -Wall -Wextra -Werror -Wshadow -Wconversion -Wsign-conversion)
add_test(NAME test_provider COMMAND test_provider)

# The client, the queue and the cache against recorded HTTP fixtures: request field policy on the wire,
# response parsing, every failure class, replay from cache, durability across a restart. The default
# `ctest` run never opens a socket.
target_link_libraries(test_backend PRIVATE noise_ai noise_core noise_io noise_dbg)
target_include_directories(test_backend PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(test_backend
    PRIVATE NOISE_TEST_FIXTURES_DIR="${CMAKE_SOURCE_DIR}/tests/fixtures/http"
            NOISE_TEST_ASSETS_DIR="${CMAKE_SOURCE_DIR}/assets")
target_compile_options(test_backend PRIVATE -Wall -Wextra -Werror -Wshadow -Wconversion -Wsign-conversion)
add_test(NAME test_backend COMMAND test_backend)
