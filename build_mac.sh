#!/bin/bash
# Builds the Scope Deck app and the ScopeTap OFX plugin on macOS - the
# counterpart of build.ps1.
#
#   ./build_mac.sh            configure + build (build-mac/)
#   ./build_mac.sh --clean    wipe the build dir first
#   ./build_mac.sh --run      build, then launch Scope Deck.app
#   ./build_mac.sh --deploy   also copy ScopeTap.ofx.bundle into Resolve's
#                             plugin folder (asks for your password: the
#                             folder is root-owned)
#
# Needs CMake and Ninja (brew install cmake ninja) and the Command Line Tools
# (xcode-select --install); no Xcode. ScopeTap builds against the OpenFX SDK
# vendored in vendor/openfx, so no Resolve install is needed to build it.
# For the universal build a GitHub release ships, see release_mac.sh.
#
# Resolve scans OFX plugins only at startup, so it must be quit before
# --deploy and started again afterwards.

set -euo pipefail

root="$(cd "$(dirname "$0")" && pwd)"
build_dir="$root/build-mac"
bundle_src="$build_dir/bundle/ScopeTap.ofx.bundle"
ofx_dir="/Library/OFX/Plugins"

clean=0; run=0; deploy=0
for arg in "$@"; do
    case "$arg" in
        --clean)  clean=1 ;;
        --run)    run=1 ;;
        --deploy) deploy=1 ;;
        *) echo "unknown option: $arg" >&2; exit 2 ;;
    esac
done

for tool in cmake ninja; do
    command -v "$tool" > /dev/null || { echo "$tool not found - brew install cmake ninja" >&2; exit 1; }
done

if [ "$clean" = 1 ] && [ -d "$build_dir" ]; then
    echo "Cleaning $build_dir"
    rm -rf "$build_dir"
fi

# CMAKE_POLICY_VERSION_MINIMUM: GLFW 3.4 still declares a 3.4 minimum, which
# CMake 4 rejects. See vendor/VENDOR.md.
echo "Configuring..."
cmake -S "$root" -B "$build_dir" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_POLICY_VERSION_MINIMUM=3.5

echo "Building..."
cmake --build "$build_dir"

app="$build_dir/Scope Deck.app"
[ -x "$app/Contents/MacOS/Scope Deck" ] || { echo "Expected $app but it is not there." >&2; exit 1; }
echo "Built $app"

plugin="$bundle_src/Contents/MacOS/ScopeTap.ofx"
if [ -f "$plugin" ]; then
    echo "Built $plugin"
else
    echo "ScopeTap.ofx not built - OFX SDK not found (see CMake's warning above). The app is unaffected."
fi

if [ "$run" = 1 ]; then
    echo "Launching..."
    open "$app"
fi

if [ "$deploy" = 0 ]; then
    [ -f "$plugin" ] && echo "Plugin not deployed. Re-run with --deploy to install into $ofx_dir"
    exit 0
fi

[ -f "$plugin" ] || { echo "Cannot deploy: ScopeTap.ofx was not built." >&2; exit 1; }
exec "$root/deploy_mac.sh"
