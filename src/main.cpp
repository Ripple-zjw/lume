#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <io.h>
#include <string>

#include "app/app.h"
#include "app/settings.h"
#include "app/version.h"
#include "core/log.h"
#include "launch/launcher.h"
#include "platform/autostart.h"
#include "platform/hotkey.h"
#include "platform/system.h"
#include "platform/tray.h"
#include "search/app_entry.h"
#include "search/everything_index.h"
#include "search/file_index.h"
#include "search/filter.h"
#include "search/merge.h"
#include "ui/theme.h"

#include <psapi.h>

namespace {

double PeakWorkingSetMb() {
  PROCESS_MEMORY_COUNTERS counters{};
  if (K32GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters))) {
    return static_cast<double>(counters.PeakWorkingSetSize) / (1024.0 * 1024.0);
  }
  return 0.0;
}

struct CliOptions {
  AppOptions app;
  std::wstring shot;
  std::wstring shotMatrix;
  float scale = 0.0f;
  bool listApps = false;
  bool listFiles = false;
  bool fileStats = false;
  bool selfCheck = false;
  std::wstring openPath;
  std::wstring setAutostart;
  std::wstring searchFiles;
  std::wstring searchRows;
  std::wstring searchLive;
  size_t indexSizeMB = 0;
  bool everythingCheck = false;
  std::wstring everythingQuery = L"notepad";
  std::wstring filterSpec;
  bool listFilters = false;
  bool benchSearch = false;
  size_t benchEntries = 1000000;
  int benchIters = 5;
};

void PrintUsage() {
  printf("%ls %ls\n", kAppName, kAppVersion);
  fputs(
      "\n"
      "Usage:\n"
      "  Lume.exe                       Run in tray-less resident mode (hotkey toggles)\n"
      "  Lume.exe --show                Start visible (auto-hide on focus loss)\n"
      "  Lume.exe --ms=1500             Auto exit after N milliseconds\n"
      "  Lume.exe --capture=out.png     Show, capture composited window, exit\n"
      "  Lume.exe --shot=out.png        Offscreen render with simulated wallpaper\n"
      "  Lume.exe --shot-matrix=dir     Render 3 DPIs x 2 themes baselines into dir\n"
      "\n"
      "Options:\n"
      "  --theme=dark|light|system        Theme (default: system)\n"
      "  --accent=system|#RRGGBB          Accent color (default: design blue #6EA8FF)\n"
      "  --log=<path>                     Write log to this file instead of <exe dir>\\Lume.log\n"
      "  --log-level=error|warn|info|debug  Log verbosity (default: info; debug = dev details)\n"
      "  --debug                          Shorthand for --log-level=debug\n"
      "  --query=<text>                   Prefill query text\n"
      "  --scale=<factor>                 Screenshot scale (default 1.0)\n"
      "  --glass                          Lower panel opacity for backdrop inspection\n"
      "  --type=<text>                    Inject characters after showing (input test)\n"
      "  --keys=up,down,enter             Inject key sequence after showing\n"
      "  --hover-row=<n>                  Inject a mouse move over row n (1-based)\n"
      "  --click-row=<n>                  Click row n with a synthetic mouse message\n"
      "  --tray-click                     Simulate a tray left-click (two v4 notifications)\n"
      "  --compose=<text>                 Inject IME composition string (render test)\n"
      "  --capture-ms=<n>                 Capture delay in ms (default 450, use 30 for fade)\n"
      "  --list-apps                      Print the collected app index and exit\n"
      "  --list-files                     Rebuild the file index, print stats and sample paths\n"
      "  --file-stats                     Rebuild the file index and print stats only\n"
      "  --index-size-mb=<n>              Override max index file size in MB (64–262144)\n"
      "  --index-dir=<dir>                Override the index snapshot directory\n"
      "  --search-files=<query>           Search the file index and print top results with scores\n"
      "  --search-rows=<query>            Print merged app/file rows with tier and score\n"
      "  --file-search=local|everything   File search source (default: local index)\n"
      "  --filter=<name|index|all|off>    Select a filter group chip (1 = all)\n"
      "  --list-filters                   Print the filter groups and exit\n"
      "  --bench-search[=<n>]             Synthesize N in-memory entries and time real searches\n"
      "  --bench-iters=<n>                Iterations per bench case (default 5)\n"
      "  --everything-check[=<query>]     Probe Everything, run one query, print hits and merged rows\n"
      "  --search-live=<query>            Rebuild index now and search it (diagnostics)\n"
      "  --open=<path>                    Verify opening a file/path directly\n"
      "  --set-autostart=1|0              Enable/disable autostart and print state\n"
      "  --hotkey=<combo>                 Override hotkey for this run (e.g. Ctrl+Alt+K)\n"
      "  --settings                       Open the settings window at startup\n"
      "  --settings-probe=id[,id...]      Open settings, post WM_COMMAND for each control id\n"
      "  --version                        Print version and exit\n"
      "  --clear-index                    Delete snapshots and rebuild at startup\n"
      "  --self-check                     Check hotkey/tray/autostart/file-open and print PASS/FAIL\n"
      "  --file-root=<dir>                Override file index roots (testing)\n"
      "  --no-hotkey                      Do not register global hotkey\n",
      stdout);
}

bool StartsWith(const std::wstring& text, const wchar_t* prefix) {
  return text.rfind(prefix, 0) == 0;
}

std::wstring ValueAfter(const std::wstring& arg, const wchar_t* prefix) {
  return arg.substr(wcslen(prefix));
}

CliOptions Parse(int argc, wchar_t** argv) {
  CliOptions cli;
  for (int i = 1; i < argc; ++i) {
    const std::wstring arg = argv[i];
    if (StartsWith(arg, L"--shot=")) {
      cli.shot = ValueAfter(arg, L"--shot=");
    } else if (StartsWith(arg, L"--shot-matrix=")) {
      cli.shotMatrix = ValueAfter(arg, L"--shot-matrix=");
    } else if (StartsWith(arg, L"--capture=")) {
      cli.app.capturePath = ValueAfter(arg, L"--capture=");
    } else if (StartsWith(arg, L"--query=")) {
      cli.app.query = ValueAfter(arg, L"--query=");
    } else if (StartsWith(arg, L"--theme=")) {
      cli.app.themeMode = ParseThemeMode(ValueAfter(arg, L"--theme="));
      cli.app.themeSpecified = true;
    } else if (StartsWith(arg, L"--accent=")) {
      cli.app.accent = ValueAfter(arg, L"--accent=");
      cli.app.accentSpecified = true;
    } else if (StartsWith(arg, L"--hotkey=")) {
      cli.app.hotkeyOverride = ValueAfter(arg, L"--hotkey=");
    } else if (arg == L"--autostart") {
      cli.app.autostartLaunch = true;
    } else if (StartsWith(arg, L"--log=")) {
      cli.app.logPath = ValueAfter(arg, L"--log=");
    } else if (StartsWith(arg, L"--log-level=")) {
      LogLevel level = LogLevel::Info;
      const std::wstring value = ValueAfter(arg, L"--log-level=");
      if (ParseLogLevel(value, &level)) {
        cli.app.logLevel = level;
        cli.app.logLevelSpecified = true;
      } else {
        wprintf(L"--log-level=%ls 无法识别（可选 error/warn/info/debug），沿用 info\n", value.c_str());
      }
    } else if (arg == L"--debug") {
      cli.app.logLevel = LogLevel::Debug;
      cli.app.logLevelSpecified = true;
    } else if (StartsWith(arg, L"--file-root=")) {
      cli.app.fileRoot = ValueAfter(arg, L"--file-root=");
    } else if (StartsWith(arg, L"--open=")) {
      cli.openPath = ValueAfter(arg, L"--open=");
    } else if (StartsWith(arg, L"--set-autostart=")) {
      cli.setAutostart = ValueAfter(arg, L"--set-autostart=");
    } else if (arg == L"--settings") {
      cli.app.openSettings = true;
    } else if (arg == L"--clear-index") {
      cli.app.clearIndexOnStart = true;
    } else if (StartsWith(arg, L"--test-capture=")) {
      cli.app.testCapture = ValueAfter(arg, L"--test-capture=");
      cli.app.openSettings = true;
    } else if (StartsWith(arg, L"--settings-probe=")) {
      cli.app.settingsProbe = ValueAfter(arg, L"--settings-probe=");
      cli.app.openSettings = true;
    } else if (StartsWith(arg, L"--search-files=")) {
      cli.searchFiles = ValueAfter(arg, L"--search-files=");
    } else if (StartsWith(arg, L"--search-rows=")) {
      cli.searchRows = ValueAfter(arg, L"--search-rows=");
    } else if (arg == L"--everything-check") {
      cli.everythingCheck = true;
    } else if (StartsWith(arg, L"--everything-check=")) {
      cli.everythingCheck = true;
      cli.everythingQuery = ValueAfter(arg, L"--everything-check=");
    } else if (StartsWith(arg, L"--file-search=")) {
      cli.app.fileSearch = ValueAfter(arg, L"--file-search=");
      cli.app.fileSearchSpecified = true;
    } else if (StartsWith(arg, L"--filter=")) {
      cli.filterSpec = ValueAfter(arg, L"--filter=");
      cli.app.filterSpec = cli.filterSpec;
      cli.app.filterSpecified = true;
    } else if (arg == L"--list-filters") {
      cli.listFilters = true;
    } else if (arg == L"--bench-search") {
      cli.benchSearch = true;
    } else if (StartsWith(arg, L"--bench-search=")) {
      cli.benchSearch = true;
      cli.benchEntries = static_cast<size_t>(
          _wcstoui64(ValueAfter(arg, L"--bench-search=").c_str(), nullptr, 10));
    } else if (StartsWith(arg, L"--bench-iters=")) {
      cli.benchIters = static_cast<int>(
          wcstol(ValueAfter(arg, L"--bench-iters=").c_str(), nullptr, 10));
    } else if (StartsWith(arg, L"--search-live=")) {
      cli.searchLive = ValueAfter(arg, L"--search-live=");
    } else if (StartsWith(arg, L"--index-size-mb=")) {
      cli.indexSizeMB = static_cast<size_t>(_wcstoui64(
          ValueAfter(arg, L"--index-size-mb=").c_str(), nullptr, 10));
      cli.app.indexSizeMB = cli.indexSizeMB;
    } else if (StartsWith(arg, L"--index-dir=")) {
      cli.app.indexDir = ValueAfter(arg, L"--index-dir=");
    } else if (arg == L"--self-check") {
      cli.selfCheck = true;
    } else if (StartsWith(arg, L"--scale=")) {
      cli.scale = static_cast<float>(wcstod(ValueAfter(arg, L"--scale=").c_str(), nullptr));
      if (cli.scale < 0.5f || cli.scale > 4.0f) cli.scale = 0.0f;
    } else if (StartsWith(arg, L"--ms=")) {
      cli.app.autoExitMs = static_cast<int>(wcstol(ValueAfter(arg, L"--ms=").c_str(), nullptr, 10));
    } else if (arg == L"--show") {
      cli.app.showAtStart = true;
    } else if (arg == L"--glass") {
      cli.app.glass = true;
    } else if (StartsWith(arg, L"--type=")) {
      cli.app.type = ValueAfter(arg, L"--type=");
    } else if (StartsWith(arg, L"--keys=")) {
      cli.app.keys = ValueAfter(arg, L"--keys=");
    } else if (StartsWith(arg, L"--compose=")) {
      cli.app.compose = ValueAfter(arg, L"--compose=");
    } else if (StartsWith(arg, L"--capture-ms=")) {
      cli.app.captureDelayMs = static_cast<int>(wcstol(ValueAfter(arg, L"--capture-ms=").c_str(),
                                                       nullptr, 10));
    } else if (StartsWith(arg, L"--selected=")) {
      cli.app.selected =
          static_cast<int>(wcstol(ValueAfter(arg, L"--selected=").c_str(), nullptr, 10));
    } else if (StartsWith(arg, L"--hover-row=")) {
      cli.app.hoverRow =
          static_cast<int>(wcstol(ValueAfter(arg, L"--hover-row=").c_str(), nullptr, 10));
    } else if (StartsWith(arg, L"--click-row=")) {
      cli.app.clickRow =
          static_cast<int>(wcstol(ValueAfter(arg, L"--click-row=").c_str(), nullptr, 10));
    } else if (arg == L"--tray-click") {
      cli.app.trayClickTest = true;
    } else if (arg == L"--no-hotkey") {
      cli.app.noHotkey = true;
    } else if (arg == L"--list-apps") {
      cli.listApps = true;
    } else if (arg == L"--list-files") {
      cli.listFiles = true;
    } else if (arg == L"--file-stats") {
      cli.fileStats = true;
    } else if (arg == L"--help" || arg == L"-h") {
      PrintUsage();
      exit(0);
    } else if (arg == L"--version" || arg == L"-v") {
      printf("%ls %ls\n", kAppName, kAppVersion);
      exit(0);
    } else {
      wprintf(L"未知参数: %ls\n", arg.c_str());
      PrintUsage();
      exit(2);
    }
  }
  // 命令行没指定时看环境变量：LUME_LOG_LEVEL=debug Lume.exe ...
  if (!cli.app.logLevelSpecified) {
    wchar_t buffer[64] = {};
    const DWORD length = GetEnvironmentVariableW(L"LUME_LOG_LEVEL", buffer, ARRAYSIZE(buffer));
    LogLevel level = LogLevel::Info;
    if (length > 0 && length < ARRAYSIZE(buffer) && ParseLogLevel(buffer, &level)) {
      cli.app.logLevel = level;
      cli.app.logLevelSpecified = true;
    }
  }
  return cli;
}

}  // namespace

namespace {

int RunSelfCheck() {
  LogInit(L"");
  int failures = 0;
  printf("Lume self-check\n");

  HWND hwnd = CreateWindowExW(0, L"STATIC", L"LumeSelfCheck", WS_POPUP, 0, 0, 0, 0, nullptr,
                              nullptr, GetModuleHandleW(nullptr), nullptr);
  if (!hwnd) {
    printf("window=FAIL\n");
    return 2;
  }

  AppSettings settings;
  SettingsStore::Load(&settings);

  HotkeyBinding binding;
  const bool parsed = ParseHotkey(settings.hotkey, &binding);
  const bool hotkeyOk = parsed && RegisterHotkeyBinding(hwnd, 0x51, binding, nullptr);
  printf("hotkey=%s (%ls)\n", hotkeyOk ? "PASS" : "FAIL", settings.hotkey.c_str());
  if (!hotkeyOk) ++failures;
  if (hotkeyOk) UnregisterHotkey(hwnd, 0x51);

  TrayIcon tray;
  const bool trayOk = tray.Create(hwnd);
  printf("tray=%s\n", trayOk ? "PASS" : "FAIL");
  if (!trayOk) ++failures;
  tray.Destroy();

  const bool wasAutostart = IsAutostartEnabled();
  std::wstring error;
  const bool setOk = SetAutostart(true, &error);
  const bool readOk = IsAutostartEnabled();
  const bool restoreOk = SetAutostart(wasAutostart, nullptr);
  const bool autostartOk = setOk && readOk && restoreOk;
  printf("autostart=%s (set=%d read=%d restore=%d)\n", autostartOk ? "PASS" : "FAIL", setOk ? 1 : 0,
         readOk ? 1 : 0, restoreOk ? 1 : 0);
  if (!autostartOk) ++failures;

  wchar_t tempDir[MAX_PATH] = {};
  GetTempPathW(ARRAYSIZE(tempDir), tempDir);
  const std::wstring samplePath = std::wstring(tempDir) + L"etools-selfcheck-opened.txt";
  HANDLE sample = CreateFileW(samplePath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
  if (sample != INVALID_HANDLE_VALUE) {
    const char content[] = "Lume self-check sample\n";
    DWORD written = 0;
    WriteFile(sample, content, sizeof(content) - 1, &written, nullptr);
    CloseHandle(sample);
  }
  error.clear();
  const bool openOk = OpenPath(samplePath, &error);
  printf("open=%s (%ls)\n", openOk ? "PASS" : "FAIL", samplePath.c_str());
  if (!openOk) ++failures;

  std::wstring savedPath;
  const bool saveOk = SettingsStore::Save(settings, &savedPath);
  if (savedPath.empty()) savedPath = SettingsStore::Path();
  printf("settings_save=%s (%ls)\n", saveOk ? "PASS" : "FAIL", savedPath.c_str());
  if (!saveOk) ++failures;

  // 筛选组：默认组能过校验、编码/解码往返一致、扩展名匹配抽检（纯函数，不碰磁盘）。
  {
    bool filterOk = true;
    FilterSet defaults = DefaultFilterSet();
    std::wstring filterError;
    if (!SanitizeFilterSet(&defaults, &filterError)) filterOk = false;
    for (const FilterGroup& group : defaults.groups) {
      FilterGroup decoded;
      if (!DecodeFilterGroup(EncodeFilterGroup(group), &decoded)) {
        filterOk = false;
        continue;
      }
      if (decoded.name != group.name || decoded.extensions != group.extensions ||
          decoded.includeApps != group.includeApps ||
          decoded.includeFolders != group.includeFolders ||
          decoded.includeNoExtension != group.includeNoExtension) {
        filterOk = false;
      }
    }
    const FilterGroup* images = nullptr;
    const FilterGroup* apps = nullptr;
    for (const FilterGroup& group : defaults.groups) {
      if (group.name == L"图片") images = &group;
      if (group.name == L"应用程序") apps = &group;
    }
    if (!images || !apps) filterOk = false;
    if (images) {
      if (!FilterMatchesPath(*images, L"C:\\pics\\a.PNG", false)) filterOk = false;
      if (FilterMatchesPath(*images, L"C:\\pics\\a.txt", false)) filterOk = false;
      if (FilterMatchesPath(*images, L"C:\\pics\\readme", false)) filterOk = false;
      if (FilterMatchesPath(*images, L"C:\\pics\\folder", true)) filterOk = false;
      if (!FilterIsExtensionWhitelist(*images)) filterOk = false;
      if (FilterEverythingSpec(*images).empty()) filterOk = false;
    }
    if (apps && !FilterMatchesPath(*apps, L"C:\\tools\\x.exe", false)) filterOk = false;
    FilterGroup foldersOnly;
    foldersOnly.name = L"目录";
    foldersOnly.includeFolders = true;
    if (FilterIsExtensionWhitelist(foldersOnly)) filterOk = false;
    printf("filters=%s (groups=%zu)\n", filterOk ? "PASS" : "FAIL", defaults.groups.size());
    if (!filterOk) ++failures;
  }

  // Everything 是可选来源：没装/没运行算 SKIP（不算失败），装了但查询失败才算 FAIL。
  {
    EverythingIndex everything;
    const EverythingStatus status = everything.Status();
    if (!status.available) {
      printf("everything=SKIP (%s)\n", WideToUtf8(status.error).c_str());
    } else {
      std::vector<FileHit> hits;
      uint64_t elapsedMs = 0;
      const bool queryOk = everything.QueryBlocking(L"notepad", &hits, &elapsedMs);
      printf("everything=%s (version=%s db_loaded=%d hits=%zu ms=%llu)\n",
             queryOk ? "PASS" : "FAIL", WideToUtf8(status.version).c_str(),
             status.dbLoaded ? 1 : 0, hits.size(),
             static_cast<unsigned long long>(elapsedMs));
      if (!queryOk) ++failures;
    }
  }

  DestroyWindow(hwnd);
  printf("result=%s\n", failures == 0 ? "PASS" : "FAIL");
  LogShutdown();
  return failures == 0 ? 0 : 2;
}

// 合成数据压力测试：把 count 条假条目放进内存里的 FileIndex，再跑真实搜索。
// 不计入计时的是「造数据」这一步；计时包含扫描、字符位图预筛、打分、过滤、排序全流程。
int RunSearchBench(size_t entries, int iterations) {
  LogInit(L"");
  printf("Lume search bench (synthetic, in-memory)\n");

  LARGE_INTEGER frequency{};
  QueryPerformanceFrequency(&frequency);
  auto nowMicros = [&frequency]() {
    LARGE_INTEGER counter{};
    QueryPerformanceCounter(&counter);
    return static_cast<double>(counter.QuadPart) * 1000000.0 /
           static_cast<double>(frequency.QuadPart);
  };

  FileIndex index;
  const double buildStart = nowMicros();
  index.LoadSyntheticForBenchmark(entries, 20261101u);
  const double buildMs = (nowMicros() - buildStart) / 1000.0;
  const size_t total = index.size();
  printf("entries=%zu build_ms=%.0f mem_mb=%.1f iters=%d limit=%zu\n", total, buildMs,
         PeakWorkingSetMb(), iterations, kMaxFileHits);

  const FilterSet filters = DefaultFilterSet();
  const wchar_t* queries[] = {L"report", L"photo", L"setup", L"2024", L"a", L"文档"};
  const wchar_t* wanted[] = {L"应用程序", L"图片"};
  struct BenchCase {
    const wchar_t* query;
    const FilterGroup* group;
    const wchar_t* label;
  };
  std::vector<BenchCase> cases;
  for (const wchar_t* query : queries) {
    cases.push_back({query, nullptr, L"全部"});
    for (const wchar_t* name : wanted) {
      for (const FilterGroup& group : filters.groups) {
        if (group.name == name) cases.push_back({query, &group, name});
      }
    }
  }

  printf("%-10s %-8s %6s %9s %9s %9s %10s\n", "query", "filter", "hits", "min_ms", "med_ms",
         "p95_ms", "scan_M_per_s");
  bool filterOk = true;
  for (const BenchCase& item : cases) {
    (void)index.Search(item.query, kMaxFileHits, item.group);  // 预热（页面/分支预测）
    std::vector<double> times;
    std::vector<FileHit> hits;
    for (int i = 0; i < iterations; ++i) {
      const double start = nowMicros();
      hits = index.Search(item.query, kMaxFileHits, item.group);
      times.push_back((nowMicros() - start) / 1000.0);
    }
    if (item.group) {
      for (const FileHit& hit : hits) {
        if (!FilterMatchesPath(*item.group, hit.path, hit.isDirectory)) filterOk = false;
      }
    }
    std::sort(times.begin(), times.end());
    const double minMs = times.front();
    const double medMs = times[times.size() / 2];
    const double p95Ms = times[std::min(times.size() - 1, (times.size() * 95) / 100)];
    const double scanRate =
        medMs > 0.0 ? static_cast<double>(total) / (medMs / 1000.0) / 1000000.0 : 0.0;
    printf("%-10s %-8s %6zu %9.2f %9.2f %9.2f %10.1f\n", WideToUtf8(item.query).c_str(),
           WideToUtf8(item.label).c_str(), hits.size(), minMs, medMs, p95Ms, scanRate);
  }

  // 单条筛选判定的成本：与扫描解耦，直接按名字重复调用谓词。
  {
    std::vector<std::wstring> names;
    names.reserve(200000);
    const wchar_t* words[] = {L"report", L"photo", L"setup", L"draft", L"backup"};
    for (size_t i = 0; i < 200000; ++i) {
      std::wstring name = words[i % 5];
      name += L'_';
      name += std::to_wstring(i);
      if (i % 7 != 0) {
        name += L'.';
        name += (i % 3 == 0) ? L"png" : L"txt";
      }
      names.push_back(std::move(name));
    }
    for (const FilterGroup& group : filters.groups) {
      if (group.name != L"应用程序" && group.name != L"图片" && group.name != L"压缩包") continue;
      const size_t calls = 2000000;
      size_t matched = 0;
      const double start = nowMicros();
      for (size_t i = 0; i < calls; ++i) {
        if (FilterMatchesPath(group, names[i % names.size()], false)) ++matched;
      }
      const double ns = (nowMicros() - start) * 1000.0 / static_cast<double>(calls);
      printf("predicate group=%s ext=%zu ns_per_call=%.1f matched=%.1f%%\n",
             WideToUtf8(group.name).c_str(), group.extensions.size(), ns,
             100.0 * static_cast<double>(matched) / static_cast<double>(calls));
    }
  }

  printf("filter_check=%s\n", filterOk ? "ok" : "FAIL");
  fflush(stdout);
  LogShutdown();
  return filterOk ? 0 : 2;
}

}  // namespace

// 诊断命令的 file_search 口径：CLI 覆盖优先，否则看设置；这些命令本身始终用本地索引。
std::wstring EffectiveFileSearch(const CliOptions& cli, const AppSettings& settings) {
  if (cli.app.fileSearchSpecified) {
    return cli.app.fileSearch == L"everything" ? L"everything" : L"local";
  }
  return settings.fileSearch == L"everything" ? L"everything" : L"local";
}

namespace {

// 诊断命令共用的筛选口径：设置里的组 + --filter 覆盖。
struct CliFilter {
  FilterSet set;
  int active = 0;
};

CliFilter ResolveCliFilter(const CliOptions& cli, const AppSettings& settings) {
  CliFilter filter;
  filter.set = settings.filters;
  if (!cli.filterSpec.empty()) {
    bool enabled = filter.set.enabled;
    int index = filter.set.active;
    if (ParseFilterSpec(filter.set, cli.filterSpec, &enabled, &index)) {
      filter.set.enabled = enabled;
      filter.set.active = index;
    } else {
      printf("filter=%s (无法识别，沿用设置值)\n", WideToUtf8(cli.filterSpec).c_str());
    }
  }
  ClampFilterActive(&filter.set);
  filter.active = filter.set.active;
  return filter;
}

// 当前生效的组名（off = 筛选已关闭，全部 = 不过滤）。
std::wstring FilterLabel(const FilterSet& set) {
  if (!set.enabled) return L"off";
  const std::vector<std::wstring> labels = FilterChipLabels(set);
  const int active = std::clamp(set.active, 0, static_cast<int>(labels.size()) - 1);
  return labels[static_cast<size_t>(active)];
}

}  // namespace
int RunMain(int argc, wchar_t** argv) {
  SetConsoleOutputCP(CP_UTF8);
  if (!EnablePerMonitorDpi()) {
    // 具体失败原因由 app 内部记录；这里仅保证尽早设置。
  }
  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

  CliOptions cli = Parse(argc, argv);
  LogSetLevel(cli.app.logLevel);
  int exitCode = 0;
  if (cli.listApps) {
    for (const AppEntry& entry : CollectAppEntries()) {
      const std::string line = WideToUtf8(entry.title) + "\n";
      fputs(line.c_str(), stdout);
    }
    fflush(stdout);
    CoUninitialize();
    return 0;
  }
  if (cli.listFiles || cli.fileStats) {
    FileIndex files;
    if (cli.indexSizeMB > 0) files.SetMaxIndexBytes(cli.indexSizeMB * 1024ull * 1024ull);
    files.SetIndexDirectory(cli.app.indexDir);
    if (!cli.app.fileRoot.empty()) files.SetRoots({cli.app.fileRoot});
    const ULONGLONG start = GetTickCount64();
    const size_t count = files.RebuildSynchronously();
    const ULONGLONG elapsed = GetTickCount64() - start;
    for (const std::wstring& root : files.roots()) {
      const std::string line = "root: " + WideToUtf8(root) + "\n";
      fputs(line.c_str(), stdout);
    }
    const std::string indexLine = "index: " + WideToUtf8(files.snapshotPath()) + "\n";
    fputs(indexLine.c_str(), stdout);
    char summary[192];
    snprintf(summary, sizeof(summary), "files: %zu, rebuild: %llu ms, peak_ws: %.0f MB\n", count,
             static_cast<unsigned long long>(elapsed), PeakWorkingSetMb());
    fputs(summary, stdout);
    if (cli.listFiles) {
      for (const std::wstring& path : files.SamplePaths(20)) {
        const std::string line = WideToUtf8(path) + "\n";
        fputs(line.c_str(), stdout);
      }
    }
    fflush(stdout);
    CoUninitialize();
    return 0;
  }
  if (!cli.searchLive.empty()) {
    LogInit(cli.app.logPath);
    AppSettings settings;
    SettingsStore::Load(&settings);
    const CliFilter filter = ResolveCliFilter(cli, settings);
    printf("file_search=%s filter=%s (诊断命令始终使用本地索引)\n",
           WideToUtf8(EffectiveFileSearch(cli, settings)).c_str(),
           WideToUtf8(FilterLabel(filter.set)).c_str());
    const size_t sizeMB = cli.indexSizeMB > 0 ? cli.indexSizeMB
                                               : static_cast<size_t>(settings.indexSizeMB);
    FileIndex index;
    index.SetMaxIndexBytes(sizeMB * 1024ull * 1024ull);
    index.SetPersistSnapshot(false);
    index.SetIndexDirectory(cli.app.indexDir);
    std::vector<std::wstring> roots = settings.fileRoots;
    if (!cli.app.fileRoot.empty()) roots = {cli.app.fileRoot};
    if (!roots.empty()) index.SetRoots(roots);
    const ULONGLONG start = GetTickCount64();
    const size_t count = index.RebuildSynchronously();
    printf("roots=%zu files=%zu size=%zu MB rebuild=%llu ms index=%ls\n", index.roots().size(),
           count, sizeMB, static_cast<unsigned long long>(GetTickCount64() - start),
           index.snapshotPath().c_str());
    LARGE_INTEGER frequency{};
    LARGE_INTEGER searchStart{};
    LARGE_INTEGER searchEnd{};
    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&searchStart);
    const std::vector<FileHit> hits =
        index.Search(cli.searchLive, 2000, ActiveFilterGroup(filter.set));
    QueryPerformanceCounter(&searchEnd);
    const double searchMs = static_cast<double>(searchEnd.QuadPart - searchStart.QuadPart) *
                            1000.0 / static_cast<double>(frequency.QuadPart);
    printf("search_ms=%.2f hits=%zu\n", searchMs, hits.size());
    size_t exactly = 0;
    const std::wstring needle = cli.searchLive;
    for (const FileHit& hit : hits) {
      const std::wstring& path = hit.path;
      const size_t slash = path.find_last_of(L"\\/");
      const std::wstring name = slash == std::wstring::npos ? path : path.substr(slash + 1);
      if (_wcsicmp(name.c_str(), needle.c_str()) == 0) ++exactly;
    }
    printf("exact_name=%zu\n", exactly);
    for (size_t i = 0; i < hits.size() && i < 30; ++i) {
      const FileHit& hit = hits[i];
      wchar_t line[1024];
      swprintf(line, ARRAYSIZE(line), L"%zu%s%s %ls\n", hit.score, hit.pathOnly ? L" [path]" : L"",
               hit.isDirectory ? L" [dir]" : L"", hit.path.c_str());
      fputs(WideToUtf8(line).c_str(), stdout);
    }
    fflush(stdout);
    LogShutdown();
    CoUninitialize();
    return 0;
  }
  if (cli.listFilters) {
    LogInit(cli.app.logPath);
    AppSettings settings;
    SettingsStore::Load(&settings);
    const CliFilter filter = ResolveCliFilter(cli, settings);
    const std::vector<std::wstring> labels = FilterChipLabels(filter.set);
    printf("enabled=%d active=%d groups=%zu\n", filter.set.enabled ? 1 : 0, filter.active,
           filter.set.groups.size());
    printf("[0] %s (built-in)\n", WideToUtf8(labels[0]).c_str());
    for (size_t i = 0; i < filter.set.groups.size(); ++i) {
      const FilterGroup& group = filter.set.groups[i];
      printf("[%zu] %s apps=%d folders=%d noext=%d ext=%s\n", i + 1,
             WideToUtf8(group.name).c_str(), group.includeApps ? 1 : 0,
             group.includeFolders ? 1 : 0, group.includeNoExtension ? 1 : 0,
             WideToUtf8(FilterEverythingSpec(group)).c_str());
    }
    printf("selected=%s\n", WideToUtf8(FilterLabel(filter.set)).c_str());
    // 设置里的组必须能通过校验（默认组也是），否则算失败。
    FilterSet validated = settings.filters;
    std::wstring error;
    const bool ok = SanitizeFilterSet(&validated, &error);
    if (!ok) printf("error=%s\n", WideToUtf8(error).c_str());
    fflush(stdout);
    LogShutdown();
    CoUninitialize();
    return ok ? 0 : 2;
  }
  if (cli.everythingCheck) {
    LogInit(cli.app.logPath);
    AppSettings settings;
    SettingsStore::Load(&settings);
    const CliFilter filter = ResolveCliFilter(cli, settings);
    printf("filter=%s\n", WideToUtf8(FilterLabel(filter.set)).c_str());
    EverythingIndex everything;
    const EverythingStatus status = everything.Status();
    printf("available=%d db_loaded=%d version=%ls\n", status.available ? 1 : 0,
           status.dbLoaded ? 1 : 0, status.version.c_str());
    if (!status.available) {
      printf("error=%s\n", WideToUtf8(status.error).c_str());
    }
    std::vector<FileHit> hits;
    uint64_t elapsedMs = 0;
    const bool ok = everything.QueryBlocking(cli.everythingQuery, &hits, &elapsedMs,
                                            ActiveFilterGroup(filter.set));
    printf("query=\"%s\" sanitized=\"%s\" search=\"%s\" ok=%d hits=%zu elapsed_ms=%llu\n",
           WideToUtf8(cli.everythingQuery).c_str(),
           WideToUtf8(EverythingIndex::SanitizeQuery(cli.everythingQuery)).c_str(),
           WideToUtf8(everything.lastSearch()).c_str(), ok ? 1 : 0, hits.size(),
           static_cast<unsigned long long>(elapsedMs));
    for (size_t i = 0; i < hits.size() && i < 10; ++i) {
      wchar_t line[1024];
      swprintf(line, ARRAYSIZE(line), L"  q=%u score=%zu%s %ls\n", hits[i].quality, hits[i].score,
               hits[i].isDirectory ? L" [dir]" : L"", hits[i].path.c_str());
      fputs(WideToUtf8(line).c_str(), stdout);
    }
    // 同一批 hits 过一遍合并层：验证换来源后 tier/quality/score 口径一致。
    const std::vector<AppEntry> apps = CollectAppEntries();
    const std::vector<ResultRow> rows = BuildMergedRows(apps, hits, nullptr, cli.everythingQuery,
                                                        20, false, ActiveFilterGroup(filter.set));
    printf("apps=%zu merged_rows=%zu\n", apps.size(), rows.size());
    for (const ResultRow& row : rows) {
      wchar_t line[1024];
      swprintf(line, ARRAYSIZE(line), L"tier=%u quality=%u score=%zu %ls | %ls\n", row.tier,
               row.quality, row.score, row.title.c_str(), row.subtitle.c_str());
      fputs(WideToUtf8(line).c_str(), stdout);
    }
    fflush(stdout);
    LogShutdown();
    CoUninitialize();
    return status.available ? 0 : 2;
  }
  if (!cli.searchRows.empty()) {
    LogInit(cli.app.logPath);
    AppSettings settings;
    SettingsStore::Load(&settings);
    const CliFilter filter = ResolveCliFilter(cli, settings);
    printf("file_search=%s filter=%s (诊断命令始终使用本地索引)\n",
           WideToUtf8(EffectiveFileSearch(cli, settings)).c_str(),
           WideToUtf8(FilterLabel(filter.set)).c_str());
    FileIndex index;
    if (cli.indexSizeMB > 0) index.SetMaxIndexBytes(cli.indexSizeMB * 1024ull * 1024ull);
    index.SetIndexDirectory(cli.app.indexDir);
    std::vector<std::wstring> roots = settings.fileRoots;
    if (!cli.app.fileRoot.empty()) roots = {cli.app.fileRoot};
    if (!roots.empty()) index.SetRoots(roots);
    index.LoadCachedSnapshot();
    const std::vector<AppEntry> apps = CollectAppEntries();
    const FilterGroup* group = ActiveFilterGroup(filter.set);
    const std::vector<FileHit> fileHits = index.Search(cli.searchRows, kMaxFileHits, group);
    const std::vector<ResultRow> rows =
        BuildMergedRows(apps, fileHits, nullptr, cli.searchRows, 40, false, group);
    printf("roots=%zu files=%zu apps=%zu rows=%zu index=%ls\n", index.roots().size(),
           index.size(), apps.size(), rows.size(), index.snapshotPath().c_str());
    for (const ResultRow& row : rows) {
      wchar_t line[1024];
      swprintf(line, ARRAYSIZE(line), L"tier=%u quality=%u score=%zu %ls | %ls\n", row.tier,
               row.quality, row.score, row.title.c_str(), row.subtitle.c_str());
      fputs(WideToUtf8(line).c_str(), stdout);
    }
    fflush(stdout);
    LogShutdown();
    CoUninitialize();
    return 0;
  }
  if (!cli.searchFiles.empty()) {
    LogInit(cli.app.logPath);
    FileIndex index;
    if (cli.indexSizeMB > 0) index.SetMaxIndexBytes(cli.indexSizeMB * 1024ull * 1024ull);
    index.SetIndexDirectory(cli.app.indexDir);
    AppSettings settings;
    SettingsStore::Load(&settings);
    const CliFilter filter = ResolveCliFilter(cli, settings);
    printf("file_search=%s filter=%s (诊断命令始终使用本地索引)\n",
           WideToUtf8(EffectiveFileSearch(cli, settings)).c_str(),
           WideToUtf8(FilterLabel(filter.set)).c_str());
    std::vector<std::wstring> roots = settings.fileRoots;
    if (!cli.app.fileRoot.empty()) roots = {cli.app.fileRoot};
    if (!roots.empty()) index.SetRoots(roots);
    const bool loaded = index.LoadCachedSnapshot();
    printf("roots=%zu files=%zu snapshot=%d bytes=%llu index=%ls\n", index.roots().size(),
           index.size(), loaded ? 1 : 0, static_cast<unsigned long long>(index.snapshotBytes()),
           index.snapshotPath().c_str());
    LARGE_INTEGER frequency{};
    LARGE_INTEGER started{};
    LARGE_INTEGER finished{};
    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&started);
    const std::vector<FileHit> hits =
        index.Search(cli.searchFiles, 20, ActiveFilterGroup(filter.set));
    QueryPerformanceCounter(&finished);
    const double elapsedMs = static_cast<double>(finished.QuadPart - started.QuadPart) * 1000.0 /
                             static_cast<double>(frequency.QuadPart);
    printf("search_ms=%.2f hits=%zu\n", elapsedMs, hits.size());
    for (const FileHit& hit : hits) {
      const std::string line =
          std::to_string(hit.score) + "  " + WideToUtf8(hit.path) + "\n";
      fputs(line.c_str(), stdout);
    }
    fflush(stdout);
    LogShutdown();
    CoUninitialize();
    return 0;
  }
  if (cli.benchSearch) {
    const size_t entries = std::clamp<size_t>(cli.benchEntries, 1000, 50000000);
    const int iterations = std::clamp(cli.benchIters, 1, 100);
    const int code = RunSearchBench(entries, iterations);
    CoUninitialize();
    return code;
  }
  if (cli.selfCheck) {
    const int code = RunSelfCheck();
    CoUninitialize();
    return code;
  }
  if (!cli.setAutostart.empty()) {
    const bool enabled = cli.setAutostart == L"1" || cli.setAutostart == L"on" ||
                         cli.setAutostart == L"true";
    std::wstring error;
    const bool ok = SetAutostart(enabled, &error);
    printf("autostart=%d ok=%d%s%s\n", IsAutostartEnabled() ? 1 : 0, ok ? 1 : 0,
           ok ? "" : " error=", ok ? "" : WideToUtf8(error).c_str());
    CoUninitialize();
    return ok ? 0 : 2;
  }
  if (!cli.openPath.empty()) {
    LogInit(cli.app.logPath);
    std::wstring error;
    const bool ok = OpenPath(cli.openPath, &error);
    const std::string line = std::string("open ") + (ok ? "ok" : "failed: ") +
                             (ok ? WideToUtf8(cli.openPath) : WideToUtf8(error)) + "\n";
    fputs(line.c_str(), stdout);
    LogShutdown();
    CoUninitialize();
    return ok ? 0 : 2;
  }

  if (!cli.shot.empty()) {
    exitCode = RunScreenshot(cli.app, cli.shot, cli.scale > 0.0f ? cli.scale : 1.0f);
  } else if (!cli.shotMatrix.empty()) {
    exitCode = RunScreenshotMatrix(cli.app, cli.shotMatrix);
  } else {
    exitCode = RunLauncher(cli.app);
  }
  CoUninitialize();
  return exitCode;
}

// 正式版是 GUI 子系统：双击不出现黑色控制台窗口；
// 从命令行/重定向运行时复用继承的 stdout，或附加父控制台，保证 CLI 输出可见。
int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
  int argc = 0;
  LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
  if (!argv) return 1;
  bool attached = false;
  bool configured = false;
  HANDLE inherited = GetStdHandle(STD_OUTPUT_HANDLE);
  if (inherited && inherited != INVALID_HANDLE_VALUE) {
    const int fd = _open_osfhandle(reinterpret_cast<intptr_t>(inherited), _O_WRONLY | _O_TEXT);
    if (fd != -1) {
      _dup2(fd, _fileno(stdout));
      _close(fd);
      configured = true;
    }
  }
  if (!configured && AttachConsole(ATTACH_PARENT_PROCESS)) {
    attached = true;
    freopen("CONOUT$", "w", stdout);
    freopen("CONOUT$", "w", stderr);
    SetConsoleOutputCP(CP_UTF8);
  }
  const int code = RunMain(argc, argv);
  LocalFree(argv);
  if (attached) FreeConsole();
  return code;
}

