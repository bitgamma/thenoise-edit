#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
set -euo pipefail

# Share one pinned Playwright installation across projects; never touch REF/node_modules.
TOOLS_DIR="$HOME/.local/share/thenoise-web-tools"
PLAYWRIGHT_VERSION=1.64.1
# Keep downloaded browsers with the writable per-user Playwright installation.
# The existing ~/.cache directory may have been created by root.
export PLAYWRIGHT_BROWSERS_PATH="$TOOLS_DIR/browsers"
mkdir -p "$PLAYWRIGHT_BROWSERS_PATH"

package="$TOOLS_DIR/node_modules/playwright/package.json"
installed=""
if [[ -f "$package" ]]; then
    installed="$(node -p 'require(process.argv[1]).version' "$package" 2>/dev/null || true)"
fi
if [[ "$installed" != "$PLAYWRIGHT_VERSION" ]]; then
    npm install --prefix "$TOOLS_DIR" --no-save --no-audit --no-fund "playwright@$PLAYWRIGHT_VERSION"
fi

# Ensures Chromium *and its headless shell* match this Playwright version.
# Already downloaded browser revisions are reused (cheap on container restarts).
"$TOOLS_DIR/node_modules/.bin/playwright" install chromium
