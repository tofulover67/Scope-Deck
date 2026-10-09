# Deploys the OFX bundles into Resolve's plugin folder and then VERIFIES the
# copies by hash.
#
#   .\deploy.ps1            deploy whichever bundles are built, then verify
#   .\deploy.ps1 -VerifyOnly   compare deployed against built, copy nothing
#
# Why this exists rather than build.ps1 -Deploy alone: that handles the real tap
# only, and neither script previously answered the question that actually
# matters afterwards - "is what Resolve will load the thing I just built?".
# MSVC stamps a timestamp into every PE header, so a rebuild from identical
# source produces different bytes; without a hash check, "deployed is stale" and
# "deployed was merely relinked" look the same and get settled by memory. They
# should be settled by a comparison.
#
# Resolve scans OFX plugins only at startup, and a running Resolve holds the
# loaded .ofx open - a Resolve sitting on the Project Manager still counts, so
# this checks for the process rather than trusting that the window is gone.

[CmdletBinding()]
param(
    [switch]$VerifyOnly
)

$ErrorActionPreference = 'Stop'

$root = $PSScriptRoot
$ofx  = 'C:\Program Files\Common Files\OFX\Plugins'

# name -> built bundle path. The spike is optional: it only exists when a CUDA
# toolchain was present, and its absence is normal rather than an error.
# ScopeTap is built in BOTH trees and they are not the same plugin: build-cuda
# compiles the CUDA reduction in (SCOPE_TAP_CUDA), build does not. The CUDA one
# is what ships - deploying the other silently gives you a tap with no GPU path
# and nothing anywhere to explain why. Prefer build-cuda, fall back to build.
$tapCuda  = Join-Path $root 'build-cuda\bundle\ScopeTap.ofx.bundle'
$tapPlain = Join-Path $root 'build\bundle\ScopeTap.ofx.bundle'
if (Test-Path (Join-Path $tapCuda 'Contents\Win64\ScopeTap.ofx')) {
    $tapBuilt = $tapCuda
    Write-Host 'ScopeTap: using the CUDA build (build-cuda)' -ForegroundColor DarkGray
} else {
    $tapBuilt = $tapPlain
    Write-Host 'ScopeTap: using the CPU-only build - no CUDA toolchain was present' -ForegroundColor DarkYellow
}

$bundles = @(
    @{ Name = 'ScopeTap';           Built = $tapBuilt }
    @{ Name = 'ScopeTapGpuSpike';   Built = Join-Path $root 'build-cuda\bundle\ScopeTapGpuSpike.ofx.bundle' }
)

function Get-BinaryPath([string]$BundleRoot, [string]$Name) {
    Join-Path $BundleRoot "Contents\Win64\$Name.ofx"
}

$present = @()
foreach ($b in $bundles) {
    $bin = Get-BinaryPath $b.Built $b.Name
    if (Test-Path $bin) { $present += $b }
    else { Write-Host "skip   $($b.Name) - not built" -ForegroundColor DarkGray }
}
if ($present.Count -eq 0) { throw "Nothing to deploy - no built bundles found." }

if (-not $VerifyOnly) {
    $proc = Get-Process -Name 'Resolve' -ErrorAction SilentlyContinue
    if ($proc) {
        throw "DaVinci Resolve is running (pid $($proc.Id)). Quit it fully - returning to the Project Manager is not enough, it still holds the loaded .ofx open."
    }

    # The copy needs admin; everything else deliberately does not, so the
    # verification below runs as the normal user against what actually landed.
    $lines = @("`$ErrorActionPreference = 'Stop'")
    foreach ($b in $present) {
        $dst = Join-Path $ofx "$($b.Name).ofx.bundle"
        $lines += "if (Test-Path '$dst') { Remove-Item -Recurse -Force '$dst' }"
        $lines += "Copy-Item -Recurse -Force '$($b.Built)' '$ofx'"
    }
    $encoded = [Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes(($lines -join "`n")))

    Write-Host "Deploying $($present.Count) bundle(s) to $ofx (admin required)..." -ForegroundColor Cyan
    $elevated = Start-Process powershell.exe -Verb RunAs -Wait -PassThru `
        -ArgumentList '-NoProfile', '-EncodedCommand', $encoded
    if ($elevated.ExitCode -ne 0) { throw "Elevated copy failed with exit code $($elevated.ExitCode)." }
}

Write-Host "`nVerifying deployed bytes against built bytes:" -ForegroundColor Cyan
$bad = 0
foreach ($b in $present) {
    $builtBin    = Get-BinaryPath $b.Built $b.Name
    $deployedBin = Get-BinaryPath (Join-Path $ofx "$($b.Name).ofx.bundle") $b.Name

    if (-not (Test-Path $deployedBin)) {
        Write-Host ("  {0,-20} NOT DEPLOYED" -f $b.Name) -ForegroundColor Red
        $bad++
        continue
    }

    $h1 = (Get-FileHash $builtBin    -Algorithm SHA256).Hash
    $h2 = (Get-FileHash $deployedBin -Algorithm SHA256).Hash

    if ($h1 -eq $h2) {
        Write-Host ("  {0,-20} MATCH   {1}" -f $b.Name, $h1.Substring(0,16)) -ForegroundColor Green
    } else {
        Write-Host ("  {0,-20} STALE   built {1} / deployed {2}" -f $b.Name, $h1.Substring(0,16), $h2.Substring(0,16)) -ForegroundColor Red
        $bad++
    }
}

if ($bad -gt 0) {
    Write-Host "`n$bad bundle(s) do not match. Re-run without -VerifyOnly." -ForegroundColor Red
    exit 1
}

Write-Host "`nAll deployed bundles match their build output. Restart Resolve to load them." -ForegroundColor Green
