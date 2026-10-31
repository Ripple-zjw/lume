# Real desktop session regression (run outside the agent sandbox).
# Usage: powershell -ExecutionPolicy Bypass -File tools\regression.ps1
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$exe = Join-Path $root 'build\eTools.exe'
if (-not (Test-Path $exe)) {
    Write-Host 'build\eTools.exe not found. Run build.ps1 first.' -ForegroundColor Red
    exit 1
}

Write-Host 'eTools real-session regression' -ForegroundColor Cyan
Write-Host "exe: $exe"
Write-Host ''

& $exe --self-check
$code = $LASTEXITCODE

Write-Host ''
Write-Host 'Manual checks (not covered by self-check):' -ForegroundColor Yellow
Write-Host '  1. Tray: an eTools icon appears; right-click shows Show / Autostart / Settings / Exit; left-click toggles.'
Write-Host '  2. Hotkey: Alt+Space (or your configured combo) shows/hides the launcher.'
Write-Host '  3. Open: when open=PASS above, the default editor should have opened %TEMP%\etools-selfcheck-opened.txt.'
Write-Host '  4. Settings: --settings opens the dark settings window; save, restart, values persist.'
Write-Host '  5. Autostart: enable it from the tray, then sign out/in and check that eTools starts automatically.'
Write-Host ''
Write-Host "self-check exit=$code"
exit $code
