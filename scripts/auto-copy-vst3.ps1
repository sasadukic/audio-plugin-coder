[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$BuiltBinaryPath,

    [Parameter(Mandatory = $true)]
    [string]$DestinationVst3Dir
)

$ErrorActionPreference = "Stop"

try {
    # Built binary path points to ...\Gnarly.vst3\Contents\x86_64-win\Gnarly.vst3
    # We need the bundle root: ...\Gnarly.vst3
    $binaryDir = Split-Path -Parent $BuiltBinaryPath
    $contentsDir = Split-Path -Parent $binaryDir
    $bundleDir = Split-Path -Parent $contentsDir

    if (-not (Test-Path $bundleDir)) {
        Write-Warning "Auto-copy skipped: built VST3 bundle not found at '$bundleDir'."
        exit 0
    }

    Write-Host "Auto-copying VST3: '$bundleDir' -> '$DestinationVst3Dir'"

    if (Test-Path $DestinationVst3Dir) {
        Remove-Item -Recurse -Force $DestinationVst3Dir
    }

    Copy-Item -Recurse -Force $bundleDir $DestinationVst3Dir
    Write-Host "Auto-copy complete."
    exit 0
}
catch {
    Write-Warning ("Auto-copy failed: " + $_.Exception.Message)
    # Never fail the build because of deployment copy issues.
    exit 0
}
