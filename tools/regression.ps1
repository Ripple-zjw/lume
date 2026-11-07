# Lume real desktop session regression (run outside the agent sandbox).
# Usage: powershell -ExecutionPolicy Bypass -File tools\regression.ps1
#        ... -SkipBench                      # skip the in-memory search benchmark
#        ... -BenchMs 60 -BenchShortMs 200   # override the med latency limits (ms)
#        ... -BenchEntries 200000            # smaller synthetic data set
[CmdletBinding()]
param(
    [switch]$SkipBench,
    [int]$BenchEntries = 1000000,
    [int]$BenchIters = 5,
    # med latency limits (ms) for 1M synthetic entries, measured by --bench-search.
    # report = normal 3+ char query, a = single char (worst case: almost every entry matches).
    [double]$BenchMs = 60,
    [double]$BenchShortMs = 200
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$exe = Join-Path $root 'build\Lume.exe'
if (-not (Test-Path $exe)) {
    Write-Host 'build\Lume.exe not found. Run build.ps1 first.' -ForegroundColor Red
    exit 1
}

Write-Host 'Lume real-session regression' -ForegroundColor Cyan
Write-Host "exe: $exe"
Write-Host ''

# PowerShell 不会等待 GUI 子系统的程序，所以 `& $exe ...` 之后 $LASTEXITCODE 可能是空的、
# 也可能与随后启动的进程抢跑。统一改成 cmd 重定向到文件：退出码可靠，且保证已跑完。
$selfLog = Join-Path $env:TEMP 'lume-selfcheck.txt'
if (Test-Path -LiteralPath $selfLog) { Remove-Item -LiteralPath $selfLog -Force }
cmd /c "`"$exe`" --self-check > `"$selfLog`" 2>&1"
$code = $LASTEXITCODE
if (Test-Path -LiteralPath $selfLog) {
    foreach ($line in @(Get-Content -LiteralPath $selfLog -Encoding UTF8 -ErrorAction SilentlyContinue)) {
        if ($line -match '=FAIL') { Write-Host $line -ForegroundColor Red }
        elseif ($line -match '=PASS') { Write-Host $line -ForegroundColor Green }
        else { Write-Host $line }
    }
    Remove-Item -LiteralPath $selfLog -Force -ErrorAction SilentlyContinue
} else {
    Write-Host 'self-check=FAIL (no output)' -ForegroundColor Red
    $code = 2
}

# Search benchmark guard: synthesized in-memory entries, real FileIndex::Search.
# Catches scan/score/filter regressions without needing a snapshot on disk.
$benchFailures = 0
if ($SkipBench) {
    Write-Host ''
    Write-Host 'Search benchmark: skipped (-SkipBench)' -ForegroundColor Yellow
} else {
    Write-Host ''
    Write-Host ("Search benchmark: {0} synthetic entries, {1} iters per case" -f $BenchEntries, $BenchIters) -ForegroundColor Cyan
    $benchLog = Join-Path $env:TEMP 'lume-bench.txt'
    if (Test-Path -LiteralPath $benchLog) { Remove-Item -LiteralPath $benchLog -Force }
    # GUI-subsystem exe: PowerShell cannot capture its stdout, so redirect via cmd.
    cmd /c "`"$exe`" --bench-search=$BenchEntries --bench-iters=$BenchIters > `"$benchLog`" 2>&1"
    $benchCode = $LASTEXITCODE
    $lines = @()
    if (Test-Path -LiteralPath $benchLog) {
        $lines = @(Get-Content -LiteralPath $benchLog -Encoding UTF8 -ErrorAction SilentlyContinue)
    }
    if ($lines.Count -eq 0) {
        Write-Host 'bench=FAIL (no output)' -ForegroundColor Red
        $benchFailures++
    } else {
        if (-not ($lines -match 'filter_check=ok')) {
            Write-Host 'bench_filter_check=FAIL' -ForegroundColor Red
            $benchFailures++
        }
        # Row format: query  filter  hits  min_ms  med_ms  p95_ms  scan_M_per_s
        # Rows for one query come in order (all / apps / images); the first row is
        # the unfiltered baseline, which is what the latency limits apply to.
        $seen = @{}
        $measured = @{}
        $measuredP95 = @{}
        foreach ($line in $lines) {
            if ($line -notmatch '^(\S+)\s+\S+\s+\d+\s+([\d.]+)\s+([\d.]+)\s+([\d.]+)\s+([\d.]+)$') { continue }
            $query = $Matches[1]
            if ($seen.ContainsKey($query)) { continue }
            $seen[$query] = $true
            $measured[$query] = [double]$Matches[3]
            $measuredP95[$query] = [double]$Matches[4]
        }
        $checks = @(
            @{ Query = 'report'; Limit = $BenchMs; Label = 'report' },
            @{ Query = 'a'; Limit = $BenchShortMs; Label = 'a_1char' }
        )
        foreach ($check in $checks) {
            if (-not $measured.ContainsKey($check.Query)) {
                Write-Host ("bench_{0}=FAIL (row missing)" -f $check.Label) -ForegroundColor Red
                $benchFailures++
                continue
            }
            $med = $measured[$check.Query]
            $p95 = $measuredP95[$check.Query]
            # p95 允许 3 倍余量，只拦长尾数量级退化（偶发调度抖动不算）。
            $p95Limit = $check.Limit * 3
            $ok = ($med -le $check.Limit) -and ($p95 -le $p95Limit)
            $state = if ($ok) { 'PASS' } else { 'FAIL' }
            $text = "bench_{0}={1} med={2:N2}ms p95={3:N2}ms limits={4}/{5}ms" -f $check.Label, $state, $med, $p95, $check.Limit, $p95Limit
            if ($ok) {
                Write-Host $text -ForegroundColor Green
            } else {
                Write-Host $text -ForegroundColor Red
                $benchFailures++
            }
        }
        if ($benchCode -ne 0) {
            Write-Host ("bench_exit=FAIL ({0})" -f $benchCode) -ForegroundColor Red
            $benchFailures++
        } else {
            Write-Host 'bench_exit=0'
        }
    }
    if (Test-Path -LiteralPath $benchLog) { Remove-Item -LiteralPath $benchLog -Force -ErrorAction SilentlyContinue }
}

Write-Host ''
Write-Host 'Log checks:' -ForegroundColor Cyan
$logInfo = Join-Path $env:TEMP 'lume-log-info.log'
$logDebug = Join-Path $env:TEMP 'lume-log-debug.log'
$logError = Join-Path $env:TEMP 'lume-log-error.log'
foreach ($path in @($logInfo, $logDebug, $logError)) {
    if (Test-Path -LiteralPath $path) { Remove-Item -LiteralPath $path -Force }
}
cmd /c "`"$exe`" --ms=700 --log=`"$logInfo`" > NUL 2>&1"
cmd /c "`"$exe`" --ms=700 --debug --log=`"$logDebug`" > NUL 2>&1"
cmd /c "`"$exe`" --ms=700 --log-level=error --log=`"$logError`" > NUL 2>&1"
$infoLines = @(Get-Content -LiteralPath $logInfo -Encoding UTF8 -ErrorAction SilentlyContinue)
$debugLines = @(Get-Content -LiteralPath $logDebug -Encoding UTF8 -ErrorAction SilentlyContinue)
$errorLines = @(Get-Content -LiteralPath $logError -Encoding UTF8 -ErrorAction SilentlyContinue)
$logFailures = 0
$logChecks = @(
    @{ Label = 'log_banner'; Ok = ($infoLines.Count -gt 0 -and $infoLines[0] -match '^\[.+\]\[\*\] ===== Lume ') },
    @{ Label = 'log_info'; Ok = [bool]($infoLines -match '\]\[I\] ') },
    @{ Label = 'log_no_debug_by_default'; Ok = -not [bool]($infoLines -match '\]\[D\] ') },
    @{ Label = 'log_debug'; Ok = [bool]($debugLines -match '\]\[D\] ') },
    @{ Label = 'log_error_only'; Ok = ($errorLines.Count -gt 0 -and -not [bool]($errorLines -match '\]\[[IDW]\] ')) }
)
foreach ($check in $logChecks) {
    if ($check.Ok) {
        Write-Host ("{0}=PASS" -f $check.Label) -ForegroundColor Green
    } else {
        Write-Host ("{0}=FAIL" -f $check.Label) -ForegroundColor Red
        $logFailures++
    }
}
foreach ($path in @($logInfo, $logDebug, $logError)) {
    if (Test-Path -LiteralPath $path) { Remove-Item -LiteralPath $path -Force -ErrorAction SilentlyContinue }
}
if ($logFailures -gt 0 -and $code -eq 0) { $code = 2 }

# Windows 设置页目录：协议可用性、条目数量，以及「搜索能命中设置页」。
Write-Host ''
Write-Host 'Settings pages:' -ForegroundColor Cyan
$settingsLog = Join-Path $env:TEMP 'lume-settings-list.txt'
$settingsRowsLog = Join-Path $env:TEMP 'lume-settings-rows.txt'
foreach ($path in @($settingsLog, $settingsRowsLog)) {
    if (Test-Path -LiteralPath $path) { Remove-Item -LiteralPath $path -Force }
}
cmd /c "`"$exe`" --list-settings > `"$settingsLog`" 2>&1"
$listCode = $LASTEXITCODE
cmd /c "`"$exe`" --filter=1 --search-rows=startup > `"$settingsRowsLog`" 2>&1"
$listLines = @(Get-Content -LiteralPath $settingsLog -Encoding UTF8 -ErrorAction SilentlyContinue)
$rowLines = @(Get-Content -LiteralPath $settingsRowsLog -Encoding UTF8 -ErrorAction SilentlyContinue)
$pageLines = @($listLines -match '^[^\t]+\tms-settings:')
$settingsFailures = 0
$settingsChecks = @(
    @{ Label = 'settings_list'; Ok = ($listCode -eq 0 -and [bool]($listLines -match '^available=1 pages=\d+$') -and $pageLines.Count -ge 50) },
    @{ Label = 'settings_rows'; Ok = [bool]($rowLines -match 'action=settings ') }
)
foreach ($check in $settingsChecks) {
    if ($check.Ok) {
        Write-Host ("{0}=PASS (pages={1})" -f $check.Label, $pageLines.Count) -ForegroundColor Green
    } else {
        Write-Host ("{0}=FAIL" -f $check.Label) -ForegroundColor Red
        $settingsFailures++
    }
}
foreach ($path in @($settingsLog, $settingsRowsLog)) {
    if (Test-Path -LiteralPath $path) { Remove-Item -LiteralPath $path -Force -ErrorAction SilentlyContinue }
}
if ($settingsFailures -gt 0 -and $code -eq 0) { $code = 2 }

Write-Host ''
Write-Host 'Manual checks (not covered by self-check):' -ForegroundColor Yellow
Write-Host '  1. Tray: a Lume icon appears; right-click shows Show / Autostart / Settings / Exit;'
Write-Host '     left-click opens the launcher and it stays on screen (no flash-and-hide).'
Write-Host '     `Lume.exe --tray-click --ms=1500` replays the two v4 notifications in-process.'
Write-Host '  2. Hotkey: Alt+Space (or your configured combo) shows/hides the launcher.'
Write-Host '  3. Open: when open=PASS above, the default editor should have opened %TEMP%\lume-selfcheck-opened.txt.'
Write-Host '  4. Settings: --settings opens the dark settings window; save, restart, values persist.'
Write-Host '  5. Autostart: enable it from the tray, then sign out/in and check that Lume starts automatically.'
Write-Host '  6. Everything: with Everything running, "everything=PASS" appears above (SKIP = not running/installed,'
Write-Host '     which is fine). `Lume.exe --everything-check` prints available=1, version and hits>0; switching'
Write-Host '     设置 → 索引 → 文件搜索来源 to Everything stops the local index, deletes files.idx and returns'
Write-Host '     results from all drives; switching back starts a background rebuild.'
Write-Host '  7. Filters: "filters=PASS" above means the default groups pass validation and encode/decode round'
Write-Host '     trips. In the launcher, Tab / Shift+Tab cycle the chips, Ctrl+1..9 jump to one, clicking also'
Write-Host '     works; 设置 → 筛选 can add/rename/reorder groups and 恢复默认 restores the six defaults.'
Write-Host '  8. Benchmark: bench_report / bench_a_1char above are med latencies over the synthetic'
Write-Host '     entries; FAIL means search got noticeably slower (raise -BenchMs/-BenchShortMs'
Write-Host '     only for slow hosts, or pass -SkipBench to skip).'
Write-Host '  9. Mouse: hovering a result row highlights it (hand cursor); clicking launches it'
Write-Host '     (Shift+click = reveal, Ctrl+click = elevated). `Lume.exe --click-row=2 --query=notepad`'
Write-Host '     runs the same path with synthetic mouse messages; the log shows 悬停 / 点击 / 激活 lines.'
Write-Host ' 10. Log: Lume.log sits next to Lume.exe (fallback %LOCALAPPDATA%\Lume\logs when that folder is'
Write-Host '     read-only); each run starts with a "===== Lume ... =====" banner line. Normal use logs'
Write-Host '     info only; tick 设置 → 常规 → 详细日志（开发排查用）or pass --debug to get per-keystroke'
Write-Host '     and Everything round-trip lines. Over 2 MB the file rotates to Lume.log.1 on next start.'
Write-Host '     `Lume.exe --log-level=error` keeps only failures; --log=<path> writes elsewhere.'
Write-Host ' 11. Settings pages: `Lume.exe --open=ms-settings:startupapps` should open 设置 → 应用 → 启动,'
Write-Host '     not the Settings home page. Try ms-settings:appsfeatures / bluetooth / windowsupdate too.'
Write-Host '     In the launcher, searching 开机自启 / 卸载 / 蓝牙 shows a 设置 · <分类> row; Enter opens that'
Write-Host '     page, Shift/Ctrl+Enter just open it (no reveal, no elevation).'
Write-Host ''
$status = "self-check exit=$code"
if ($benchFailures -gt 0) {
    $status += " bench_failures=$benchFailures"
}
Write-Host $status
if ($benchFailures -gt 0 -and $code -eq 0) { $code = 2 }
exit $code
