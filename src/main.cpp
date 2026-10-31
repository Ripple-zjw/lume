#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <string>

#include "app/app.h"
#include "core/log.h"
#include "platform/system.h"
#include "search/app_entry.h"
#include "ui/theme.h"

namespace {

struct CliOptions {
  AppOptions app;
  std::wstring shot;
  std::wstring shotMatrix;
  float scale = 0.0f;
  bool listApps = false;
};

void PrintUsage() {
  fputs(
      "eTools launcher (P1)\n"
      "\n"
      "Usage:\n"
      "  eTools.exe                       Run in tray-less resident mode (hotkey toggles)\n"
      "  eTools.exe --show                Start visible (auto-hide on focus loss)\n"
      "  eTools.exe --ms=1500             Auto exit after N milliseconds\n"
      "  eTools.exe --capture=out.png     Show, capture composited window, exit\n"
      "  eTools.exe --shot=out.png        Offscreen render with simulated wallpaper\n"
      "  eTools.exe --shot-matrix=dir     Render 3 DPIs x 2 themes baselines into dir\n"
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
    } else if (StartsWith(arg, L"--accent=")) {
      cli.app.accent = ValueAfter(arg, L"--accent=");
    } else if (StartsWith(arg, L"--log=")) {
      cli.app.logPath = ValueAfter(arg, L"--log=");
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
    } else if (arg == L"--help" || arg == L"-h") {
      PrintUsage();
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

int wmain(int argc, wchar_t** argv) {
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
