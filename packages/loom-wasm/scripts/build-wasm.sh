#!/usr/bin/env bash
# Build the wasm host + demo modules and stage them into this package's
# wasm/ dir, ready for `npm pack`/`npm publish`.
#
# Local, scripted, non-CI for v1 -- no workflow in this repo builds
# Emscripten yet (that's separate follow-on work), so this is a developer's
# manual publish step, run from a machine with the Emscripten toolchain set
# up (see `just setup-wasm`).
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PKG_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
REPO_ROOT="$(cd "$PKG_DIR/../.." && pwd)"

# --- version lockstep check --------------------------------------------------
# The package's version tracks LOOM_SDK_VERSION (see the root VERSION file,
# consumed by sdk/conanfile.py) in lockstep -- npm install @joshpolansky/loom-wasm@X
# unambiguously means "wasm host built against SDK version X". No separate
# compat-check layer: runtime/src/module_loader.cpp already refuses to load a
# module built against a different SDK version, so a drift here would just
# make the resulting package silently useless for whatever it's paired with.
sdk_version="$(cat "$REPO_ROOT/VERSION" | tr -d '[:space:]')"
pkg_version="$(node -p "require('$PKG_DIR/package.json').version")"
if [ "$sdk_version" != "$pkg_version" ]; then
    echo "error: package.json version ($pkg_version) != root VERSION ($sdk_version)." >&2
    echo "       Bump packages/loom-wasm/package.json to match before building." >&2
    exit 1
fi

# --- build --------------------------------------------------------------
echo "==> just build-wasm (root: $REPO_ROOT)"
( cd "$REPO_ROOT" && just build-wasm )

# --- stage into wasm/ -----------------------------------------------------
STAGE="$PKG_DIR/wasm"
mkdir -p "$STAGE" "$STAGE/demo-modules"

echo "==> staging loom_wasm.js/.wasm"
cp "$REPO_ROOT/frontend/public/loom_wasm.js" "$STAGE/loom_wasm.js"
cp "$REPO_ROOT/frontend/public/loom_wasm.wasm" "$STAGE/loom_wasm.wasm"

# Same list as frontend/src/wasm/boot.ts's WASM_MODULES -- the bundled demo
# module set. Optional quick-start assets for a consumer with no modules of
# their own yet; a real consumer (e.g. Actuate) points moduleUrl at their own
# build output instead and never touches these.
DEMO_MODULES=(
    class_based.so
    command_probe.so
    crasher.so
    example_motor.so
    oscilloscope.so
    pneumatic_actuator.so
    sequencer.so
    stack_light.so
)
echo "==> staging ${#DEMO_MODULES[@]} demo modules"
for m in "${DEMO_MODULES[@]}"; do
    cp "$REPO_ROOT/frontend/public/$m" "$STAGE/demo-modules/$m"
done

echo "==> done: $STAGE"
