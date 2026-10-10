#!/bin/bash
# Builds what a GitHub release ships for macOS - a universal Scope Deck.app and
# ScopeTap.ofx.bundle (with the Metal reduction) - checks it, and wraps it in
# ScopeDeck-<version>.pkg.
# The counterpart of release.ps1.
#
#   ./release_mac.sh                      build + check, installer as 0.0.0-dev
#   ./release_mac.sh --version 1.2.3      same, stamped 1.2.3
#   ./release_mac.sh --skip-installer     build + check only
#
# .github/workflows/release.yml runs exactly this on a CI Mac, so a local run
# is a rehearsal of the release. Differences from build_mac.sh: its own build
# dir (build-mac-release), always from scratch, both architectures, only the
# shipping targets, and the conformance and contention gates run before
# anything is packaged.
#
# Needs CMake and Ninja, the Command Line Tools, and (for the installer)
# pkgbuild/productbuild, which every macOS has. No signing identity: the
# package is unsigned, and macOS asks the user to allow it once.

set -euo pipefail

root="$(cd "$(dirname "$0")" && pwd)"
build_dir="$root/build-mac-release"
dist_dir="$root/dist"
version="0.0.0-dev"
skip_installer=0

while [ $# -gt 0 ]; do
    case "$1" in
        --version) shift; version="$1" ;;
        --skip-installer) skip_installer=1 ;;
        *) echo "unknown option: $1" >&2; exit 2 ;;
    esac
    shift
done

# CFBundleVersion and the package version take numbers only, so
# 1.2.3-beta.1 becomes 1.2.3.
numeric="${version%%[-+]*}"
case "$numeric" in
    *[!0-9.]*|'') echo "Version '$version' does not start with a number like 1.2.3." >&2; exit 1 ;;
esac

for tool in cmake ninja lipo codesign; do
    command -v "$tool" > /dev/null || { echo "$tool not found." >&2; exit 1; }
done

# --- build ----------------------------------------------------------------------

# Always from scratch: a release must not inherit a cache - OFX_SDK_DIR, the
# architecture list - from whatever configured this directory last.
rm -rf "$build_dir"

echo "Configuring..."
cmake -S "$root" -B "$build_dir" -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
      -DCMAKE_OSX_ARCHITECTURES="arm64;x86_64" \
      -DSCOPEDECK_VERSION="$version"

echo "Building..."
cmake --build "$build_dir" --target scopedeck ScopeTap scope_conformance scope_publish_contention \
      scope_conformance_metal scope_publish_race_metal

# --- checks ---------------------------------------------------------------------

app="$build_dir/Scope Deck.app"
app_bin="$app/Contents/MacOS/Scope Deck"
bundle="$build_dir/bundle/ScopeTap.ofx.bundle"
plugin="$bundle/Contents/MacOS/ScopeTap.ofx"
for f in "$app_bin" "$plugin" "$app/Contents/Resources/timecode_poll_worker.py" \
         "$app/Contents/Resources/subtitle_poll_worker.py" "$bundle/Contents/Info.plist"; do
    [ -e "$f" ] || { echo "Expected $f but it is not there." >&2; exit 1; }
done

# Both slices, or an Intel Mac gets "cannot be opened" and a Resolve on one
# silently skips the plugin.
for f in "$app_bin" "$plugin"; do
    archs="$(lipo -archs "$f")"
    case "$archs" in
        *arm64*x86_64*|*x86_64*arm64*) echo "$(basename "$f"): $archs" ;;
        *) echo "$f is not universal (got '$archs')." >&2; exit 1 ;;
    esac
done

# The CPU conformance gate: every reduction backend against the reference,
# bit-exactly. Runs the host's own slice.
echo "Running scope_conformance..."
"$build_dir/scope_conformance"

# The publish protocol: several publishers on one scratch block against the
# real reader. A few seconds.
echo "Running scope_publish_contention..."
"$build_dir/scope_publish_contention"

# The Metal gates: the GPU reduction bit-exact against the reference over the
# full matrix including the HD sweep (~30 s), and the real tap driven through
# the real publish hub. A CI Mac has a GPU, so unlike the CUDA gate these run
# on the runner.
echo "Running scope_conformance_metal --hd..."
"$build_dir/scope_conformance_metal" --hd
echo "Running scope_publish_race_metal..."
"$build_dir/scope_publish_race_metal"

# The Resolve workers' failure reasons, which the app shows as sentences.
# Mock objects, so no Resolve and a second or two.
echo "Running resolve_worker_errors_check..."
python3 "$root/tools/resolve_worker_errors_check.py"

# Ad-hoc signed (no identity): seals the bundle's contents so a modified copy
# fails verification, and gives TCC a signature to attach the audio and screen
# permissions to. Not notarized - see the release notes for the one-time
# "Open Anyway" step.
codesign --force --deep --sign - "$app"
codesign --force --deep --sign - "$bundle"

if [ "$skip_installer" = 1 ]; then
    echo "Built and checked (installer skipped)."
    exit 0
fi

# --- installer ------------------------------------------------------------------

for tool in pkgbuild productbuild; do
    command -v "$tool" > /dev/null || { echo "$tool not found." >&2; exit 1; }
done

pkgroot="$build_dir/pkgroot"
rm -rf "$pkgroot"
mkdir -p "$pkgroot/Applications" "$pkgroot/Library/OFX/Plugins"
cp -R "$app" "$pkgroot/Applications/"
cp -R "$bundle" "$pkgroot/Library/OFX/Plugins/"
# Files copied on a Mac carry a provenance attribute; left in, pkgbuild
# records a ._ sidecar for every file in the payload.
xattr -cr "$pkgroot"

# pkgbuild's default is to "relocate" a bundle onto wherever an older copy of
# it lives, which would install over a build directory's Scope Deck.app on a
# developer's Mac instead of into /Applications. Off for both bundles.
components="$build_dir/components.plist"
pkgbuild --analyze --root "$pkgroot" "$components" > /dev/null
i=0
while /usr/libexec/PlistBuddy -c "Print :$i" "$components" > /dev/null 2>&1; do
    /usr/libexec/PlistBuddy -c "Delete :$i:BundleIsRelocatable" "$components" > /dev/null 2>&1 || true
    /usr/libexec/PlistBuddy -c "Add :$i:BundleIsRelocatable bool false" "$components"
    i=$((i + 1))
done

component_pkg="$build_dir/ScopeDeck-component.pkg"
pkgbuild --root "$pkgroot" \
         --component-plist "$components" \
         --identifier com.scopedeck.pkg \
         --version "$numeric" \
         --install-location / \
         --scripts "$root/installer/mac/scripts" \
         "$component_pkg"

distribution="$build_dir/distribution.xml"
sed "s/@VERSION@/$numeric/g" "$root/installer/mac/distribution.xml" > "$distribution"

mkdir -p "$dist_dir"
out="$dist_dir/ScopeDeck-$version.pkg"
productbuild --distribution "$distribution" \
             --package-path "$build_dir" \
             --resources "$root/installer/mac/resources" \
             "$out"

echo "Installer: $out"

# --- manual-install archive -------------------------------------------------------

# The same two bundles the package installs, as a plain zip for anyone who
# would rather drag them into place - README.txt, "Installing by hand", walks
# through it. ditto keeps the bundles' structure and signatures intact.
stage="$build_dir/ScopeDeck-$version-macos"
rm -rf "$stage"
mkdir -p "$stage"
cp -R "$app" "$bundle" "$stage/"
cp "$root/README.txt" "$root/LICENSE" "$stage/"
xattr -cr "$stage"   # no __MACOSX sidecars in the zip, same reason as above
zip_out="$dist_dir/ScopeDeck-$version-macos.zip"
rm -f "$zip_out"
ditto -c -k --keepParent "$stage" "$zip_out"
echo "Archive:   $zip_out"
