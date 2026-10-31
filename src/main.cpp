#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <io.h>
#include <string>

#include "app/app.h"
#include "app/settings.h"
#include "core/log.h"
#include "launch/launcher.h"
#include "platform/autostart.h"
#include "platform/hotkey.h"
#include "platform/system.h"
#include "platform/tray.h"
#include "search/app_entry.h"
#include "search/file_index.h"
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
};

void PrintUsage() {
  fputs(
      "Lume 0.1.0\n"
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
      "  --log=<path>                     Write log to this file instead of %%LOCALAPPDATA%%\n"
      "  --query=<text>                   Prefill query text\n"
      "  --scale=<factor>                 Screenshot scale (default 1.0)\n"
      "  --glass                          Lower panel opacity for backdrop inspection\n"
      "  --type=<text>                    Inject characters after showing (input test)\n"
      "  --keys=up,down,enter             Inject key sequence after showing\n"
      "  --compose=<text>                 Inject IME composition string (render test)\n"
      "  --capture-ms=<n>                 Capture delay in ms (default 450, use 30 for fade)\n"
      "  --list-apps                      Print the collected app index and exit\n"
      "  --list-files                     Rebuild the file index, print stats and sample paths\n"
      "  --file-stats                     Rebuild the file index and print stats only\n"
      "  --index-size-mb=<n>              Override max index file size in MB (64–262144)\n"
      "  --search-files=<query>           Search the file index and print top results with scores\n"
      "  --search-rows=<query>            Print merged app/file rows with tier and score\n"
      "  --search-live=<query>            Rebuild index now and search it (diagnostics)\n"
      "  --open=<path>                    Verify opening a file/path directly\n"
      "  --set-autostart=1|0              Enable/disable autostart and print state\n"
      "  --hotkey=<combo>                 Override hotkey for this run (e.g. Ctrl+Alt+K)\n"
      "  --settings                       Open the settings window at startup\n"
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
    } else if (StartsWith(arg, L"--file-root=")) {
      cli.app.fileRoot = ValueAfter(arg, L"--file-root=");
    } else if (StartsWith(arg, L"--open=")) {
      cli.openPath = ValueAfter(arg, L"--open=");
    } else if (StartsWith(arg, L"--set-autostart=")) {
      cli.setAutostart = ValueAfter(arg, L"--set-autostart=");
    } else if (StartsWith(arg, L"--settings")) {
      cli.app.openSettings = true;
    } else if (arg == L"--clear-index") {
      cli.app.clearIndexOnStart = true;
    } else if (StartsWith(arg, L"--test-capture=")) {
      cli.app.testCapture = ValueAfter(arg, L"--test-capture=");
      cli.app.openSettings = true;
    } else if (StartsWith(arg, L"--search-files=")) {
      cli.searchFiles = ValueAfter(arg, L"--search-files=");
    } else if (StartsWith(arg, L"--search-rows=")) {
      cli.searchRows = ValueAfter(arg, L"--search-rows=");
    } else if (StartsWith(arg, L"--search-live=")) {
      cli.searchLive = ValueAfter(arg, L"--search-live=");
    } else if (StartsWith(arg, L"--index-size-mb=")) {
      cli.indexSizeMB = static_cast<size_t>(_wcstoui64(
          ValueAfter(arg, L"--index-size-mb=").c_str(), nullptr, 10));
      cli.app.indexSizeMB = cli.indexSizeMB;
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
      printf("Lume 0.1.0\n");
      exit(0);
    } else {
      wprintf(L"未知参数: %ls\n", arg.c_str());
      PrintUsage();
      exit(2);
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

  DestroyWindow(hwnd);
  printf("result=%s\n", failures == 0 ? "PASS" : "FAIL");
  LogShutdown();
  return failures == 0 ? 0 : 2;
}

}  // namespace

int RunMain(int argc, wchar_t** argv) {
  SetConsoleOutputCP(CP_UTF8);
  if (!EnablePerMonitorDpi()) {
    // 具体失败原因由 app 内部记录；这里仅保证尽早设置。
  }
  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

  CliOptions cli = Parse(argc, argv);
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
    if (!cli.app.fileRoot.empty()) files.SetRoots({cli.app.fileRoot});
    const ULONGLONG start = GetTickCount64();
    const size_t count = files.RebuildSynchronously();
    const ULONGLONG elapsed = GetTickCount64() - start;
    for (const std::wstring& root : files.roots()) {
      const std::string line = "root: " + WideToUtf8(root) + "\n";
      fputs(line.c_str(), stdout);
    }
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
    const size_t sizeMB = cli.indexSizeMB > 0 ? cli.indexSizeMB
                                               : static_cast<size_t>(settings.indexSizeMB);
    FileIndex index;
    index.SetMaxIndexBytes(sizeMB * 1024ull * 1024ull);
    index.SetPersistSnapshot(false);
    std::vector<std::wstring> roots = settings.fileRoots;
    if (!cli.app.fileRoot.empty()) roots = {cli.app.fileRoot};
    if (!roots.empty()) index.SetRoots(roots);
    const ULONGLONG start = GetTickCount64();
    const size_t count = index.RebuildSynchronously();
    printf("roots=%zu files=%zu size=%zu MB rebuild=%llu ms\n", index.roots().size(), count,
           sizeMB, static_cast<unsigned long long>(GetTickCount64() - start));
    LARGE_INTEGER frequency{};
    LARGE_INTEGER searchStart{};
    LARGE_INTEGER searchEnd{};
    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&searchStart);
    const std::vector<FileHit> hits = index.Search(cli.searchLive, 2000);
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
  if (!cli.searchRows.empty()) {
    LogInit(cli.app.logPath);
    AppSettings settings;
    SettingsStore::Load(&settings);
    FileIndex index;
    if (cli.indexSizeMB > 0) index.SetMaxIndexBytes(cli.indexSizeMB * 1024ull * 1024ull);
    std::vector<std::wstring> roots = settings.fileRoots;
    if (!cli.app.fileRoot.empty()) roots = {cli.app.fileRoot};
    if (!roots.empty()) index.SetRoots(roots);
    index.LoadCachedSnapshot();
    const std::vector<AppEntry> apps = CollectAppEntries();
    const std::vector<ResultRow> rows =
        BuildMergedRows(apps, &index, nullptr, cli.searchRows, 40, false);
    printf("roots=%zu files=%zu apps=%zu rows=%zu\n", index.roots().size(), index.size(),
           apps.size(), rows.size());
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
    AppSettings settings;
    SettingsStore::Load(&settings);
    std::vector<std::wstring> roots = settings.fileRoots;
    if (!cli.app.fileRoot.empty()) roots = {cli.app.fileRoot};
    if (!roots.empty()) index.SetRoots(roots);
    const bool loaded = index.LoadCachedSnapshot();
    printf("roots=%zu files=%zu snapshot=%d\n", index.roots().size(), index.size(),
           loaded ? 1 : 0);
    LARGE_INTEGER frequency{};
    LARGE_INTEGER started{};
    LARGE_INTEGER finished{};
    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&started);
    const std::vector<FileHit> hits = index.Search(cli.searchFiles, 20);
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

