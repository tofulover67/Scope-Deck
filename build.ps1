# Builds the native Scope Deck app and the ScopeTap OFX plugin.
#
#   .\build.ps1            configure + build
#   .\build.ps1 -Clean     wipe the build dir first
#   .\build.ps1 -Run       build, then launch scopedeck.exe
#   .\build.ps1 -Deploy    also copy ScopeTap.ofx.bundle into Resolve's plugin
#                          folder (prompts for admin)
#
# The plugin (plugin/, and core/ScopeCore.{h,cpp}, core/ScopeShm.{h,cpp} - the
# write side of the wire format core/ScopeReader.{h,cpp} reads) was migrated in
# from "Scope Deck (old)" so this tree is self-sufficient - that old tree is not
# a build dependency of this one for anything, and can eventually go away.
# ScopeTap needs Resolve's OpenFX SDK (OFX_SDK_DIR, defaulted in CMakeLists.txt
# to the copy vendored in vendor/openfx) to build; if that SDK is not found,
# CMake warns and skips just that target - scopedeck.exe still builds fine
# without it. For the build a GitHub release ships, see release.ps1.
#
# Resolve scans OFX plugins only at startup, so it must be closed before -Deploy
# and restarted afterwards.

[CmdletBinding()]
param(
    [switch]$Clean,
    [switch]$Run,
    [switch]$Deploy
)

$ErrorActionPreference = 'Stop'

$root      = $PSScriptRoot
$buildDir  = Join-Path $root 'build'
$bundleSrc = Join-Path $buildDir 'bundle\ScopeTap.ofx.bundle'
$ofxDir    = 'C:\Program Files\Common Files\OFX\Plugins'

# --- locate the MSVC toolchain -------------------------------------------

$vswhere = 'C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path $vswhere)) {
    throw "vswhere not found. Install Visual Studio Build Tools with the 'Desktop development with C++' workload."
}

$vsPath = & $vswhere -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath | Select-Object -First 1
if (-not $vsPath) { throw "No MSVC C++ toolchain found." }

$vcvars = Join-Path $vsPath 'VC\Auxiliary\Build\vcvars64.bat'
if (-not (Test-Path $vcvars)) { throw "vcvars64.bat not found at $vcvars" }

# --- build ----------------------------------------------------------------

if ($Clean -and (Test-Path $buildDir)) {
    Write-Host "Cleaning $buildDir" -ForegroundColor DarkGray
    Remove-Item -Recurse -Force $buildDir
}

# CMAKE_POLICY_VERSION_MINIMUM: GLFW 3.4 still declares a 3.4 minimum, which
# CMake 4 rejects. See vendor/VENDOR.md - it is GLFW's declaration that is stale,
# not anything here.
Write-Host "Configuring..." -ForegroundColor Cyan
cmd /c "`"$vcvars`" >nul 2>&1 && cmake -S `"$root`" -B `"$buildDir`" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_POLICY_VERSION_MINIMUM=3.5"
if ($LASTEXITCODE -ne 0) { throw "CMake configure failed." }

Write-Host "Building..." -ForegroundColor Cyan
cmd /c "`"$vcvars`" >nul 2>&1 && cmake --build `"$buildDir`""
if ($LASTEXITCODE -ne 0) { throw "Build failed." }

$exe = Join-Path $buildDir 'scopedeck.exe'
if (-not (Test-Path $exe)) { throw "Expected $exe but it is not there." }

Write-Host "Built $exe ($((Get-Item $exe).Length) bytes)" -ForegroundColor Green

$pluginBinary = Join-Path $bundleSrc 'Contents\Win64\ScopeTap.ofx'
if (Test-Path $pluginBinary) {
    Write-Host "Built $pluginBinary ($((Get-Item $pluginBinary).Length) bytes)" -ForegroundColor Green
} else {
    Write-Host "ScopeTap.ofx not built - OFX SDK not found (see CMake's warning above). scopedeck.exe is unaffected." -ForegroundColor DarkYellow
}

if ($Run) {
    # Start-Process, not '& $exe': the app is a GUI program and invoking it
    # directly would hold this terminal until it is closed.
    Write-Host "Launching..." -ForegroundColor Cyan
    Start-Process -FilePath $exe | Out-Null
}

# --- deploy the plugin ------------------------------------------------------

if (-not $Deploy) {
    if (Test-Path $pluginBinary) {
        Write-Host "`nPlugin not deployed. Re-run with -Deploy to install into $ofxDir" -ForegroundColor DarkGray
    }
    return
}

if (-not (Test-Path $pluginBinary)) {
    throw "Cannot deploy: ScopeTap.ofx was not built (OFX SDK not found)."
}

if (Get-Process -Name 'Resolve' -ErrorAction SilentlyContinue) {
    throw "DaVinci Resolve is running. Close it first - OFX plugins are scanned only at startup."
}

Write-Host "Deploying to $ofxDir (admin required)..." -ForegroundColor Cyan

$deployScript = @"
`$ErrorActionPreference = 'Stop'
`$dst = Join-Path '$ofxDir' 'ScopeTap.ofx.bundle'
if (Test-Path `$dst) { Remove-Item -Recurse -Force `$dst }
Copy-Item -Recurse -Force '$bundleSrc' '$ofxDir'
"@

$encoded = [Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes($deployScript))
$proc = Start-Process powershell.exe -Verb RunAs -Wait -PassThru `
    -ArgumentList '-NoProfile', '-EncodedCommand', $encoded

if ($proc.ExitCode -ne 0) { throw "Elevated copy failed with exit code $($proc.ExitCode)." }

$installed = Join-Path $ofxDir 'ScopeTap.ofx.bundle\Contents\Win64\ScopeTap.ofx'
if (Test-Path $installed) {
    Write-Host "Installed: $installed" -ForegroundColor Green
    Write-Host "Start Resolve, then look for 'Scope Tap' under the Scope Deck group in the OpenFX panel." -ForegroundColor Green
} else {
    throw "Deploy reported success but $installed is missing."
}
