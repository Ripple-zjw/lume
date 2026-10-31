#include "app/app.h"

#include <cmath>
#include <utility>

#include "core/com_ptr.h"
#include "core/log.h"
#include "platform/hotkey.h"
#include "platform/system.h"
#include "ui/renderer.h"
#include "ui/scene.h"
#include "ui/window.h"

namespace {

constexpr int kHotkeyId = 1;
constexpr UINT_PTR kTimerCapture = 0x10;
constexpr UINT_PTR kTimerExit = 0x11;
constexpr int kRowsForLayout = 3;

UINT PanelCanvasWidth(const Theme& theme, float scale) {
  return static_cast<UINT>(std::lround((theme.panelWidth + theme.shadowMargin * 2.0f) * scale));
}

UINT PanelCanvasHeight(const Theme& theme, float scale) {
  return static_cast<UINT>(
      std::lround((PanelHeight(theme, kRowsForLayout) + theme.shadowMargin * 2.0f) * scale));
}

}  // namespace

int RunScreenshot(const AppOptions& options, const std::wstring& path, float scale) {
  LogInit(options.logPath);
  Renderer renderer;
  if (!renderer.Init()) {
    Log(L"渲染器初始化失败");
    return 2;
  }
  Theme theme = MakeTheme(options.themeMode, ParseAccent(options.accent));
  Scene scene;
  if (!scene.Init(renderer, theme)) {
    Log(L"场景初始化失败");
    return 2;
  }
  if (!renderer.AttachOffscreen(PanelCanvasWidth(theme, scale), PanelCanvasHeight(theme, scale))) {
    Log(L"离屏目标创建失败");
    return 2;
  }
  if (!renderer.BeginFrame()) return 2;
  SceneState state;
  state.query = options.query;
  state.selected = 0;
  state.glass = options.glass;
  scene.Draw(theme, state, true, scale);
  renderer.EndFrame();
  return renderer.SaveFrameToPng(path) ? 0 : 2;
}

int RunScreenshotMatrix(const AppOptions& options, const std::wstring& directory) {
  if (!EnsureDirectory(directory)) {
    Log(L"无法创建目录: %ls", directory.c_str());
    return 2;
  }
  const float scales[] = {1.0f, 1.5f, 2.0f};
  const ThemeMode modes[] = {ThemeMode::Dark, ThemeMode::Light};
  int failures = 0;
  for (ThemeMode mode : modes) {
    for (float scale : scales) {
      AppOptions single = options;
      single.themeMode = mode;
      const int dpi = static_cast<int>(std::lround(scale * 100.0f));
      wchar_t name[96];
      swprintf(name, ARRAYSIZE(name), L"%ls\\baseline-%ls-%d.png", directory.c_str(),
               ThemeModeName(mode), dpi);
      if (RunScreenshot(single, name, scale) != 0) {
        Log(L"生成失败: %ls", name);
        ++failures;
      }
    }
  }
  Log(L"基线矩阵完成: %d 张, 失败 %d", 6 - failures, failures);
  return failures == 0 ? 0 : 2;
}

int RunLauncher(const AppOptions& options) {
  if (!EnablePerMonitorDpi()) Log(L"Per-Monitor V2 DPI 感知设置失败");
  LogInit(options.logPath);

  const OsVersion version = QueryOsVersion();
  Log(L"Windows %ls", FormatOsVersion(version).c_str());
  if (!IsSupportedOs()) {
    MessageBoxW(nullptr, L"eTools 需要 Windows 11 22H2（build 22621）或更高版本。", L"eTools",
                MB_ICONERROR | MB_OK);
    return 2;
  }

  HANDLE instanceMutex = CreateMutexW(nullptr, TRUE, L"Local\\eTools.Launcher.SingleInstance");
  if (instanceMutex && GetLastError() == ERROR_ALREADY_EXISTS) {
    if (HWND existing = FindWindowW(WindowClassName(), nullptr)) {
      PostMessageW(existing, kMsgToggle, 0, 0);
    }
    CloseHandle(instanceMutex);
    return 0;
  }

  Renderer renderer;
  Scene scene;
  LauncherWindow window;
  int exitCode = 0;

  Theme theme = MakeTheme(options.themeMode, ParseAccent(options.accent));
  if (!renderer.Init()) {
    Log(L"渲染器初始化失败");
    exitCode = 2;
  } else if (!scene.Init(renderer, theme)) {
    Log(L"场景初始化失败");
    exitCode = 2;
  } else if (!window.Create(GetModuleHandleW(nullptr), &renderer, &scene)) {
    Log(L"窗口创建失败");
    exitCode = 2;
  }

  if (exitCode == 0) {
    window.SetTheme(theme);
    SceneState state;
    state.query = options.query;
    state.glass = options.glass;
    window.SetSceneState(state);
    window.RenderNow();  // 隐藏预热：把首次绘制的着色器/布局成本挪到启动阶段

    std::wstring hotkeyName;
    if (!options.noHotkey && !RegisterDefaultHotkey(window.hwnd(), kHotkeyId, &hotkeyName)) {
      Log(L"没有可用的全局热键，仅能通过 --show 测试");
    }

    window.SetOnHotkey([&window]() { window.Toggle(); });
    window.SetOnTimer([&](UINT_PTR id) {
      if (id == kTimerCapture) {
        KillTimer(window.hwnd(), kTimerCapture);
        if (!options.capturePath.empty() && !renderer.CaptureWindowToPng(options.capturePath)) {
          Log(L"截图失败");
        }
        PostQuitMessage(0);
      } else if (id == kTimerExit) {
        KillTimer(window.hwnd(), kTimerExit);
        PostQuitMessage(0);
      }
    });

    if (options.showAtStart) {
      window.ShowAndFocus();
    }
    if (!options.capturePath.empty()) {
      window.ShowAndFocus();
      SetTimer(window.hwnd(), kTimerCapture, 450, nullptr);
    } else if (options.autoExitMs > 0) {
      SetTimer(window.hwnd(), kTimerExit, static_cast<UINT>(options.autoExitMs), nullptr);
    }

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
      TranslateMessage(&msg);
      DispatchMessageW(&msg);
    }

    if (!options.noHotkey) UnregisterHotkey(window.hwnd(), kHotkeyId);
  }

  if (instanceMutex) {
    ReleaseMutex(instanceMutex);
    CloseHandle(instanceMutex);
    instanceMutex = nullptr;
  }
  LogShutdown();
  return exitCode;
}
