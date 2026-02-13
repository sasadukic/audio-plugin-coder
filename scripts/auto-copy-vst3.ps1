[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$BuiltBinaryPath,

    [Parameter(Mandatory = $true)]
    [string]$DestinationVst3Dir,

    [int]$Retries = 3,

    [int]$RetryDelayMs = 750
)

$ErrorActionPreference = "Stop"

function Fail([int]$code, [string]$message) {
    [Console]::Error.WriteLine($message)
    exit $code
}

function Get-BundlePathFromBuiltBinary([string]$builtBinaryPath) {
    $binaryDir = Split-Path -Parent $builtBinaryPath
    $contentsDir = Split-Path -Parent $binaryDir
    return (Split-Path -Parent $contentsDir)
}

function Get-BinaryPathInBundle([string]$bundleDir, [string]$binaryName) {
    return (Join-Path $bundleDir ("Contents\x86_64-win\" + $binaryName))
}

function Copy-BundleStrict([string]$sourceBundleDir, [string]$destinationBundleDir, [int]$maxRetries, [int]$delayMs) {
    $attempt = 0
    while ($true) {
        $attempt++
        try {
            if (Test-Path -LiteralPath $destinationBundleDir) {
                Remove-Item -LiteralPath $destinationBundleDir -Recurse -Force
            }

            Copy-Item -LiteralPath $sourceBundleDir -Destination $destinationBundleDir -Recurse -Force
            return
        }
        catch {
            if ($attempt -ge $maxRetries) {
                throw
            }
            Start-Sleep -Milliseconds $delayMs
        }
    }
}

$bundleDir = Get-BundlePathFromBuiltBinary $BuiltBinaryPath
if (-not (Test-Path -LiteralPath $bundleDir)) {
    Fail 2 "Auto-copy failed: built VST3 bundle not found at '$bundleDir'."
}

$builtBinaryName = Split-Path -Leaf $BuiltBinaryPath
$sourceBinaryPath = Get-BinaryPathInBundle $bundleDir $builtBinaryName
if (-not (Test-Path -LiteralPath $sourceBinaryPath)) {
    Fail 3 "Auto-copy failed: built VST3 binary not found at '$sourceBinaryPath'."
}

try {
    Write-Host "Auto-copying VST3: '$bundleDir' -> '$DestinationVst3Dir'"
    Copy-BundleStrict $bundleDir $DestinationVst3Dir $Retries $RetryDelayMs
}
catch {
    Fail 4 ("Auto-copy failed: " + $_.Exception.Message)
}

$destinationBinaryPath = Get-BinaryPathInBundle $DestinationVst3Dir $builtBinaryName
if (-not (Test-Path -LiteralPath $destinationBinaryPath)) {
    Fail 5 "Auto-copy failed: destination VST3 binary missing at '$destinationBinaryPath'."
}

$sourceHash = (Get-FileHash -LiteralPath $sourceBinaryPath -Algorithm SHA256).Hash
$destinationHash = (Get-FileHash -LiteralPath $destinationBinaryPath -Algorithm SHA256).Hash
if ($sourceHash -ne $destinationHash) {
    Fail 6 "Auto-copy failed: hash mismatch after copy. source=$sourceHash destination=$destinationHash"
}

Write-Host "Auto-copy complete. Hash verified."
exit 0
