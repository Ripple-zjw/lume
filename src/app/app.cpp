#include "app/app.h"

#include <algorithm>
#include <cmath>
#include <utility>
#include <vector>

#include "app/settings.h"
#include "core/com_ptr.h"
#include "core/log.h"
#include "platform/hotkey.h"
#include "platform/system.h"
#include "search/app_entry.h"
#include "search/file_index.h"
#include "search/merge.h"
#include "ui/icon_cache.h"
#include "ui/renderer.h"
#include "ui/scene.h"
#include "ui/window.h"

namespace {

constexpr int kHotkeyId = 1;
constexpr UINT_PTR kTimerCapture = 0x10;
constexpr UINT_PTR kTimerExit = 0x11;

UINT PanelCanvasWidth(const Theme& theme, float scale) {
  return static_cast<UINT>(std::lround((theme.panelWidth + theme.shadowMargin * 2.0f) * scale));
}

UINT PanelCanvasHeight(const Theme& theme, int visibleRows, float scale) {
  return static_cast<UINT>(
      std::lround((PanelHeight(theme, visibleRows) + theme.shadowMargin * 2.0f) * scale));
}

bool RegisterConfiguredHotkey(HWND hwnd, const AppSettings& settings, int id,
                              std::wstring* usedName) {
  HotkeyBinding desired;
  const bool parsed = ParseHotkey(settings.hotkey, &desired);
  if (parsed && RegisterHotkeyBinding(hwnd, id, desired, nullptr)) {
    if (usedName) *usedName = desired.name;
    return true;
  }
  for (size_t i = 0; i < HotkeyBindingCount(); ++i) {
    const HotkeyBinding* fallback = DefaultHotkeyBinding(i);
    if (parsed && fallback->modifiers == desired.modifiers &&
        fallback->virtualKey == desired.virtualKey) {
      continue;
    }
    if (RegisterHotkeyBinding(hwnd, id, *fallback, nullptr)) {
      if (usedName) *usedName = fallback->name;
      Log(L"已回退到热键: %ls", fallback->name.c_str());
      return true;
    }
  }
  return false;
}

WPARAM KeyTokenToVk(const std::wstring& token) {  if (token == L"up") return VK_UP;
  if (token == L"down") return VK_DOWN;
  if (token == L"left") return VK_LEFT;
  if (token == L"right") return VK_RIGHT;
  if (token == L"home") return VK_HOME;
  if (token == L"end") return VK_END;
  if (token == L"pgup") return VK_PRIOR;
  if (token == L"pgdn") return VK_NEXT;
  if (token == L"enter") return VK_RETURN;
  if (token == L"esc") return VK_ESCAPE;
  if (token == L"back") return VK_BACK;
  if (token == L"delete") return VK_DELETE;
  return 0;
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
  IconCache icons;
  icons.Init(&renderer);
  Scene scene;
  if (!scene.Init(renderer, theme, &icons)) {
    Log(L"场景初始化失败");
    return 2;
  }
  const std::vector<AppEntry> apps = CollectAppEntries();
  FileIndex files;
  if (!options.fileRoot.empty()) files.SetRoots({options.fileRoot});
  files.LoadCachedSnapshot();
  std::vector<ResultRow> rows = BuildMergedRows(apps, &files, nullptr, options.query);
  const int visibleRows = std::clamp(static_cast<int>(rows.size()), 1, Scene::kMaxVisibleRows);
  if (!renderer.AttachOffscreen(PanelCanvasWidth(theme, scale),
                                PanelCanvasHeight(theme, visibleRows, scale))) {
    Log(L"离屏目标创建失败");
    return 2;
  }
  if (!renderer.BeginFrame()) return 2;
  SceneState state;
  state.displayText = options.query;
  state.caretIndex = options.query.size();
  state.rows = std::move(rows);
  state.visibleRows = visibleRows;
  state.selected = options.selected;
  state.selectionRow = static_cast<float>(options.selected);
  state.contentAlpha = 1.0f;
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
    MessageBoxW(nullptr, L"Lume 需要 Windows 11 22H2（build 22621）或更高版本。", L"Lume",
                MB_ICONERROR | MB_OK);
    return 2;
  }

  HANDLE instanceMutex = CreateMutexW(nullptr, TRUE, L"Local\\Lume.Launcher.SingleInstance");
  if (instanceMutex && GetLastError() == ERROR_ALREADY_EXISTS) {
    if (HWND existing = FindWindowW(WindowClassName(), nullptr)) {
      PostMessageW(existing, kMsgToggle, 0, 0);
    }
    CloseHandle(instanceMutex);
    return 0;
  }

  AppSettings settings;
  SettingsStore::Load(&settings);
  if (options.themeSpecified) settings.theme = ThemeModeName(options.themeMode);
  if (options.accentSpecified) settings.accent = options.accent;
  if (!options.hotkeyOverride.empty()) settings.hotkey = options.hotkeyOverride;
  if (options.indexSizeMB > 0) settings.indexSizeMB = static_cast<int>(options.indexSizeMB);
  Log(L"设置: theme=%ls accent=\"%ls\" hotkey=%ls autostart=%d roots=%zu", settings.theme.c_str(),
      settings.accent.c_str(), settings.hotkey.c_str(), settings.autostart ? 1 : 0,
      settings.fileRoots.size());

  Renderer renderer;
  IconCache icons;
  Scene scene;
  LauncherWindow window;
  int exitCode = 0;

  Theme theme = MakeTheme(ParseThemeMode(settings.theme), ParseAccent(settings.accent));
  if (!renderer.Init()) {
    Log(L"渲染器初始化失败");
    exitCode = 2;
  } else if (!icons.Init(&renderer)) {
    Log(L"图标缓存初始化失败");
    exitCode = 2;
  } else if (!scene.Init(renderer, theme, &icons)) {
    Log(L"场景初始化失败");
    exitCode = 2;
  } else if (!window.Create(GetModuleHandleW(nullptr), &renderer, &scene, &icons, settings,
                            options.fileRoot)) {
    Log(L"窗口创建失败");
    exitCode = 2;
  }

  if (exitCode == 0) {
    window.SetGlass(options.glass);
    window.SetQuery(options.query);
    window.RenderNow();  // 隐藏预热：把首次绘制的着色器/布局成本挪到启动阶段

    std::wstring hotkeyName;
    if (!options.noHotkey &&
        !RegisterConfiguredHotkey(window.hwnd(), settings, kHotkeyId, &hotkeyName)) {
      Log(L"没有可用的全局热键，仅能通过 --show 测试");
    }

    window.SetOnHotkey([&window]() { window.Toggle(); });
    window.SetOnHotkeyRequest([&](const AppSettings& updated) {
      if (options.noHotkey) return;
      UnregisterHotkey(window.hwnd(), kHotkeyId);
      std::wstring used;
      if (!RegisterConfiguredHotkey(window.hwnd(), updated, kHotkeyId, &used)) {
        Log(L"新热键注册失败");
      }
    });
    window.SetOnSettingsChanged(
        [](const AppSettings& updated) { SettingsStore::Save(updated); });
    if (options.openSettings) {
      window.OpenSettings();
      if (!options.testCapture.empty()) {
        window.TestHotkeyCapture(options.testCapture);
      }
    }
    if (options.clearIndexOnStart) {
      window.ClearFileIndex();
    }
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
      SetTimer(window.hwnd(), kTimerCapture, static_cast<UINT>(std::max(10, options.captureDelayMs)),
               nullptr);
    } else if (options.autoExitMs > 0) {
      SetTimer(window.hwnd(), kTimerExit, static_cast<UINT>(options.autoExitMs), nullptr);
    }
    if (!options.compose.empty()) {
      window.SimulateComposition(options.compose, options.compose.size());
    }
    if (!options.type.empty() || !options.keys.empty()) {
      for (wchar_t ch : options.type) {
        PostMessageW(window.hwnd(), WM_CHAR, static_cast<WPARAM>(ch), 1);
      }
      size_t start = 0;
      while (start <= options.keys.size()) {
        const size_t comma = options.keys.find(L',', start);
        const std::wstring token =
            options.keys.substr(start, comma == std::wstring::npos ? std::wstring::npos
                                                                  : comma - start);
        const WPARAM key = KeyTokenToVk(token);
        if (key != 0) PostMessageW(window.hwnd(), WM_KEYDOWN, key, 0);
        if (comma == std::wstring::npos) break;
        start = comma + 1;
      }
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
  window.Shutdown();
  icons.Shutdown();
  LogShutdown();
  return exitCode;
}
