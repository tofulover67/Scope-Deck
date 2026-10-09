# Builds what a GitHub release ships - scopedeck.exe, the CUDA ScopeTap and the
# Premiere plugin ScopeTransmit - checks it, and wraps it in the installer.
#
#   .\release.ps1                     build + check, installer as 0.0.0-dev
#   .\release.ps1 -Version 1.2.3      same, stamped 1.2.3
#   .\release.ps1 -SkipInstaller      build + check only (no Inno Setup needed)
#
# .github/workflows/release.yml runs exactly this on a tag push, so a local run
# is a rehearsal of the release. Differences from build.ps1: its own build dir
# (build-release), CUDA required rather than optional, only the shipping
# targets, and the CPU conformance gate run before anything is packaged.
#
# Needs: Visual Studio's C++ tools, CUDA 12.9 (CUDA_PATH or -CudaRoot),
# internet access (it downloads the Python it bundles), and Inno Setup 6
# unless -SkipInstaller.

[CmdletBinding()]
param(
    [string]$Version = '0.0.0-dev',
    [string]$CudaRoot = $env:CUDA_PATH,
    # CUDA 12.9's nvcc refuses any MSVC newer than VS 2022 (toolset 14.4x), so
    # the toolset is pinned rather than left to whatever vcvars defaults to - on
    # a machine with VS 2026 Build Tools that default is 14.5x and nvcc stops.
    [string]$VcToolset = '14.44',
    [switch]$SkipInstaller,
    # With the Premiere SDK present: also copy the fresh ScopeTransmit.prm into
    # prebuilt\ and record its sources' fingerprint - then commit prebuilt\.
    [switch]$UpdatePrebuiltTransmit
)

$ErrorActionPreference = 'Stop'

# The Python the installer ships, for the timecode and subtitle workers, so a
# tester needs nothing but the installer. The embeddable build: stdlib only,
# which is all the workers import (plus Resolve's own scripting module, which
# they load from Resolve's install). 3.14.7 because Resolve's scripting is
# confirmed working with it. The hash is python.org's own, from the .spdx.json
# published beside the zip - change both together.
$pythonVersion = '3.14.7'
$pythonSha256  = 'D297E5FF019966817AD8502465176139F2D3D840FA4ED84B13BED399A6AB1F15'

$root     = $PSScriptRoot
$buildDir = Join-Path $root 'build-release'
$distDir  = Join-Path $root 'dist'

# --- toolchain ----------------------------------------------------------------

$vswhere = 'C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path $vswhere)) { throw "vswhere not found - install Visual Studio's 'Desktop development with C++'." }
$vsPath = & $vswhere -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath | Select-Object -First 1
if (-not $vsPath) { throw 'No MSVC C++ toolchain found.' }
$vcvars = Join-Path $vsPath 'VC\Auxiliary\Build\vcvars64.bat'

# Pull the vcvars environment into this session, so cmake, cl and ninja are
# all on PATH for every step below rather than one cmd /c line at a time.
$vcEnv = cmd /c "`"$vcvars`" -vcvars_ver=$VcToolset >nul 2>&1 && set"
if ($LASTEXITCODE -ne 0) { throw "vcvars64 failed - is MSVC toolset $VcToolset installed? (Visual Studio Installer, 'MSVC v143 - VS 2022 C++ x64/x86 build tools')" }
$vcEnv | ForEach-Object {
    if ($_ -match '^([^=]+)=(.*)$') { [Environment]::SetEnvironmentVariable($Matches[1], $Matches[2]) }
}
foreach ($tool in 'cl', 'cmake', 'ninja') {
    if (-not (Get-Command $tool -ErrorAction SilentlyContinue)) { throw "$tool not on PATH after vcvars64." }
}

# CUDA is not optional here. Without it CMake quietly builds a CPU-only
# ScopeTap, which is a working plugin that simply never uses the GPU - exactly
# the thing that must not reach a release by accident.
if (-not $CudaRoot) { throw 'CUDA not found - set CUDA_PATH or pass -CudaRoot.' }
$nvcc = Join-Path $CudaRoot 'bin\nvcc.exe'
if (-not (Test-Path $nvcc)) { throw "nvcc not found at $nvcc" }

# --- build ----------------------------------------------------------------------

# Always from scratch: a release must not inherit a cache - OFX_SDK_DIR, the
# CUDA compiler - from whatever configured this directory last.
if (Test-Path $buildDir) { Remove-Item -Recurse -Force $buildDir }

Write-Host 'Configuring...' -ForegroundColor Cyan
# Every -D quoted: Windows PowerShell splits an unquoted -DFOO=3.5 at the dot.
cmake -S $root -B $buildDir -G Ninja '-DCMAKE_BUILD_TYPE=Release' `
      '-DCMAKE_POLICY_VERSION_MINIMUM=3.5' "-DCMAKE_CUDA_COMPILER=$nvcc"
if ($LASTEXITCODE -ne 0) { throw 'CMake configure failed.' }

Write-Host 'Building...' -ForegroundColor Cyan
cmake --build $buildDir --target scopedeck ScopeTap scope_conformance scope_publish_contention
if ($LASTEXITCODE -ne 0) { throw 'Build failed.' }

# --- bundled Python ---------------------------------------------------------------

# Into build-release\python, the folder the bridges look for beside
# scopedeck.exe - so running build-release\scopedeck.exe uses it too.
$pythonZip = Join-Path $buildDir "python-$pythonVersion-embed-amd64.zip"
$pythonDir = Join-Path $buildDir 'python'
Write-Host "Fetching Python $pythonVersion (embeddable)..." -ForegroundColor Cyan
Invoke-WebRequest "https://www.python.org/ftp/python/$pythonVersion/python-$pythonVersion-embed-amd64.zip" `
    -OutFile $pythonZip -UseBasicParsing
$actual = (Get-FileHash $pythonZip -Algorithm SHA256).Hash
if ($actual -ne $pythonSha256) { throw "Python download does not match its pinned SHA-256 (got $actual)." }
Expand-Archive $pythonZip -DestinationPath $pythonDir
Remove-Item $pythonZip

$pythonReport = & (Join-Path $pythonDir 'python.exe') --version
if ($pythonReport -ne "Python $pythonVersion") { throw "Bundled Python reports '$pythonReport', expected Python $pythonVersion." }
Write-Host "Bundled $pythonReport" -ForegroundColor Green

# --- Premiere plugin ------------------------------------------------------------

# ScopeTransmit needs Adobe's Premiere Pro SDK, which is never committed - its
# headers are marked Adobe Confidential - so a GitHub runner cannot compile
# it. The repo carries a compiled copy in prebuilt\ instead, beside a
# fingerprint of the sources it was built from, and a release refuses to
# package that copy once those sources have changed. A compiled plugin is
# what the SDK exists to let us distribute; only the SDK itself stays out.
#
# With the SDK here, the plugin is built fresh, and -UpdatePrebuiltTransmit
# refreshes the committed copy from that build.
$transmitSources = 'plugin/ScopeTransmit.cpp', 'core/ScopeControl.cpp', 'core/ScopeControl.h',
                   'core/ScopeCore.cpp', 'core/ScopeCore.h', 'core/ScopeShm.cpp', 'core/ScopeShm.h',
                   'core/ScopeTypes.h'

# Names and bytes of every source, in order. Byte-exact on every machine:
# .gitattributes turns off line-ending conversion for the whole tree.
function Get-TransmitFingerprint {
    $sha = [Security.Cryptography.SHA256]::Create()
    foreach ($rel in $transmitSources) {
        $name  = [Text.Encoding]::UTF8.GetBytes("$rel`n")
        $bytes = [IO.File]::ReadAllBytes((Join-Path $root $rel))
        [void]$sha.TransformBlock($name, 0, $name.Length, $null, 0)
        [void]$sha.TransformBlock($bytes, 0, $bytes.Length, $null, 0)
    }
    [void]$sha.TransformFinalBlock([byte[]]@(), 0, 0)
    -join ($sha.Hash | ForEach-Object { $_.ToString('x2') })
}

$prebuiltDir  = Join-Path $root 'prebuilt'
$prebuiltPrm  = Join-Path $prebuiltDir 'ScopeTransmit.prm'
$prebuiltHash = Join-Path $prebuiltDir 'ScopeTransmit.sources.sha256'
$builtPrm     = Join-Path $buildDir 'ScopeTransmit.prm'
$fingerprint  = Get-TransmitFingerprint
$recorded     = if (Test-Path $prebuiltHash) { (Get-Content $prebuiltHash -Raw).Trim() } else { '' }
$sdkHeader    = Join-Path $root 'adobe premiere sdk\Windows\Premiere Pro 26.0 C++ SDK\Examples\Headers\PrSDKTransmit.h'

if (Test-Path $sdkHeader) {
    Write-Host 'Building ScopeTransmit (Premiere SDK present)...' -ForegroundColor Cyan
    cmake --build $buildDir --target ScopeTransmit
    if ($LASTEXITCODE -ne 0) { throw 'ScopeTransmit build failed.' }

    if ($UpdatePrebuiltTransmit) {
        New-Item -ItemType Directory -Force $prebuiltDir | Out-Null
        Copy-Item $builtPrm $prebuiltPrm -Force
        Set-Content -Path $prebuiltHash -Value $fingerprint -NoNewline -Encoding ascii
        Write-Host 'prebuilt\ScopeTransmit.prm updated - commit prebuilt\ with the source change.' -ForegroundColor Green
    } elseif ($recorded -ne $fingerprint) {
        Write-Host 'WARNING: prebuilt\ScopeTransmit.prm is out of date with these sources - the GitHub release will refuse to build until you run .\release.ps1 -UpdatePrebuiltTransmit and commit prebuilt\.' -ForegroundColor Yellow
    }
} else {
    if (-not (Test-Path $prebuiltPrm) -or -not $recorded) {
        throw 'No Premiere SDK here and no prebuilt\ScopeTransmit.prm to package.'
    }
    if ($recorded -ne $fingerprint) {
        throw 'prebuilt\ScopeTransmit.prm was built from different sources than these. On a machine with the Premiere SDK, run .\release.ps1 -UpdatePrebuiltTransmit and commit prebuilt\.'
    }
    Copy-Item $prebuiltPrm $builtPrm -Force
    Write-Host 'Using prebuilt\ScopeTransmit.prm (its sources match these).' -ForegroundColor Green
}

# The one export Premiere loads a Transmit plugin by.
$prmText = [Text.Encoding]::ASCII.GetString([IO.File]::ReadAllBytes($builtPrm))
if ($prmText -notmatch 'xTransmitEntry') { throw 'ScopeTransmit.prm does not export xTransmitEntry.' }

# --- checks ---------------------------------------------------------------------

$exe    = Join-Path $buildDir 'scopedeck.exe'
$plugin = Join-Path $buildDir 'bundle\ScopeTap.ofx.bundle\Contents\Win64\ScopeTap.ofx'
foreach ($f in $exe, $plugin,
               (Join-Path $buildDir 'timecode_poll_worker.py'),
               (Join-Path $buildDir 'subtitle_poll_worker.py')) {
    if (-not (Test-Path $f)) { throw "Expected $f but it is not there." }
}

# The static CUDA runtime loads nvcuda.dll by name at first use, so the name is
# in the binary exactly when the CUDA path was compiled in.
$pluginText = [Text.Encoding]::ASCII.GetString([IO.File]::ReadAllBytes($plugin))
if ($pluginText -notmatch 'nvcuda\.dll') { throw 'ScopeTap.ofx was built without the CUDA path.' }
Write-Host 'ScopeTap.ofx has the CUDA path compiled in.' -ForegroundColor Green

# The CPU conformance gate. The CUDA one (scope_conformance_cuda) needs an
# NVIDIA card, which a GitHub runner does not have - run it on the 5070 Ti box.
Write-Host 'Running scope_conformance...' -ForegroundColor Cyan
& (Join-Path $buildDir 'scope_conformance.exe')
if ($LASTEXITCODE -ne 0) { throw 'scope_conformance failed - not packaging this build.' }

# The publish protocol: several publishers on one scratch block against the
# real reader. CPU only, so the runner can run it; a few seconds.
Write-Host 'Running scope_publish_contention...' -ForegroundColor Cyan
& (Join-Path $buildDir 'scope_publish_contention.exe')
if ($LASTEXITCODE -ne 0) { throw 'scope_publish_contention failed - not packaging this build.' }

if ($SkipInstaller) { return }

# --- installer ------------------------------------------------------------------

# The machine-wide install (a GitHub runner, choco) or the per-user one (the
# Inno Setup installer's "only for me" option).
$iscc = @(
    (Join-Path ${env:ProgramFiles(x86)} 'Inno Setup 6\ISCC.exe'),
    (Join-Path $env:LOCALAPPDATA 'Programs\Inno Setup 6\ISCC.exe')
) | Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $iscc) { throw 'Inno Setup 6 not found (or pass -SkipInstaller).' }

# Windows version resources take numbers only, so 1.2.3-beta.1 becomes 1.2.3.
$numeric = ($Version -split '[-+]')[0]
if ($numeric -notmatch '^\d+(\.\d+){0,3}$') { throw "Version '$Version' does not start with a number like 1.2.3." }

& $iscc "/DAppVersion=$Version" "/DNumericVersion=$numeric" `
        "/DBuildDir=$buildDir" "/DOutputDir=$distDir" (Join-Path $root 'installer\ScopeDeck.iss')
if ($LASTEXITCODE -ne 0) { throw 'Inno Setup failed.' }

Write-Host "Installer: $(Join-Path $distDir "ScopeDeck-Setup-$Version.exe")" -ForegroundColor Green
