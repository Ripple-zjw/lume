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
        foreach ($line in $lines) {
            if ($line -notmatch '^(\S+)\s+\S+\s+\d+\s+([\d.]+)\s+([\d.]+)\s+([\d.]+)\s+([\d.]+)$') { continue }
            $query = $Matches[1]
            if ($seen.ContainsKey($query)) { continue }
            $seen[$query] = $true
            $measured[$query] = [double]$Matches[3]
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
            $ok = $med -le $check.Limit
            $state = if ($ok) { 'PASS' } else { 'FAIL' }
            $text = "bench_{0}={1} med={2:N2}ms limit={3}ms" -f $check.Label, $state, $med, $check.Limit
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
Write-Host 'Manual checks (not covered by self-check):' -ForegroundColor Yellow
Write-Host '  1. Tray: a Lume icon appears; right-click shows Show / Autostart / Settings / Exit; left-click toggles.'
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
Write-Host ''
$status = "self-check exit=$code"
if ($benchFailures -gt 0) {
    $status += " bench_failures=$benchFailures"
}
Write-Host $status
if ($benchFailures -gt 0 -and $code -eq 0) { $code = 2 }
exit $code
