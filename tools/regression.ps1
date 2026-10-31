# 真实桌面会话回归（不要在 agent 沙箱里跑）
# 用法：powershell -ExecutionPolicy Bypass -File tools\regression.ps1
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$exe = Join-Path $root 'build\eTools.exe'
if (-not (Test-Path $exe)) {
    Write-Host '找不到 build\eTools.exe，请先运行 build.ps1' -ForegroundColor Red
    exit 1
}

Write-Host 'eTools 真实会话回归' -ForegroundColor Cyan
Write-Host "exe: $exe"
Write-Host ''

& $exe --self-check
$code = $LASTEXITCODE

Write-Host ''
Write-Host '人工确认项（self-check 无法覆盖）：' -ForegroundColor Yellow
Write-Host '  1. 托盘：应出现 eTools 图标；右键有“显示 / 开机自启 / 设置… / 退出”，左键切换窗口。'
Write-Host '  2. 热键：Alt+Space（或你设置的热键）应唤出/隐藏窗口。'
Write-Host '  3. 文件打开：上面 open=PASS 时，应已用默认程序打开 %TEMP%\etools-selfcheck-opened.txt。'
Write-Host '  4. 设置：--settings 打开设置窗；保存后重启 eTools，主题/强调色/热键/目录仍生效。'
Write-Host '  5. 自启：托盘勾选“开机自启”后，注销/重启应自动启动（或检查 HKCU\...\Run）。'
Write-Host ''
Write-Host "self-check exit=$code"
exit $code
