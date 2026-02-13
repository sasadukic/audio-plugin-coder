[CmdletBinding()]
param(
    [string]$BuildDir = "build",
    [string]$Config = "Release",
    [string]$Target = "dream_VST3"
)

$ErrorActionPreference = "Stop"

$repoRoot = Resolve-Path (Join-Path $PSScriptRoot "..")
$destinationFolder = "C:\Program Files\Common Files\VST3"

Push-Location $repoRoot
try {
    Write-Host "Configuring CMake (auto-copy OFF, open-after-build OFF)..." -ForegroundColor Cyan
    & cmake -S . -B $BuildDir `
        -DAPC_AUTO_COPY_VST3_TO_SYSTEM_DIR=OFF `
        -DAPC_OPEN_BUILD_FOLDER_AFTER_BUILD=OFF
    if ($LASTEXITCODE -ne 0) {
        throw "CMake configure failed with exit code $LASTEXITCODE."
    }

    Write-Host "Building target '$Target' ($Config)..." -ForegroundColor Cyan
    & cmake --build $BuildDir --config $Config --target $Target
    if ($LASTEXITCODE -ne 0) {
        throw "Build failed with exit code $LASTEXITCODE."
    }

    $sourceBundle = Join-Path $repoRoot "$BuildDir\plugins\dream\dream_artefacts\$Config\VST3\SPECRAUM.vst3"
    if (!(Test-Path $sourceBundle)) {
        throw "Built plugin not found at '$sourceBundle'."
    }

    if (!(Test-Path $destinationFolder)) {
        throw "Destination folder not found at '$destinationFolder'."
    }

    Write-Host "Opening source and destination folders..." -ForegroundColor Green
    Start-Process explorer.exe "/select,`"$sourceBundle`""
    Start-Process explorer.exe "$destinationFolder"
}
finally {
    Pop-Location
}

exit 0
