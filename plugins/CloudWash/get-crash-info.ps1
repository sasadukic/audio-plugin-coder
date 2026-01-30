# Get crash information from Event Viewer
$appErrors = Get-WinEvent -LogName Application -MaxEvents 50 | Where-Object {$_.ProviderName -eq 'Application Error'}

foreach ($evt in $appErrors) {
    if ($evt.Message -like '*CloudWash*') {
        Write-Host "==================== CRASH FOUND ====================" -ForegroundColor Red
        Write-Host "Time: $($evt.TimeCreated)"
        Write-Host "Message:"
        Write-Host $evt.Message
        Write-Host "====================================================" -ForegroundColor Red
        break
    }
}

if (-not $appErrors) {
    Write-Host "No application errors found in Event Log" -ForegroundColor Yellow
}
