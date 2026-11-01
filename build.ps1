#Requires -Version 5.1
<#
.SYNOPSIS
  Build Lume with the portable MinGW-w64 toolchain.
.EXAMPLE
  .\build.ps1                          # Release build
  .\build.ps1 -Config Debug -Run
  .\build.ps1 -Config Test             # build and run unit tests
  .\build.ps1 -Recompile               # ignore incremental cache
#>
[CmdletBinding()]
param(
  [ValidateSet('Debug', 'Release', 'Test')][string]$Config = 'Release',
  [switch]$Run,
  [switch]$Recompile,
  # 打发布包：dist\Lume-<Version>-win64\{Lume.exe, Install.cmd, README.txt} + 同名 zip
  [switch]$Package,
  [string]$Version = '0.2.1'
)

$ErrorActionPreference = 'Stop'
[Console]::OutputEncoding = [System.Text.Encoding]::UTF8
$Root = Split-Path -Parent $MyInvocation.MyCommand.Definition
$MingwBin = Join-Path $Root '.toolchain\mingw64\bin'
$Gpp = Join-Path $MingwBin 'g++.exe'
$Gcc = Join-Path $MingwBin 'gcc.exe'
$Windres = Join-Path $MingwBin 'windres.exe'
$BuildDir = Join-Path $Root 'build'
$ObjDir = Join-Path $BuildDir ("obj\{0}" -f $Config.ToLowerInvariant())

if (-not (Test-Path -LiteralPath $Gpp)) {
  throw "Toolchain missing: $Gpp`nRun first: node tools/fetch-toolchain.mjs"
}
New-Item -ItemType Directory -Force -Path $BuildDir, $ObjDir | Out-Null

$common = @(
  '-std=c++20', '-municode', '-DUNICODE', '-D_UNICODE', '-DWIN32_LEAN_AND_MEAN', '-DNOMINMAX',
  '-D_WIN32_WINNT=0x0A00', '-DWINVER=0x0A00', '-D_WIN32_IE=0x0A00',
  '-finput-charset=UTF-8', '-fexec-charset=UTF-8',
  '-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter',
  # Everything.h 默认把 API 声明成 __declspec(dllimport)；SDK 源码是编进 exe 的，
  # 这里把它清空，否则静态链接会出现 __imp_Everything_* 未定义。
  '-DEVERYTHINGUSERAPI=',
  '-I', (Join-Path $Root 'src'),
  '-I', (Join-Path $Root 'third_party\everything\include')
)
switch ($Config) {
  'Debug' { $common += @('-O0', '-g3', '-DDEBUG', '-D_DEBUG') }
  default { $common += @('-O2', '-DNDEBUG', '-s') }
}

# 第三方 C 源码（Everything SDK，见 third_party/everything/README.md）：
# 以 C 语义编译、关掉全部警告、不加 -Werror（源码里有 MSVC 的 #pragma warning 与 stricmp）。
$thirdPartyCommon = @(
  # 不定义 WIN32_LEAN_AND_MEAN：SDK 源码要用 shellapi.h（ShellExecuteEx），而该头被 LEAN_AND_MEAN 排除。
  '-std=c11', '-municode', '-DUNICODE', '-D_UNICODE', '-DNOMINMAX',
  '-D_WIN32_WINNT=0x0A00', '-DWINVER=0x0A00', '-D_WIN32_IE=0x0A00',
  '-w', '-I', (Join-Path $Root 'third_party\everything\include')
)
switch ($Config) {
  'Debug' { $thirdPartyCommon += @('-O0', '-g3', '-DDEBUG', '-D_DEBUG') }
  default { $thirdPartyCommon += @('-O2', '-DNDEBUG') }
}
$thirdPartySources = @(
  (Join-Path $Root 'third_party\everything\src\Everything.c')
)

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
  # PowerShell 5.1 treats stderr output of native tools as a terminating error when
  # ErrorActionPreference is Stop; run with Continue and inspect the real exit code.
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
    throw "$Label failed (exit $code)"
  }
  $manifestNote = $false
  foreach ($line in $output) {
    if ("$line" -match 'multiple non-default manifests') { $manifestNote = $true; continue }
    if ("$line" -match 'warning:|note:') {
      Write-Host ("    {0}" -f $line) -ForegroundColor DarkYellow
    }
  }
  if ($manifestNote) {
    Write-Host '    note: binutils 2.47 ".rsrc merge failure" is a non-fatal diagnostic; manifest is embedded (verified via permonitorv2 string)' -ForegroundColor DarkGray
  }
}

function Invoke-BuildTarget {
  param(
    [string]$Name,
    [object[]]$Sources,
    [string]$ExePath,
    [string[]]$ExtraFlags = @(),
    # 第三方 C 源码：用 gcc + $thirdPartyCommon 编译，产物一并参与链接。
    [object[]]$ThirdParty = @()
  )
  if ($Sources.Count -eq 0) { throw "No sources found for $Name" }
  $newestHeader = Get-NewestWriteTime (Join-Path $Root 'src')
  $thirdPartyHeader = Get-NewestWriteTime (Join-Path $Root 'third_party')
  if ($thirdPartyHeader -gt $newestHeader) { $newestHeader = $thirdPartyHeader }
  $objects = @()
  $compiled = 0
  foreach ($src in $ThirdParty) {
    if (-not (Test-Path -LiteralPath $src)) { throw "Missing third-party source: $src" }
    $rel = $src.Substring($Root.Length).TrimStart('\', '/')
    $obj = Join-Path $ObjDir (($rel -replace '[\\/]', '_') -replace '\.(cpp|c)$', '.o')
    $objects += $obj
    $rebuild = $Recompile -or -not (Test-Path -LiteralPath $obj)
    if (-not $rebuild) {
      $objTime = (Get-Item -LiteralPath $obj).LastWriteTime
      $rebuild = ($src.LastWriteTime -gt $objTime) -or ($newestHeader -gt $objTime)
    }
    if ($rebuild) {
      Write-Host ("  CC  {0}  (third-party)" -f $rel) -ForegroundColor DarkGray
      Invoke-NativeTool -Tool $Gcc -Arguments (@($thirdPartyCommon) + @('-c', $src, '-o', $obj)) -Label "compile $rel"
      $compiled++
    }
  }
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
      Invoke-NativeTool -Tool $Gpp -Arguments (@($common) + $ExtraFlags + @('-c', $src.FullName, '-o', $obj)) -Label "compile $rel"
      $compiled++
    }
  }

  $res = Join-Path $BuildDir 'app.res'
  if ($Name -eq 'Lume') {
    $rc = Join-Path $Root 'assets\app.rc'
    if ((Test-Path -LiteralPath $rc) -and
        ((-not (Test-Path -LiteralPath $res)) -or (Get-Item $rc).LastWriteTime -gt (Get-Item $res).LastWriteTime -or $Recompile)) {
      Write-Host '  RC  assets/app.rc' -ForegroundColor DarkGray
      $rcPath = Join-Path $Root 'assets\app.rc'
      Invoke-NativeTool -Tool $Windres -Arguments @('-I', (Join-Path $Root 'assets'), '-O', 'coff', '-o', $res, $rcPath) -Label 'windres'
    }
    if (Test-Path -LiteralPath $res) { $objects += $res }
  }

  Write-Host ("  LD  {0}  ({1} sources, {2} compiled)" -f $Name, $Sources.Count, $compiled) -ForegroundColor DarkGray
  $sw = [Diagnostics.Stopwatch]::StartNew()
  $subsystem = if ($Name -eq 'Lume') { @('-mwindows') } else { @() }
  Invoke-NativeTool -Tool $Gpp -Arguments (@($common) + $objects + @('-o', $ExePath) + $staticLink + $subsystem + $libs + $ExtraFlags) -Label "link $Name"
  $sw.Stop()
  $size = [math]::Round((Get-Item -LiteralPath $ExePath).Length / 1KB, 1)
  Write-Host ("Built: {0}  ({1} KB, link {2}ms)" -f $ExePath, $size, $sw.ElapsedMilliseconds) -ForegroundColor Green
  return $ExePath
}

if ($Config -eq 'Test') {
  $testSources = @(Get-Sources (Join-Path $Root 'tests')) + @(Get-Sources (Join-Path $Root 'src\core'))
  $exe = Invoke-BuildTarget -Name 'etools_tests' -Sources $testSources -ExePath (Join-Path $BuildDir 'etools_tests.exe')
  & $exe
  exit $LASTEXITCODE
}

$appSources = Get-Sources (Join-Path $Root 'src')
$appExe = Invoke-BuildTarget -Name 'Lume' -Sources $appSources -ExePath (Join-Path $BuildDir 'Lume.exe') -ThirdParty $thirdPartySources
if ($Run) {
  Write-Host ("Launching {0}" -f $appExe) -ForegroundColor Cyan
  Start-Process -FilePath $appExe -WorkingDirectory $BuildDir
}

if ($Package) {
  if ($Config -ne 'Release') { throw '-Package 需要 Release 构建' }
  # exe 版本资源必须与包版本一致，避免把旧构建打进去
  $exeVersion = (Get-Item -LiteralPath $appExe).VersionInfo.ProductVersion
  if ($exeVersion -notlike "$Version*") {
    throw "Lume.exe 版本 '$exeVersion' 与 -Version '$Version' 不一致（检查 assets/app.rc 并 -Recompile）"
  }
  $readme = Join-Path $Root 'packaging\README.txt'
  $install = Join-Path $Root 'packaging\Install.cmd'
  foreach ($required in @($readme, $install)) {
    if (-not (Test-Path -LiteralPath $required)) { throw "发布包缺少文件: $required" }
  }

  $dist = Join-Path $Root 'dist'
  $stageName = "Lume-$Version-win64"
  $stage = Join-Path $dist $stageName
  if (Test-Path -LiteralPath $stage) { Remove-Item -LiteralPath $stage -Recurse -Force }
  New-Item -ItemType Directory -Force -Path $stage | Out-Null
  Copy-Item -LiteralPath $appExe -Destination (Join-Path $stage 'Lume.exe')
  Copy-Item -LiteralPath $install -Destination $stage
  Copy-Item -LiteralPath $readme -Destination $stage

  $zip = Join-Path $dist "$stageName.zip"
  if (Test-Path -LiteralPath $zip) { Remove-Item -LiteralPath $zip -Force }
  Compress-Archive -Path (Join-Path $stage '*') -DestinationPath $zip -CompressionLevel Optimal

  $zipSize = [math]::Round((Get-Item -LiteralPath $zip).Length / 1KB, 1)
  $sha = (Get-FileHash -LiteralPath $zip -Algorithm SHA256).Hash
  Write-Host ("Packaged: {0}  ({1} KB, Lume.exe {2})" -f $zip, $zipSize, $exeVersion) -ForegroundColor Green
  Write-Host ("SHA256:   {0}" -f $sha)
}
