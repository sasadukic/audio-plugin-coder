[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$FolderPath
)

$ErrorActionPreference = "Stop"

try {
    $resolved = Resolve-Path $FolderPath -ErrorAction Stop
    Start-Process explorer.exe "$resolved"
}
catch {
    Write-Warning ("Could not open folder in Explorer: " + $_.Exception.Message)
}

exit 0
