#!/bin/bash
# Deploys ScopeTap.ofx.bundle into Resolve's plugin folder and then VERIFIES
# the copy by hash - the counterpart of deploy.ps1.
#
#   ./deploy_mac.sh                deploy build-mac's bundle, then verify
#   ./deploy_mac.sh --verify-only  compare deployed against built, copy nothing
#   ./deploy_mac.sh --from DIR     use another build dir (e.g. build-mac-release)
#
# /Library/OFX/Plugins is owned by root, so the copy runs through sudo and
# asks for your password; the verification afterwards runs as you, against
# what actually landed. Resolve scans OFX plugins only at startup and holds
# the loaded .ofx open, so it must be fully quit first - a Resolve sitting on
# the Project Manager still counts.

set -euo pipefail

root="$(cd "$(dirname "$0")" && pwd)"
build_dir="$root/build-mac"
ofx_dir="/Library/OFX/Plugins"
verify_only=0

while [ $# -gt 0 ]; do
    case "$1" in
        --verify-only) verify_only=1 ;;
        --from) shift; build_dir="$1" ;;
        *) echo "unknown option: $1" >&2; exit 2 ;;
    esac
    shift
done

built="$build_dir/bundle/ScopeTap.ofx.bundle"
built_bin="$built/Contents/MacOS/ScopeTap.ofx"
deployed="$ofx_dir/ScopeTap.ofx.bundle"
deployed_bin="$deployed/Contents/MacOS/ScopeTap.ofx"

[ -f "$built_bin" ] || { echo "Nothing to deploy - $built_bin is not built." >&2; exit 1; }

if [ "$verify_only" = 0 ]; then
    if pgrep -x Resolve > /dev/null; then
        echo "DaVinci Resolve is running. Quit it fully - returning to the Project Manager is not enough, it still holds the loaded .ofx open." >&2
        exit 1
    fi
    echo "Deploying to $deployed (your password is for the copy into /Library)..."
    sudo mkdir -p "$ofx_dir"
    sudo rm -rf "$deployed"
    sudo cp -R "$built" "$deployed"
fi

echo
echo "Verifying deployed bytes against built bytes:"
if [ ! -f "$deployed_bin" ]; then
    printf '  %-20s NOT DEPLOYED\n' ScopeTap
    exit 1
fi
h1="$(shasum -a 256 "$built_bin" | cut -c1-64)"
h2="$(shasum -a 256 "$deployed_bin" | cut -c1-64)"
if [ "$h1" = "$h2" ]; then
    printf '  %-20s MATCH   %s\n' ScopeTap "${h1:0:16}"
else
    printf '  %-20s STALE   built %s / deployed %s\n' ScopeTap "${h1:0:16}" "${h2:0:16}"
    echo
    echo "Deployed bundle does not match. Re-run without --verify-only."
    exit 1
fi
echo
echo "Deployed bundle matches its build output. Start Resolve to load it, then look for 'Scope Tap' under the Scope Deck group in the OpenFX panel."
