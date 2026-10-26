#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
set -euo pipefail

# Browser setup must not be skipped if an existing native CMake cache is stale.
bash .devcontainer/playwright.sh

# The native test target remains the default editor for clangd and CTest.
cmake -S . -B build-devcontainer -G Ninja -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
if [[ ! -e compile_commands.json && ! -L compile_commands.json ]]; then
    ln -s build-devcontainer/compile_commands.json compile_commands.json
fi

echo "Native and browser tools ready: emcc, emcmake, node, Playwright Chromium"
