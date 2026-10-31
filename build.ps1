#Requires -Version 5.1
<#
.SYNOPSIS
  构建 eTools（免安装 MinGW-w64 工具链）。
.EXAMPLE
  .\build.ps1                 # Release 构建
  .\build.ps1 -Config Debug -Run
  .\build.ps1 -Config Test    # 构建并运行单元测试
  .\build.ps1 -Recompile      # 忽略增量缓存，全量重编
#>
[CmdletBinding()]
param(
  [ValidateSet('Debug', 'Release', 'Test')][string]$Config = 'Release',
  [switch]$Run,
  [switch]$Recompile
)

$ErrorActionPreference = 'Stop'
[Console]::OutputEncoding = [System.Text.Encoding]::UTF8
$Root = Split-Path -Parent $MyInvocation.MyCommand.Definition
$MingwBin = Join-Path $Root '.toolchain\mingw64\bin'
$Gpp = Join-Path $MingwBin 'g++.exe'
$Windres = Join-Path $MingwBin 'windres.exe'
$BuildDir = Join-Path $Root 'build'
$ObjDir = Join-Path $BuildDir ("obj\{0}" -f $Config.ToLowerInvariant())

if (-not (Test-Path -LiteralPath $Gpp)) {
  throw "未找到工具链: $Gpp`n请先执行: node tools/fetch-toolchain.mjs"
}
New-Item -ItemType Directory -Force -Path $BuildDir, $ObjDir | Out-Null

$common = @(
  '-std=c++20', '-municode', '-DUNICODE', '-D_UNICODE', '-DWIN32_LEAN_AND_MEAN', '-DNOMINMAX',
  '-D_WIN32_WINNT=0x0A00', '-DWINVER=0x0A00',
  '-finput-charset=UTF-8', '-fexec-charset=UTF-8',
  '-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter',
  '-I', (Join-Path $Root 'src')
)
switch ($Config) {
  'Debug' { $common += @('-O0', '-g3', '-DDEBUG', '-D_DEBUG') }
  default { $common += @('-O2', '-DNDEBUG', '-s') }
}

$libs = @(
  '-ld2d1', '-ldwrite', '-ld3d11', '-ldxgi', '-ldwmapi', '-ldcomp', '-lwindowscodecs',
  '-lole32', '-luuid', '-loleaut32', '-lshell32', '-lshlwapi', '-lpropsys', '-luxtheme',
  '-lcomctl32', '-lgdi32', '-limm32', '-lshcore', '-lversion'
)
$staticLink = @('-static', '-s')

function Get-Sources([string]$dir) {
  if (-not (Test-Path -LiteralPath $dir)) { return @() }
  return @(Get-ChildItem -LiteralPath $dir -Recurse -Filter *.cpp -File | Sort-Object FullName)
}

function Get-NewestWriteTime([string]$dir) {
  if (-not (Test-Path -LiteralPath $dir)) { return [datetime]::MinValue }
  $files = @(Get-ChildItem -LiteralPath $dir -Recurse -File -Include *.h, *.hpp -ErrorAction SilentlyContinue)
  if ($files.Count -eq 0) { return [datetime]::MinValue }
  return ($files | Measure-Object -Property LastWriteTime -Maximum).Maximum
}

function Invoke-NativeTool {
  param([string]$Tool, [string[]]$Arguments, [string]$Label)
  # PowerShell 5.1 在 ErrorActionPreference=Stop 下会把原生程序写到 stderr 的无害诊断
  # 当成终止性错误；这里临时降级为 Continue，并保留真实退出码。
  $previous = $ErrorActionPreference
  $ErrorActionPreference = 'Continue'
  try {
    $output = & $Tool @Arguments 2>&1
    $code = $LASTEXITCODE
  } finally {
    $ErrorActionPreference = $previous
  }
  if ($code -ne 0) {
    $output | ForEach-Object { Write-Host ("    {0}" -f $_) -ForegroundColor Red }
    throw "$Label 失败 (exit $code)"
  }
  $manifestNote = $false
  foreach ($line in $output) {
    if ("$line" -match 'multiple non-default manifests') { $manifestNote = $true; continue }
    if ("$line" -match 'warning:|note:') {
      Write-Host ("    {0}" -f $line) -ForegroundColor DarkYellow
    }
  }
  if ($manifestNote) {
    Write-Host '    提示: binutils 2.47 的 ".rsrc merge failure" 是非致命诊断，清单已嵌入（已用 permonitorv2 校验）' -ForegroundColor DarkGray
  }
}

function Invoke-BuildTarget {
  param(
    [string]$Name,
    [object[]]$Sources,
    [string]$ExePath,
    [string[]]$ExtraFlags = @()
  )
  if ($Sources.Count -eq 0) { throw "没有找到 $Name 的源文件" }
  $newestHeader = Get-NewestWriteTime (Join-Path $Root 'src')
  $objects = @()
  $compiled = 0
  foreach ($src in $Sources) {
    $rel = $src.FullName.Substring($Root.Length).TrimStart('\', '/')
    $obj = Join-Path $ObjDir (($rel -replace '[\\/]', '_') -replace '\.cpp$', '.o')
    $objects += $obj
    $rebuild = $Recompile -or -not (Test-Path -LiteralPath $obj)
    if (-not $rebuild) {
      $objTime = (Get-Item -LiteralPath $obj).LastWriteTime
      $rebuild = ($src.LastWriteTime -gt $objTime) -or ($newestHeader -gt $objTime)
    }
    if ($rebuild) {
      Write-Host ("  CC  {0}" -f $rel) -ForegroundColor DarkGray
      Invoke-NativeTool -Tool $Gpp -Arguments (@($common) + $ExtraFlags + @('-c', $src.FullName, '-o', $obj)) -Label "编译 $rel"
      $compiled++
    }
  }

  $res = Join-Path $BuildDir 'app.res'
  if ($Name -eq 'eTools') {
    $rc = Join-Path $Root 'assets\app.rc'
    if ((Test-Path -LiteralPath $rc) -and
        ((-not (Test-Path -LiteralPath $res)) -or (Get-Item $rc).LastWriteTime -gt (Get-Item $res).LastWriteTime -or $Recompile)) {
      Write-Host '  RC  assets/app.rc' -ForegroundColor DarkGray
      $rcPath = Join-Path $Root 'assets\app.rc'
      Invoke-NativeTool -Tool $Windres -Arguments @('-I', (Join-Path $Root 'assets'), '-O', 'coff', '-o', $res, $rcPath) -Label '资源编译'
    }
    if (Test-Path -LiteralPath $res) { $objects += $res }
  }

  Write-Host ("  LD  {0}  ({1} 个源文件, 本次编译 {2} 个)" -f $Name, $Sources.Count, $compiled) -ForegroundColor DarkGray
  $sw = [Diagnostics.Stopwatch]::StartNew()
  Invoke-NativeTool -Tool $Gpp -Arguments (@($common) + $objects + @('-o', $ExePath) + $staticLink + $libs + $ExtraFlags) -Label "链接 $Name"
  $sw.Stop()
  $size = [math]::Round((Get-Item -LiteralPath $ExePath).Length / 1KB, 1)
  Write-Host ("构建完成: {0}  ({1} KB, 链接 {2}ms)" -f $ExePath, $size, $sw.ElapsedMilliseconds) -ForegroundColor Green
  return $ExePath
}

if ($Config -eq 'Test') {
  $testSources = @(Get-Sources (Join-Path $Root 'tests')) + @(Get-Sources (Join-Path $Root 'src\core'))
  $exe = Invoke-BuildTarget -Name 'etools_tests' -Sources $testSources -ExePath (Join-Path $BuildDir 'etools_tests.exe')
  & $exe
  exit $LASTEXITCODE
}

$appSources = Get-Sources (Join-Path $Root 'src')
$appExe = Invoke-BuildTarget -Name 'eTools' -Sources $appSources -ExePath (Join-Path $BuildDir 'eTools.exe')
if ($Run) {
  Write-Host ("启动 {0}" -f $appExe) -ForegroundColor Cyan
  Start-Process -FilePath $appExe -WorkingDirectory $BuildDir
}
