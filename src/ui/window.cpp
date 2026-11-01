#include "ui/window.h"

#include <dwmapi.h>
#include <imm.h>
#include <shellapi.h>
#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <utility>

#include "app/version.h"
#include "core/log.h"
#include "launch/launcher.h"
#include "platform/autostart.h"
#include "platform/hotkey.h"
#include "platform/system.h"
#include "search/merge.h"
#include "search/usage.h"
#include "ui/renderer.h"

#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif
#ifndef DWMWA_WINDOW_CORNER_PREFERENCE
#define DWMWA_WINDOW_CORNER_PREFERENCE 33
#endif
#ifndef DWMWA_SYSTEMBACKDROP_TYPE
#define DWMWA_SYSTEMBACKDROP_TYPE 38
#endif
// 注意 DWMSBT_NONE 是 1 不是 0（0 = DWMSBT_AUTO，交给 DWM 自己挑，等于没关）。
// 2 = DWMSBT_MAINWINDOW（Mica），P6.5 起刻意不用，原因见 ApplyBackdrop()。
#ifndef DWMSBT_NONE
#define DWMSBT_NONE 1
#endif
#ifndef DWMWCP_DONOTROUND
#define DWMWCP_DONOTROUND 1
#endif
// 窗口描边颜色：DWMWA_COLOR_NONE 让 DWM 不画那圈 1px 边框。
#ifndef DWMWA_BORDER_COLOR
#define DWMWA_BORDER_COLOR 34
#endif
#ifndef DWMWA_COLOR_NONE
#define DWMWA_COLOR_NONE 0xFFFFFFFE
#endif

namespace {

constexpr wchar_t kClassName[] = L"LumeLauncherWindow";
constexpr UINT_PTR kTimerCaret = 0x20;
constexpr UINT_PTR kTimerAnim = 0x21;
constexpr UINT_PTR kTimerFileRefresh = 0x22;
constexpr UINT_PTR kTimerIcons = 0x23;
constexpr UINT_PTR kTimerTrayRetry = 0x24;
constexpr UINT kCaretBlinkMs = 530;
// 托盘左键去抖：一次点击 shell 会送来两条通知（NIN_SELECT + WM_LBUTTONUP），窗口只处理一次。
constexpr uint64_t kTrayClickDebounceMs = 400;
// 托盘点开的这一下，shell 收尾时会把前台抢回 taskbar；这段时间内由 shell 触发的失活不作数。
constexpr uint64_t kTrayDeactivateGraceMs = 600;
constexpr double kShowAnimMs = 90.0;
constexpr double kSelectionAnimMs = 120.0;
constexpr double kContentAnimMs = 80.0;

UINT TaskbarCreatedMessage() {
  static UINT message = RegisterWindowMessageW(L"TaskbarCreated");
  return message;
}

double ElapsedMs(const LARGE_INTEGER& start) {
  LARGE_INTEGER now{};
  LARGE_INTEGER frequency{};
  QueryPerformanceCounter(&now);
  QueryPerformanceFrequency(&frequency);
  return static_cast<double>(now.QuadPart - start.QuadPart) * 1000.0 /
         static_cast<double>(frequency.QuadPart);
}

double Progress(const LARGE_INTEGER& start, double durationMs) {
  const double t = ElapsedMs(start) / durationMs;
  return t < 0.0 ? 0.0 : (t > 1.0 ? 1.0 : t);
}

// 是不是 shell 自己的界面（任务栏 / 托盘溢出区 / XAML 托管的溢出面板）。
// 托盘左键处理过程中前台会短暂落到这些窗口上，用来区分「用户真的去点别处了」和「点击收尾」。
bool IsShellSurface(HWND window) {
  if (!window) return false;
  wchar_t name[64] = {};
  if (!GetClassNameW(window, name, ARRAYSIZE(name))) return false;
  return wcscmp(name, L"Shell_TrayWnd") == 0 || wcscmp(name, L"Shell_SecondaryTrayWnd") == 0 ||
         wcscmp(name, L"NotifyIconOverflowWindow") == 0 ||
         wcscmp(name, L"TopLevelWindowForOverflowXamlIsland") == 0;
}

float EaseOut(double t) {
  const float clamped = static_cast<float>(t < 0.0 ? 0.0 : (t > 1.0 ? 1.0 : t));
  const float inverse = 1.0f - clamped;
  return 1.0f - inverse * inverse;
}

}  // namespace

const wchar_t* WindowClassName() { return kClassName; }

LauncherWindow::~LauncherWindow() {
  if (hwnd_) {
    DestroyWindow(hwnd_);
    hwnd_ = nullptr;
  }
}

bool LauncherWindow::Create(HINSTANCE instance, Renderer* renderer, Scene* scene,
                            IconCache* icons, const AppSettings& settings,
                            const std::wstring& fileRoot) {
  instance_ = instance;
  renderer_ = renderer;
  scene_ = scene;
  icons_ = icons;
  settings_ = settings;
  theme_ = MakeTheme(ParseThemeMode(settings_.theme), ParseAccent(settings_.accent));
  visibleRows_ = Scene::kMaxVisibleRows;
  input_.SetText(L"");
  usage_.Load();
  files_.SetMaxIndexBytes(static_cast<size_t>(std::clamp(settings_.indexSizeMB, 64, 262144)) *
                          1024ull * 1024ull);
  files_.SetIndexDirectory(settings_.indexDir);
  Log(L"索引目录: %ls", files_.indexDirectory().c_str());
  if (!fileRoot.empty()) {
    files_.SetRoots({fileRoot});
  } else if (!settings_.fileRoots.empty()) {
    files_.SetRoots(settings_.fileRoots);
  }

  LARGE_INTEGER indexStart{};
  QueryPerformanceCounter(&indexStart);
  apps_ = CollectAppEntries();
  Log(L"应用索引耗时: %.1f ms", ElapsedMs(indexStart));
  Log(L"文件搜索来源: %ls", FileSearchIsEverything() ? L"everything" : L"local");
  if (!FileSearchIsEverything()) {
    files_.Start();
    filesRunning_ = true;
    fileVersionSeen_ = files_.version();
  }
  RefreshResults();  // Everything 模式此时还没有窗口可收回复，先只出应用结果
  PreloadIcons(true);  // 启动时同步预热首屏图标，避免第一次唤出看到字块

  // 按光标所在显示器创建，避免首次显示时因 DPI 变化重建交换链。
  POINT initialCursor{};
  GetCursorPos(&initialCursor);
  ApplyDpi(DpiForPoint(initialCursor));

  WNDCLASSEXW wc{};
  wc.cbSize = sizeof(wc);
  wc.lpfnWndProc = Thunk;
  wc.hInstance = instance;
  wc.lpszClassName = kClassName;
  wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  wc.hIcon = static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(1), IMAGE_ICON,
                                          GetSystemMetrics(SM_CXICON),
                                          GetSystemMetrics(SM_CYICON), LR_DEFAULTCOLOR));
  wc.hIconSm = static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(1), IMAGE_ICON,
                                            GetSystemMetrics(SM_CXSMICON),
                                            GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR));
  wc.style = CS_HREDRAW | CS_VREDRAW;
  if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
    LogError(L"RegisterClassExW 失败: %lu", GetLastError());
    return false;
  }

  hwnd_ = CreateWindowExW(
      WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOREDIRECTIONBITMAP, kClassName, L"Lume", WS_POPUP,
      0, 0, static_cast<int>(widthPx_), static_cast<int>(heightPx_), nullptr, nullptr, instance,
      this);
  if (!hwnd_) {
    LogError(L"CreateWindowExW 失败: %lu", GetLastError());
    return false;
  }
  if (!renderer_->AttachWindow(hwnd_, widthPx_, heightPx_)) {
    LogError(L"渲染目标绑定失败");
    return false;
  }
  ApplyBackdrop();
  SetTimer(hwnd_, kTimerFileRefresh, 1000, nullptr);
  if (settings_.tray && !tray_.Create(hwnd_)) {
    LogWarn(L"托盘初始化失败（继续运行），2 秒后重试");
    trayRetries_ = 0;
    SetTimer(hwnd_, kTimerTrayRetry, 2000, nullptr);
  }
  if (FileSearchIsEverything()) {
    everything_.Attach(hwnd_);
    // 上次切到 Everything 但没连上时留下的快照，这里再试一次。
    TryDeleteSnapshotForEverything();
    RefreshResults();  // 现在能收回复了：补一次真实查询
  }
  return true;
}

void LauncherWindow::Shutdown() {
  if (hwnd_) {
    KillTimer(hwnd_, kTimerCaret);
    KillTimer(hwnd_, kTimerAnim);
    KillTimer(hwnd_, kTimerFileRefresh);
    KillTimer(hwnd_, kTimerIcons);
    KillTimer(hwnd_, kTimerTrayRetry);
  }
  usage_.Save();
  settingsWindow_.reset();
  tray_.Destroy();
  everything_.Detach();
  files_.Stop();
  filesRunning_ = false;
}

void LauncherWindow::SetTheme(const Theme& theme) {
  theme_ = theme;
  if (scene_) scene_->SetTheme(theme);
  ApplyBackdrop();
  UpdateSceneState();
  if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
}

void LauncherWindow::SetQuery(const std::wstring& query) {
  input_.SetText(query);
  contentAlpha_ = 1.0f;
  RefreshResults();
  if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
}

void LauncherWindow::SetGlass(bool glass) {
  glass_ = glass;
  UpdateSceneState();
  if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
}

void LauncherWindow::SimulateComposition(const std::wstring& composition, size_t caretInComposition) {
  input_.SetComposition(composition, caretInComposition);
  UpdateSceneState();
  UpdateImeWindow();
  if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
}

void LauncherWindow::SetOnHotkey(std::function<void()> callback) {
  onHotkey_ = std::move(callback);
}

void LauncherWindow::SetOnTimer(std::function<void(UINT_PTR)> callback) {
  onTimer_ = std::move(callback);
}

void LauncherWindow::ApplyDpi(UINT dpi) {
  dpi_ = dpi < 48 ? 96 : dpi;
  const float scale = static_cast<float>(dpi_) / 96.0f;
  const float panelHeight = PanelHeight(theme_, visibleRows_, settings_.filters.enabled);
  widthPx_ = static_cast<UINT>(std::lround((theme_.panelWidth + theme_.shadowMargin * 2.0f) * scale));
  heightPx_ =
      static_cast<UINT>(std::lround((panelHeight + theme_.shadowMargin * 2.0f) * scale));
}

void LauncherWindow::ApplyBackdrop() {
  if (!hwnd_) return;
  const BOOL dark = theme_.dark ? TRUE : FALSE;
  const HRESULT darkHr = DwmSetWindowAttribute(hwnd_, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark,
                                               sizeof(dark));
  if (FAILED(darkHr)) LogHr(L"DwmSetWindowAttribute(dark)", darkHr, LogLevel::Warn);
  // 不能开系统背景（Mica）。它填满的是**整个窗口矩形**（暗色下是 #202020），而我们的窗口比面板
  // 大整整一圈 shadowMargin：于是面板外面那圈透明区域被 Mica 涂成一个不透明的方框，
  // 看上去就是「搜索框外面又套了一个框」。实测（窗口背后垫一块纯品红）：带 Mica 时窗口边缘那一圈
  // 是 (32,32,32) 的死色（品红完全透不出来），关掉后同一圈变成品红被自绘阴影压暗的渐变。
  const int backdrop = DWMSBT_NONE;
  const HRESULT backdropHr =
      DwmSetWindowAttribute(hwnd_, DWMWA_SYSTEMBACKDROP_TYPE, &backdrop, sizeof(backdrop));
  if (FAILED(backdropHr)) LogHr(L"DwmSetWindowAttribute(backdrop)", backdropHr, LogLevel::Warn);
  // 也不要 DWM 给窗口裁圆角（DWMWCP_ROUND）。它的半径是系统定的（约 8–10px），比面板的
  // theme.corner（16 DIP）小得多；裁剪会沿窗口矩形切掉自绘阴影的四角，看起来就像外框的圆角
  // 「比面板方」。窗口本身没有填充，形状全靠自绘的面板 + 阴影，所以不裁圆角没有任何副作用。
  const int corner = DWMWCP_DONOTROUND;
  DwmSetWindowAttribute(hwnd_, DWMWA_WINDOW_CORNER_PREFERENCE, &corner, sizeof(corner));
  // 系统背景还会沿窗口矩形画一圈 1px 描边，同样多余（阴影是我们自己画的）。
  const COLORREF borderNone = static_cast<COLORREF>(DWMWA_COLOR_NONE);
  const HRESULT borderHr =
      DwmSetWindowAttribute(hwnd_, DWMWA_BORDER_COLOR, &borderNone, sizeof(borderNone));
  if (FAILED(borderHr)) LogHr(L"DwmSetWindowAttribute(border)", borderHr, LogLevel::Warn);
}

void LauncherWindow::PositionOnCursorMonitor() {
  POINT cursor{};
  GetCursorPos(&cursor);
  const RECT work = WorkAreaForPoint(cursor);
  const UINT dpi = DpiForPoint(cursor);
  if (dpi != dpi_) {
    ApplyDpi(dpi);
    if (!renderer_->Resize(widthPx_, heightPx_)) {
      LogError(L"窗口尺寸切换失败 (dpi=%u)", dpi);
    }
  }
  const int workWidth = work.right - work.left;
  const int workHeight = work.bottom - work.top;
  const float scale = static_cast<float>(dpi_) / 96.0f;
  const int marginPx = static_cast<int>(std::lround(theme_.shadowMargin * scale));
  const int x = work.left + (workWidth - static_cast<int>(widthPx_)) / 2;
  int y = work.top + static_cast<int>(std::lround(workHeight * 0.18f)) - marginPx;
  const int minY = work.top - marginPx + 8;
  const int maxY = work.bottom - static_cast<int>(heightPx_) + marginPx - 8;
  if (y < minY) y = minY;
  if (y > maxY) y = maxY;
  SetWindowPos(hwnd_, HWND_TOPMOST, x, y, static_cast<int>(widthPx_), static_cast<int>(heightPx_),
               SWP_NOACTIVATE);
}

void LauncherWindow::PreloadIcons(bool warm) {
  if (!icons_) return;
  const size_t count = std::min(rows_.size(), static_cast<size_t>(visibleRows_));
  for (size_t i = 0; i < count; ++i) {
    if (rows_[i].iconKey.empty()) continue;
    if (warm) {
      icons_->Warm(rows_[i].iconKey);
    } else {
      icons_->Get(rows_[i].iconKey);
    }
  }
}

void LauncherWindow::UpdateLayoutForRows() {
  const int wanted = std::clamp(static_cast<int>(rows_.size()), 0, Scene::kMaxVisibleRows);
  if (wanted == visibleRows_) return;
  visibleRows_ = wanted;
  const float scale = static_cast<float>(dpi_) / 96.0f;
  heightPx_ = static_cast<UINT>(
      std::lround((PanelHeight(theme_, visibleRows_, settings_.filters.enabled) +
                   theme_.shadowMargin * 2.0f) *
                  scale));
  if (renderer_ && renderer_->kind() != RenderTargetKind::None) {
    renderer_->Resize(widthPx_, heightPx_);
  }
  if (hwnd_) {
    RECT rect{};
    GetWindowRect(hwnd_, &rect);
    SetWindowPos(hwnd_, HWND_TOPMOST, rect.left, rect.top, static_cast<int>(widthPx_),
                 static_cast<int>(heightPx_), SWP_NOACTIVATE);
  }
}

void LauncherWindow::OpenSettings() {
  if (!settingsWindow_) settingsWindow_ = std::make_unique<SettingsWindow>();
  SettingsWindow::Callbacks callbacks;
  callbacks.onSave = [this](const AppSettings& updated) {
    ApplySettings(updated);
    if (onHotkeyRequest_) onHotkeyRequest_(updated);
    if (onSettingsChanged_) onSettingsChanged_(updated);
  };
  callbacks.queryIndex = [this]() { return QueryIndexStatus(); };
  callbacks.queryEverything = [this]() { return QueryEverythingStatus(); };
  callbacks.queryAbout = [this]() { return QueryAboutInfo(); };
  callbacks.onClearIndex = [this]() { ClearFileIndex(); };
  callbacks.onRebuildIndex = [this]() { RebuildFileIndex(); };
  callbacks.onOpenIndexDirectory = [this]() { OpenIndexDirectory(); };
  callbacks.onOpenLog = [this]() { return OpenLogFile(); };
  settingsWindow_->Show(instance_, hwnd_, settings_, std::move(callbacks));
}

IndexStatus LauncherWindow::QueryIndexStatus() const {
  IndexStatus status;
  status.fileSearch = FileSearchIsEverything() ? L"everything" : L"local";
  status.directory = files_.indexDirectory();
  status.defaultDirectory = FileIndex::DefaultIndexDirectory();
  status.snapshotPath = files_.snapshotPath();
  status.snapshotBytes = files_.snapshotBytes();
  status.entries = files_.size();
  status.lastRebuildDurationMs = files_.lastRebuildDurationMs();
  status.limitMB = settings_.indexSizeMB;
  status.rebuilding = files_.rebuilding();
  status.snapshotWritable = files_.snapshotWritable();
  if (FileSearchIsEverything()) {
    const EverythingStatus everything = everything_.Status();
    status.evAvailable = everything.available;
    status.evDbLoaded = everything.dbLoaded;
    status.evVersion = everything.version;
    status.evError = everything.error;
  }
  return status;
}

void LauncherWindow::ClearFileIndex() {
  if (FileSearchIsEverything()) {
    Log(L"当前文件搜索来源是 Everything，本地索引操作已跳过");
    return;
  }
  Log(L"用户手动清除文件索引（不重建）");
  files_.ClearIndex();
  RefreshResults();
  StartContentFade();
  if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
}

void LauncherWindow::RebuildFileIndex() {
  if (FileSearchIsEverything()) {
    Log(L"当前文件搜索来源是 Everything，本地索引操作已跳过");
    return;
  }
  Log(L"用户手动重建文件索引");
  files_.RebuildIndex();
}

void LauncherWindow::OpenIndexDirectory() {
  if (FileSearchIsEverything()) {
    Log(L"当前文件搜索来源是 Everything，没有本地索引目录");
    return;
  }
  std::wstring directory = files_.indexDirectory();
  if (directory.empty()) {
    LogWarn(L"打开索引目录失败: 目录为空");
    return;
  }
  if (!EnsureDirectory(directory)) {
    LogError(L"打开索引目录失败: 无法创建 %ls", directory.c_str());
    return;
  }
  std::wstring error;
  if (!OpenPath(directory, &error)) {
    LogError(L"打开索引目录失败: %ls", error.c_str());
  }
}

bool LauncherWindow::OpenLogFile() {
  const std::wstring path = LogPath();
  if (path.empty()) {
    LogWarn(L"打开日志失败: 还没有日志文件");
    return false;
  }
  std::wstring error;
  if (OpenPath(path, &error)) {
    Log(L"打开日志: %ls", path.c_str());
    return true;
  }
  // 文件还不存在（或没有关联程序）时退回打开所在目录。
  const size_t slash = path.find_last_of(L"\\/");
  if (slash != std::wstring::npos) {
    const std::wstring directory = path.substr(0, slash);
    if (OpenPath(directory, &error)) {
      Log(L"打开日志目录: %ls", directory.c_str());
      return true;
    }
  }
  LogError(L"打开日志失败: %ls", error.c_str());
  return false;
}

void LauncherWindow::TestHotkeyCapture(const std::wstring& combo) {
  if (!settingsWindow_ || !settingsWindow_->visible()) {
    Log(L"热键捕获测试: 设置窗未打开");
    return;
  }
  HWND settings = settingsWindow_->hwnd();
  HWND capture = FindWindowExW(settings, nullptr, L"LumeHotkeyCapture", nullptr);
  if (!capture) {
    Log(L"热键捕获测试: 找不到捕获框");
    return;
  }
  HotkeyBinding binding;
  if (!ParseHotkey(combo, &binding)) {
    Log(L"热键捕获测试: 无法解析 %ls", combo.c_str());
    return;
  }

  SetForegroundWindow(settings);
  SetFocus(capture);
  Sleep(120);  // 等 WM_SETFOCUS 安装低级键盘钩子

  std::vector<INPUT> inputs;
  auto addKey = [&](UINT virtualKey, bool up) {
    INPUT input{};
    input.type = INPUT_KEYBOARD;
    input.ki.wVk = static_cast<WORD>(virtualKey);
    if (up) input.ki.dwFlags = KEYEVENTF_KEYUP;
    inputs.push_back(input);
  };
  if (binding.modifiers & MOD_CONTROL) addKey(VK_CONTROL, false);
  if (binding.modifiers & MOD_ALT) addKey(VK_MENU, false);
  if (binding.modifiers & MOD_SHIFT) addKey(VK_SHIFT, false);
  if (binding.modifiers & MOD_WIN) addKey(VK_LWIN, false);
  addKey(binding.virtualKey, false);
  addKey(binding.virtualKey, true);
  if (binding.modifiers & MOD_WIN) addKey(VK_LWIN, true);
  if (binding.modifiers & MOD_SHIFT) addKey(VK_SHIFT, true);
  if (binding.modifiers & MOD_ALT) addKey(VK_MENU, true);
  if (binding.modifiers & MOD_CONTROL) addKey(VK_CONTROL, true);
  SendInput(static_cast<UINT>(inputs.size()), inputs.data(), sizeof(INPUT));

  // 抽送消息，让钩子投递的捕获消息被处理。
  const DWORD deadline = GetTickCount() + 400;
  MSG message{};
  while (GetTickCount() < deadline) {
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
      TranslateMessage(&message);
      DispatchMessageW(&message);
    }
    Sleep(10);
  }

  wchar_t text[128] = {};
  GetWindowTextW(capture, text, ARRAYSIZE(text));
  Log(L"热键捕获测试: 合成 %ls -> 捕获框 \"%ls\"", combo.c_str(), text);
}

void LauncherWindow::TryDeleteSnapshotForEverything() {
  if (!FileSearchIsEverything()) return;
  if (files_.snapshotBytes() == 0) {
    pendingSnapshotDelete_ = false;  // 本来就没有快照
    return;
  }
  const EverythingStatus status = everything_.Status();
  if (!status.available || !status.dbLoaded) {
    pendingSnapshotDelete_ = true;
    LogWarn(L"文件搜索来源: Everything 不可用（%ls）；本地快照暂留", status.error.c_str());
    return;
  }
  const uint64_t bytes = files_.snapshotBytes();
  files_.ClearIndex();
  pendingSnapshotDelete_ = false;
  Log(L"文件搜索来源: 已切到 Everything，删除本地快照 %.1f MB",
      static_cast<double>(bytes) / (1024.0 * 1024.0));
}

void LauncherWindow::ApplySettings(const AppSettings& settings) {
  const bool wasEverything = FileSearchIsEverything();
  settings_ = settings;
  // 日志等级随设置即时生效：设置窗里勾/不勾「详细日志」，不用重启进程。
  const LogLevel wanted = LogLevelFromText(settings_.logLevel);
  if (wanted != LogLevelCurrent()) {
    Log(L"日志等级: %ls -> %ls（来自设置）", LogLevelName(LogLevelCurrent()), LogLevelName(wanted));
    LogSetLevel(wanted);
  }
  // 组被删/改过之后 active 可能越界：收敛到合法范围再查询。
  ClampFilterActive(&settings_.filters);
  SetTheme(MakeTheme(ParseThemeMode(settings_.theme), ParseAccent(settings_.accent)));
  std::wstring error;
  if (!SetAutostart(settings_.autostart, &error)) {
    LogError(L"应用自启设置失败: %ls", error.c_str());
  }

  if (FileSearchIsEverything()) {
    // Everything 模式：本地索引线程全部停掉，内存索引清空。
    everything_.Attach(hwnd_);
    if (filesRunning_) {
      files_.Stop();
      filesRunning_ = false;
    }
    if (!wasEverything) {
      queryGeneration_ = 0;
      fileHits_.clear();
    }
    // 只有真的连上 Everything 才删快照，避免切过去后既没有 Everything 也没有本地索引。
    if (!wasEverything || pendingSnapshotDelete_) TryDeleteSnapshotForEverything();
  } else {
    everything_.Detach();
    if (filesRunning_) {
      files_.Stop();
      filesRunning_ = false;
    }
    files_.SetMaxIndexBytes(static_cast<size_t>(std::clamp(settings_.indexSizeMB, 64, 262144)) *
                            1024ull * 1024ull);
    files_.SetIndexDirectory(settings_.indexDir);
    files_.SetRoots(settings_.fileRoots);
    files_.Start();
    filesRunning_ = true;
    fileVersionSeen_ = files_.version();
    pendingSnapshotDelete_ = false;
    if (wasEverything) {
      fileHits_.clear();
      Log(L"文件搜索来源: 已切回本地索引（无快照时后台重建）");
    }
  }

  Log(L"索引目录: %ls", files_.indexDirectory().c_str());
  RefreshResults();
  StartContentFade();
  if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
}

void LauncherWindow::HandleTrayCommand(UINT command) {
  switch (command) {
    case kTrayShow:
      ShowAndFocus();
      break;
    case kTrayAutostart: {
      settings_.autostart = !settings_.autostart;
      std::wstring error;
      if (!SetAutostart(settings_.autostart, &error)) {
        LogError(L"切换自启失败: %ls", error.c_str());
      }
      if (onSettingsChanged_) onSettingsChanged_(settings_);
      break;
    }
    case kTraySettings:
      OpenSettings();
      break;
    case kTrayExit:
      Log(L"托盘退出");
      PostQuitMessage(0);
      break;
    default:
      break;
  }
}

void LauncherWindow::SetOnSettingsChanged(std::function<void(const AppSettings&)> callback) {
  onSettingsChanged_ = std::move(callback);
}

void LauncherWindow::SetOnHotkeyRequest(std::function<void(const AppSettings&)> callback) {
  onHotkeyRequest_ = std::move(callback);
}

bool LauncherWindow::FileSearchIsEverything() const {
  return _wcsicmp(settings_.fileSearch.c_str(), L"everything") == 0;
}

EverythingStatus LauncherWindow::QueryEverythingStatus() const {
  return everything_.Status();
}

AboutInfo LauncherWindow::QueryAboutInfo() const {
  AboutInfo info;
  info.version = kAppVersion;
  const OsVersion os = QueryOsVersion();
  // Windows 11 从 build 22000 起；本程序又要求 22621+，这里只做展示用。
  wchar_t osText[96] = {};
  swprintf(osText, ARRAYSIZE(osText), L"Windows %ls（build %lu）",
           os.build >= 22000 ? L"11" : L"10", os.build);
  info.os = osText;
  info.arch = L"x64";
  wchar_t dpi[64] = {};
  swprintf(dpi, ARRAYSIZE(dpi), L"%u (%u%%)", dpi_, dpi_ * 100u / 96u);
  info.dpi = dpi;
  info.hotkey = settings_.hotkey;
  info.settingsPath = SettingsStore::Path();
  info.usagePath = UsageStore::DefaultPath();
  info.logPath = LogPath();
  info.logLevel = LogLevelName(LogLevelCurrent());
  info.indexDir = files_.indexDirectory();
  info.fileSearch = FileSearchIsEverything() ? L"everything" : L"local";
  if (FileSearchIsEverything()) {
    const EverythingStatus status = everything_.Status();
    if (!status.available) {
      info.everything = status.error.empty() ? L"未运行" : status.error;
    } else {
      info.everything = status.version + (status.dbLoaded ? L"（已连接，索引就绪）"
                                                          : L"（已连接，索引未就绪）");
    }
  }
  return info;
}

void LauncherWindow::RebuildRows() {
  rows_ = BuildMergedRows(apps_, fileHits_, &usage_, input_.text(), 200, true,
                          CurrentFilterGroup());
  selected_ = 0;
  scroll_ = 0;
  selectionRow_ = 0.0f;
  hoverRow_ = -1;  // 行集合换了，旧的悬停序号不再有效
  pressedRow_ = -1;
  UpdateLayoutForRows();
  EnsureSelectionVisible();
  PreloadIcons(false);
  UpdateSceneState();
}

const FilterGroup* LauncherWindow::CurrentFilterGroup() const {
  return ActiveFilterGroup(settings_.filters);
}

void LauncherWindow::SelectFilter(int index) {
  if (!settings_.filters.enabled) return;
  const int chips = static_cast<int>(settings_.filters.groups.size()) + 1;
  if (index < 0 || index >= chips) return;
  if (index == settings_.filters.active) return;
  settings_.filters.active = index;
  const std::wstring label = FilterChipLabels(settings_.filters)[static_cast<size_t>(index)];
  LARGE_INTEGER start{};
  QueryPerformanceCounter(&start);
  RefreshResults();
  StartContentFade();
  hoverChip_ = -1;
  hoverRow_ = -1;
  pressedRow_ = -1;
  UpdateSceneState();
  if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
  Log(L"筛选: %ls（第 %d 组）-> %zu 条 (%.1f ms)", label.c_str(), index + 1, rows_.size(),
      ElapsedMs(start));
  // 记住上次选中的组（settings.conf 的 filterActive=）；只有切组时才会写这一次盘。
  if (onSettingsChanged_) onSettingsChanged_(settings_);
}

void LauncherWindow::CycleFilter(int delta) {
  const int chips = static_cast<int>(settings_.filters.groups.size()) + 1;
  if (chips <= 1) return;
  const int current = std::clamp(settings_.filters.active, 0, chips - 1);
  SelectFilter(((current + delta) % chips + chips) % chips);
}

void LauncherWindow::RefreshResults() {
  UpdateFileHits();
  RebuildRows();
}

void LauncherWindow::UpdateFileHits() {
  const std::wstring& query = input_.text();
  const FilterGroup* filter = CurrentFilterGroup();
  if (!FileSearchIsEverything()) {
    fileHits_ = files_.Search(query, kMaxFileHits, filter);
    return;
  }
  if (query.empty()) {
    fileHits_.clear();
    return;
  }
  if (input_.composing()) return;  // 组字期间不去打扰 Everything
  RequestEverythingRows();
}

void LauncherWindow::RequestEverythingRows() {
  if (!hwnd_) return;
  if (handlingEverythingReply_) {
    // 防御：正在处理 Everything 的回复（它阻塞在投递上），此时不能再发查询，改到消息循环里发。
    PostMessageW(hwnd_, kMsgEverythingRetry, 0, 0);
    return;
  }
  const uint32_t generation = ++queryGeneration_;
  const FilterGroup* filter = CurrentFilterGroup();
  // 同一次「查询 + 组」如果已经因为 ext: 下推拿过 0 条，就不再下推（见 WM_COPYDATA 里的重试）。
  const std::wstring key = input_.text() + L'\n' + (filter ? filter->name : L"");
  const bool allowPushdown = (key != pushdownRetryKey_);
  LARGE_INTEGER sendStart{};
  QueryPerformanceCounter(&sendStart);
  const bool sent = everything_.Request(input_.text(), generation, filter, allowPushdown);
  const double sendMs = ElapsedMs(sendStart);
  QueryPerformanceCounter(&everythingRequestedAt_);  // 往返计时从「发完」开始
  if (sent) {
    // 之前因 Everything 不可用而留下的快照，现在可以补删了。
    if (pendingSnapshotDelete_) TryDeleteSnapshotForEverything();
    if (sendMs >= 5.0) {
      LogWarn(L"Everything 发送偏慢: \"%ls\" %.1f ms", input_.text().c_str(), sendMs);
    }
    return;
  }
  // 失败只做限频日志（每次按键都会走到这里）。
  const uint64_t now = GetTickCount64();
  if (now - lastEverythingErrorLogAt_ < 10000) return;
  lastEverythingErrorLogAt_ = now;
  LogWarn(L"Everything 不可用: %ls", everything_.Status().error.c_str());
}

void LauncherWindow::Refilter() {
  LARGE_INTEGER start{};
  QueryPerformanceCounter(&start);
  RefreshResults();
  StartContentFade();
  UpdateSceneState();
  if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
  LogDebug(L"过滤: \"%ls\" -> %zu 条 (%.1f ms)", input_.text().c_str(), rows_.size(),
           ElapsedMs(start));
}

void LauncherWindow::UpdateSceneState() {
  sceneState_.displayText = input_.DisplayText();
  sceneState_.caretIndex = input_.DisplayCaret();
  sceneState_.compositionStart = input_.composing() ? input_.compositionStart() : 0;
  sceneState_.compositionLength = input_.composing() ? input_.composition().size() : 0;
  sceneState_.caretVisible = caretVisible_;
  sceneState_.rows = rows_;
  sceneState_.chips.clear();
  if (settings_.filters.enabled) {
    const std::vector<std::wstring> labels = FilterChipLabels(settings_.filters);
    const int active = std::clamp(settings_.filters.active, 0, static_cast<int>(labels.size()) - 1);
    for (size_t i = 0; i < labels.size(); ++i) {
      ChipState chip;
      chip.label = labels[i];
      chip.active = static_cast<int>(i) == active;
      sceneState_.chips.push_back(std::move(chip));
    }
  }
  sceneState_.hoverChip = hoverChip_;
  sceneState_.hoverRow = hoverRow_;
  sceneState_.selected = selected_;
  sceneState_.scroll = scroll_;
  sceneState_.visibleRows = visibleRows_;
  sceneState_.selectionRow = selectionRow_;
  sceneState_.contentAlpha = contentAlpha_;
  sceneState_.globalAlpha = showAlpha_;
  sceneState_.glass = glass_;
}

void LauncherWindow::UpdateImeWindow() {
  if (!hwnd_ || !scene_) return;
  HIMC context = ImmGetContext(hwnd_);
  if (!context) return;
  RECT windowRect{};
  GetWindowRect(hwnd_, &windowRect);
  const float scale = static_cast<float>(dpi_) / 96.0f;
  const float caretDip = scene_->CaretX(sceneState_);
  POINT point{};
  point.x = windowRect.left +
            static_cast<LONG>(std::lround((theme_.shadowMargin + caretDip) * scale));
  point.y = windowRect.top +
            static_cast<LONG>(std::lround((theme_.shadowMargin + 20.0f) * scale));
  COMPOSITIONFORM form{};
  form.dwStyle = CFS_POINT;
  form.ptCurrentPos = point;
  ImmSetCompositionWindow(context, &form);

  LOGFONTW font{};
  font.lfHeight = -static_cast<LONG>(std::lround(theme_.queryFontSize * scale));
  font.lfWeight = FW_SEMIBOLD;
  lstrcpynW(font.lfFaceName, theme_.fontFamily, ARRAYSIZE(font.lfFaceName));
  ImmSetCompositionFontW(context, &font);
  ImmReleaseContext(hwnd_, context);
}

void LauncherWindow::EnsureSelectionVisible() {
  if (selected_ < scroll_) scroll_ = selected_;
  if (selected_ >= scroll_ + visibleRows_) scroll_ = selected_ - visibleRows_ + 1;
  const int maxScroll = std::max(0, static_cast<int>(rows_.size()) - visibleRows_);
  scroll_ = std::clamp(scroll_, 0, maxScroll);
}

void LauncherWindow::MoveSelection(int delta) {
  if (rows_.empty()) return;
  const int next =
      std::clamp(selected_ + delta, 0, static_cast<int>(rows_.size()) - 1);
  if (next == selected_) return;
  selected_ = next;
  EnsureSelectionVisible();
  selectionFrom_ = selectionRow_;
  selectionTo_ = static_cast<float>(selected_);
  QueryPerformanceCounter(&selectionStart_);
  selectionAnimating_ = true;
  LogDebug(L"选中: [%d] %ls", selected_, rows_[static_cast<size_t>(selected_)].title.c_str());
  UpdateSceneState();
  if (!animating_) {
    animating_ = true;
    SetTimer(hwnd_, kTimerAnim, 8, nullptr);
  }
}

void LauncherWindow::ScrollBy(int delta) {
  if (rows_.empty()) return;
  const int maxScroll = std::max(0, static_cast<int>(rows_.size()) - visibleRows_);
  const int next = std::clamp(scroll_ + delta, 0, maxScroll);
  if (next == scroll_) return;
  scroll_ = next;
  UpdateSceneState();
  InvalidateRect(hwnd_, nullptr, FALSE);
}

void LauncherWindow::StartShowAnimation() {
  QueryPerformanceCounter(&showStart_);
  showAnimating_ = true;
  showAlpha_ = 0.0f;
  if (!animating_) {
    animating_ = true;
    SetTimer(hwnd_, kTimerAnim, 8, nullptr);
  }
}

void LauncherWindow::StartContentFade() {
  QueryPerformanceCounter(&contentStart_);
  contentAlpha_ = 0.0f;
  contentAnimating_ = true;
  if (!animating_) {
    animating_ = true;
    SetTimer(hwnd_, kTimerAnim, 8, nullptr);
  }
}

void LauncherWindow::TickAnimation() {
  if (showAnimating_) {
    const double t = Progress(showStart_, kShowAnimMs);
    showAlpha_ = EaseOut(t);
    if (t >= 1.0) {
      showAlpha_ = 1.0f;
      showAnimating_ = false;
    }
  }
  if (selectionAnimating_) {
    const double t = Progress(selectionStart_, kSelectionAnimMs);
    selectionRow_ = selectionFrom_ + (selectionTo_ - selectionFrom_) * EaseOut(t);
    if (t >= 1.0) {
      selectionRow_ = selectionTo_;
      selectionAnimating_ = false;
    }
  }
  if (contentAnimating_) {
    const double t = Progress(contentStart_, kContentAnimMs);
    contentAlpha_ = EaseOut(t);
    if (t >= 1.0) {
      contentAlpha_ = 1.0f;
      contentAnimating_ = false;
    }
  }
  UpdateSceneState();
  if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
  if (!showAnimating_ && !selectionAnimating_ && !contentAnimating_) {
    animating_ = false;
    if (hwnd_) KillTimer(hwnd_, kTimerAnim);
  }
}

void LauncherWindow::ResetQuery() {
  input_.SetText(L"");
  contentAlpha_ = 1.0f;
  caretVisible_ = true;
  RefreshResults();
}

void LauncherWindow::RenderNow() {
  if (!renderer_ || !scene_ || !renderer_->BeginFrame()) return;
  scene_->Draw(theme_, sceneState_, false, static_cast<float>(dpi_) / 96.0f);
  renderer_->EndFrame();
}

void LauncherWindow::ShowFromTray() {
  // 托盘左键只负责「打开」，不做切换：一次点击 shell 会先后送来 NIN_SELECT 与 WM_LBUTTONUP 两条
  // 通知，任何「切换」语义都会变成「显示 → 隐藏」，用户看到的就是搜索框闪一下消失。
  const uint64_t now = GetTickCount64();
  if (lastTrayClickAt_ != 0 && now - lastTrayClickAt_ < kTrayClickDebounceMs) {
    LogDebug(L"托盘左键: 忽略重复通知（距上次 %llu ms）", now - lastTrayClickAt_);
    return;
  }
  lastTrayClickAt_ = now;
  Log(L"托盘左键: 显示窗口");
  ShowAndFocus();
}

void LauncherWindow::InjectTrayClick() {
  if (!hwnd_) return;
  // v4 的真实顺序：先 NIN_SELECT，再 WM_LBUTTONUP（都是 kTrayMessage，通知码在 lParam 低字）。
  Log(L"注入托盘左键: NIN_SELECT + WM_LBUTTONUP");
  PostMessageW(hwnd_, kTrayMessage, 0, MAKELPARAM(NIN_SELECT, 1));
  PostMessageW(hwnd_, kTrayMessage, 0, MAKELPARAM(WM_LBUTTONUP, 1));
}

void LauncherWindow::ProbeSettingsCommands(const std::wstring& spec) {
  if (!settingsWindow_ || !settingsWindow_->hwnd() || spec.empty()) return;
  HWND target = settingsWindow_->hwnd();
  size_t start = 0;
  std::wstring sent;
  while (start <= spec.size()) {
    size_t comma = spec.find(L',', start);
    if (comma == std::wstring::npos) comma = spec.size();
    const std::wstring token = spec.substr(start, comma - start);
    start = comma + 1;
    if (token.empty()) continue;
    const int id = static_cast<int>(wcstol(token.c_str(), nullptr, 10));
    if (id <= 0) continue;
    PostMessageW(target, WM_COMMAND, MAKEWPARAM(static_cast<WORD>(id), BN_CLICKED), 0);
    sent += (sent.empty() ? L"" : L",") + std::to_wstring(id);
  }
  Log(L"设置窗探针: 发送控件命令 %ls", sent.c_str());
}

void LauncherWindow::Toggle() {
  if (visible_) {
    Hide();
  } else {
    ShowAndFocus();
  }
}

void LauncherWindow::ShowAndFocus() {
  if (visible_) return;
  // 空查询的推荐来自使用记录，每次唤出前重建，保证是最近用过的顺序。
  if (input_.text().empty()) RefreshResults();
  LARGE_INTEGER positionStart{};
  QueryPerformanceCounter(&positionStart);
  PositionOnCursorMonitor();
  const double positionMs = ElapsedMs(positionStart);
  QueryPerformanceCounter(&showRequestedAt_);
  measureFirstFrame_ = true;
  ShowWindow(hwnd_, SW_SHOW);
  const double showMs = ElapsedMs(showRequestedAt_);
  bool foregroundOk = SetForegroundWindow(hwnd_) != FALSE;
  if (!foregroundOk) {
    // 前台锁定：临时附加输入线程后重试
    const HWND foreground = GetForegroundWindow();
    const DWORD foregroundThread = GetWindowThreadProcessId(foreground, nullptr);
    const DWORD thisThread = GetCurrentThreadId();
    if (foregroundThread && foregroundThread != thisThread &&
        AttachThreadInput(thisThread, foregroundThread, TRUE)) {
      foregroundOk = SetForegroundWindow(hwnd_) != FALSE;
      AttachThreadInput(thisThread, foregroundThread, FALSE);
    }
  }
  Log(L"显示窗口 (dpi=%u, 前台=%ls, 定位 %.1fms, Show %.1fms, 激活后 %.1fms)", dpi_,
      foregroundOk ? L"是" : L"否", positionMs, showMs, ElapsedMs(showRequestedAt_));
  visible_ = true;
  caretVisible_ = true;
  StartShowAnimation();
  SetTimer(hwnd_, kTimerCaret, kCaretBlinkMs, nullptr);
  SetTimer(hwnd_, kTimerIcons, 120, nullptr);
  InvalidateRect(hwnd_, nullptr, FALSE);
  UpdateWindow(hwnd_);
  UpdateImeWindow();
}

void LauncherWindow::Hide() {
  if (!hwnd_) return;
  ShowWindow(hwnd_, SW_HIDE);
  if (visible_) Log(L"隐藏窗口");
  visible_ = false;
  hoverChip_ = -1;
  hoverRow_ = -1;
  pressedRow_ = -1;
  mouseTracking_ = false;
  KillTimer(hwnd_, kTimerCaret);
  KillTimer(hwnd_, kTimerAnim);
  KillTimer(hwnd_, kTimerIcons);
  animating_ = false;
  showAnimating_ = false;
  selectionAnimating_ = false;
  contentAnimating_ = false;
  showAlpha_ = 1.0f;
  ResetQuery();
}

void LauncherWindow::ToPanelDip(int clientX, int clientY, float* xDip, float* yDip) const {
  const float scale = static_cast<float>(dpi_) / 96.0f;
  if (xDip) *xDip = static_cast<float>(clientX) / scale - theme_.shadowMargin;
  if (yDip) *yDip = static_cast<float>(clientY) / scale - theme_.shadowMargin;
}

int LauncherWindow::ChipAtPoint(int clientX, int clientY) {
  if (!scene_ || sceneState_.chips.empty()) return -1;
  float x = 0.0f;
  float y = 0.0f;
  ToPanelDip(clientX, clientY, &x, &y);
  return scene_->ChipAt(sceneState_, x, y);
}

int LauncherWindow::RowAtPoint(int clientX, int clientY) {
  if (!scene_ || sceneState_.rows.empty()) return -1;
  float x = 0.0f;
  float y = 0.0f;
  ToPanelDip(clientX, clientY, &x, &y);
  return scene_->RowAt(sceneState_, x, y);
}

bool LauncherWindow::RowIsActionable(int index) const {
  if (index < 0 || index >= static_cast<int>(rows_.size())) return false;
  return rows_[static_cast<size_t>(index)].action != RowAction::None;
}

void LauncherWindow::InjectMouseRow(int row, bool click) {
  if (!hwnd_ || row < 0) return;
  const float scale = static_cast<float>(dpi_) / 96.0f;
  const float listTop = theme_.listTop + (sceneState_.chips.empty() ? 0.0f : theme_.filterRow);
  const float xDip = theme_.panelWidth * 0.5f;
  const float yDip =
      listTop + static_cast<float>(row) * (theme_.rowHeight + theme_.rowGap) + theme_.rowHeight * 0.5f;
  const int xPx =
      static_cast<int>(std::lround((theme_.shadowMargin + xDip) * scale));
  const int yPx =
      static_cast<int>(std::lround((theme_.shadowMargin + yDip) * scale));
  const LPARAM point = MAKELPARAM(xPx, yPx);
  LogDebug(L"注入鼠标: 行 %d -> 客户端 (%d, %d)%ls", row + 1, xPx, yPx, click ? L" + 单击" : L"");
  PostMessageW(hwnd_, WM_MOUSEMOVE, 0, point);
  if (click) {
    PostMessageW(hwnd_, WM_LBUTTONDOWN, MK_LBUTTON, point);
    PostMessageW(hwnd_, WM_LBUTTONUP, 0, point);
  }
}

void LauncherWindow::ClickRow(int index, bool shift, bool ctrl) {
  if (!RowIsActionable(index)) return;
  Log(L"点击: [%d] %ls", index, rows_[static_cast<size_t>(index)].title.c_str());
  if (index != selected_) {
    // 点击直接把选中挪过去：不滑动，指针已经在目标行上了。
    selected_ = index;
    EnsureSelectionVisible();
    selectionRow_ = static_cast<float>(selected_);
    selectionFrom_ = selectionRow_;
    selectionTo_ = selectionRow_;
    selectionAnimating_ = false;
    hoverRow_ = index;
    UpdateSceneState();
    InvalidateRect(hwnd_, nullptr, FALSE);
  }
  ActivateRow(index, shift, ctrl);
}

void LauncherWindow::ActivateRow(int index, bool shift, bool ctrl) {
  if (index < 0 || index >= static_cast<int>(rows_.size())) {
    Log(L"激活: 当前没有可启动项");
    return;
  }
  const ResultRow& row = rows_[static_cast<size_t>(index)];
  Log(L"激活: [%d] %ls action=%d target=%ls", index, row.title.c_str(),
      static_cast<int>(row.action), row.target.c_str());
  if (row.action == RowAction::None) {
    Log(L"激活: 当前行为占位提示");
    return;
  }
  std::wstring error;
  bool ok = false;
  if (row.action == RowAction::LaunchApp || row.action == RowAction::LaunchUwp) {
    AppEntry entry;
    entry.title = row.title;
    entry.detail = row.subtitle;
    entry.launchTarget = row.target;
    entry.iconKey = row.iconKey;
    entry.glyph = row.glyph;
    entry.color = row.color;
    entry.kind = (row.action == RowAction::LaunchUwp) ? LaunchKind::AppUserModelId
                                                      : LaunchKind::Shell;
    const LaunchMode mode =
        shift ? LaunchMode::Reveal : (ctrl ? LaunchMode::Elevated : LaunchMode::Normal);
    ok = LaunchEntry(entry, mode, &error);
    if (ok) {
      Log(L"启动: %ls (%ls)", row.title.c_str(),
          mode == LaunchMode::Reveal ? L"定位"
                                     : (mode == LaunchMode::Elevated ? L"管理员" : L"普通"));
    } else {
      LogError(L"启动失败: %ls -> %ls", row.title.c_str(), error.c_str());
    }
  } else {
    ok = shift ? RevealPath(row.target, &error) : OpenPath(row.target, &error);
    if (ok) {
      Log(L"%ls: %ls", shift ? L"定位" : L"打开", row.target.c_str());
    } else {
      LogError(L"打开失败: %ls -> %ls", row.target.c_str(), error.c_str());
    }
  }
  if (ok) {
    usage_.Record(row.target);
    Hide();
  }
}

bool LauncherWindow::HandleKeyDown(WPARAM key) {
  const bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
  // 筛选组：Tab / Shift+Tab 循环，Ctrl+1..9 直达（「全部」= 第 1 个）。
  if (key == VK_TAB) {
    if (!settings_.filters.enabled || input_.composing()) return false;
    const bool shiftTab = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
    CycleFilter(shiftTab ? -1 : 1);
    return true;
  }
  if (ctrl && key >= '1' && key <= '9') {
    if (!settings_.filters.enabled) return false;
    const int index = static_cast<int>(key - '1');
    if (index >= static_cast<int>(settings_.filters.groups.size()) + 1) return true;  // 吞掉，不动
    SelectFilter(index);
    return true;
  }
  switch (key) {
    case VK_ESCAPE:
      if (input_.composing()) {
        input_.CancelComposition();
        UpdateSceneState();
        UpdateImeWindow();
        InvalidateRect(hwnd_, nullptr, FALSE);
      } else {
        Hide();
      }
      return true;
    case VK_RETURN: {
      const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
      ActivateRow(selected_, shift, ctrl);
      return true;
    }
    case VK_BACK:
      if (input_.Backspace()) {
        Refilter();
        UpdateImeWindow();
        LogDebug(L"查询: \"%ls\"", input_.text().c_str());
      }
      return true;
    case VK_DELETE:
      if (input_.DeleteForward()) {
        Refilter();
        UpdateImeWindow();
      }
      return true;
    case VK_LEFT:
      input_.MoveLeft();
      UpdateSceneState();
      UpdateImeWindow();
      InvalidateRect(hwnd_, nullptr, FALSE);
      return true;
    case VK_RIGHT:
      input_.MoveRight();
      UpdateSceneState();
      UpdateImeWindow();
      InvalidateRect(hwnd_, nullptr, FALSE);
      return true;
    case VK_HOME:
      input_.MoveHome();
      UpdateSceneState();
      UpdateImeWindow();
      InvalidateRect(hwnd_, nullptr, FALSE);
      return true;
    case VK_END:
      input_.MoveEnd();
      UpdateSceneState();
      UpdateImeWindow();
      InvalidateRect(hwnd_, nullptr, FALSE);
      return true;
    case VK_UP:
      MoveSelection(-1);
      return true;
    case VK_DOWN:
      MoveSelection(1);
      return true;
    case VK_PRIOR:
      MoveSelection(-visibleRows_);
      return true;
    case VK_NEXT:
      MoveSelection(visibleRows_);
      return true;
    default:
      if (ctrl && (key == 'V' || key == 'v')) {
        if (input_.PasteFromClipboard(hwnd_)) {
          Refilter();
          UpdateImeWindow();
          LogDebug(L"粘贴: \"%ls\"", input_.text().c_str());
        }
        return true;
      }
      return false;
  }
}

bool LauncherWindow::HandleChar(wchar_t ch) {
  if (ch < 0x20 || ch == 0x7F) return false;
  if (input_.composing()) return false;
  if (!input_.InsertChar(ch)) return false;
  Refilter();
  UpdateImeWindow();
  LogDebug(L"查询: \"%ls\" (caret=%zu)", input_.text().c_str(), input_.caret());
  return true;
}

bool LauncherWindow::HandleIme(UINT message, WPARAM wParam, LPARAM lParam) {
  (void)wParam;
  if (message == WM_IME_STARTCOMPOSITION) {
    input_.BeginComposition();
    UpdateSceneState();
    UpdateImeWindow();
    InvalidateRect(hwnd_, nullptr, FALSE);
    return true;
  }
  if (message == WM_IME_ENDCOMPOSITION) {
    input_.CancelComposition();
    UpdateSceneState();
    InvalidateRect(hwnd_, nullptr, FALSE);
    return true;
  }
  if (message != WM_IME_COMPOSITION) return false;

  HIMC context = ImmGetContext(hwnd_);
  if (!context) return false;
  bool committed = false;
  bool composing = false;
  if (lParam & GCS_RESULTSTR) {
    const LONG bytes = ImmGetCompositionStringW(context, GCS_RESULTSTR, nullptr, 0);
    if (bytes > 0) {
      std::wstring result(static_cast<size_t>(bytes) / sizeof(wchar_t), L'\0');
      ImmGetCompositionStringW(context, GCS_RESULTSTR, result.data(), bytes);
      input_.CommitComposition(std::move(result));
      committed = true;
      LogDebug(L"IME 上屏: \"%ls\"", input_.text().c_str());
    }
  } else if (lParam & GCS_COMPSTR) {
    const LONG bytes = ImmGetCompositionStringW(context, GCS_COMPSTR, nullptr, 0);
    std::wstring composition;
    if (bytes > 0) {
      composition.resize(static_cast<size_t>(bytes) / sizeof(wchar_t));
      ImmGetCompositionStringW(context, GCS_COMPSTR, composition.data(), bytes);
    }
    const LONG caret = ImmGetCompositionStringW(context, GCS_CURSORPOS, nullptr, 0);
    input_.SetComposition(std::move(composition), caret >= 0 ? static_cast<size_t>(caret) : 0);
    composing = true;
  }
  ImmReleaseContext(hwnd_, context);
  if (committed) {
    Refilter();
    UpdateImeWindow();
  } else if (composing) {
    UpdateSceneState();
    UpdateImeWindow();
    InvalidateRect(hwnd_, nullptr, FALSE);
  }
  return true;
}

LRESULT CALLBACK LauncherWindow::Thunk(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
  if (message == WM_NCCREATE) {
    auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
    auto* self = static_cast<LauncherWindow*>(create->lpCreateParams);
    self->hwnd_ = hwnd;
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
  }
  auto* self = reinterpret_cast<LauncherWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  if (self && self->hwnd_ == hwnd) {
    return self->Handle(message, wParam, lParam);
  }
  return DefWindowProcW(hwnd, message, wParam, lParam);
}

LRESULT LauncherWindow::Handle(UINT message, WPARAM wParam, LPARAM lParam) {
  if (message == TaskbarCreatedMessage() && message != 0) {
    if (tray_.added()) {
      tray_.ReAdd();
    } else if (settings_.tray) {
      tray_.Create(hwnd_);
    }
    return 0;
  }
  if (message == WM_COPYDATA) {
    // Everything 的查询回复：SDK 把结果拷进自己的全局列表后这里再读出来。
    // 注意：处理期间 Everything 正阻塞在「把这条回复投给我们」的 SendMessage 上（3 秒超时），
    // 所以这段代码里不能再去调用 Everything（Request / Status），只能拷贝 + 重排 + 延后重试。
    uint32_t generation = 0;
    std::vector<FileHit> hits;
    handlingEverythingReply_ = true;
    const bool handled = everything_.HandleReply(message, wParam, lParam, &generation, &hits);
    handlingEverythingReply_ = false;
    if (handled) {
      if (generation == queryGeneration_) {
        fileHits_ = std::move(hits);
        // 纯扩展名组把 ext: 下推给 Everything 后 0 条：去掉下推重试一次（每次查询 + 组只重试一次），
        // 免得某个扩展名在 ext: 语法里表现异常时界面空白。
        const FilterGroup* filter = CurrentFilterGroup();
        const std::wstring key = input_.text() + L'\n' + (filter ? filter->name : L"");
        if (fileHits_.empty() && everything_.lastRequestHadFilter() && key != pushdownRetryKey_) {
          pushdownRetryKey_ = key;
          LogWarn(L"Everything 下推 ext: 后 0 条，改用本地过滤重试: \"%ls\"",
                  everything_.lastSearch().c_str());
          // 重试必须延后到消息循环里：这里直接 Request 会与 Everything 互相等待，
          // 最后只能等它的 3 秒投递超时，表现为切换筛选组时卡顿。
          PostMessageW(hwnd_, kMsgEverythingRetry, 0, 0);
          // 先把这一批（空）结果画出来：占位行或应用行，不要让界面停在旧结果上。
          RebuildRows();
          StartContentFade();
          UpdateSceneState();
          InvalidateRect(hwnd_, nullptr, FALSE);
          return 0;
        }
        RebuildRows();
        StartContentFade();
        UpdateSceneState();
        InvalidateRect(hwnd_, nullptr, FALSE);
        LogDebug(L"Everything 结果: %zu 条（往返 %.1f ms）", fileHits_.size(),
                 ElapsedMs(everythingRequestedAt_));
      }
      return 0;
    }
    return DefWindowProcW(hwnd_, message, wParam, lParam);
  }
  switch (message) {
    case WM_ERASEBKGND:
      return 1;
    case kTrayMessage: {
      // NOTIFYICON_VERSION_4 的布局：lParam 低字是通知码（NIN_SELECT / WM_LBUTTONUP / WM_CONTEXTMENU
      // 等），高字是图标 id。一次左键点击 shell 会**先后**送来两条（NIN_SELECT + WM_LBUTTONUP），
      // 因此这里绝不能直接 Toggle()：那会「显示 → 隐藏」，用户看到的就是搜索框闪一下没了。
      const UINT event = LOWORD(lParam);
      LogDebug(L"托盘通知: event=%u wParam=%llu", event, static_cast<unsigned long long>(wParam));
      if (event == WM_LBUTTONUP || event == NIN_SELECT || event == NIN_KEYSELECT) {
        ShowFromTray();
      } else if (event == WM_RBUTTONUP || event == WM_CONTEXTMENU) {
        const UINT command = tray_.ShowMenu(settings_.autostart);
        if (command) HandleTrayCommand(command);
      }
      return 0;
    }
    case WM_PAINT: {
      PAINTSTRUCT ps;
      BeginPaint(hwnd_, &ps);
      const bool wasFirstFrame = measureFirstFrame_;
      if (wasFirstFrame) LogDebug(L"WM_PAINT 到达: %.1f ms", ElapsedMs(showRequestedAt_));
      RenderNow();
      if (wasFirstFrame) {
        Log(L"首帧耗时: %.1f ms", ElapsedMs(showRequestedAt_));
        measureFirstFrame_ = false;
      }
      EndPaint(hwnd_, &ps);
      return 0;
    }
    case WM_CHAR:
      if (static_cast<wchar_t>(wParam) == 0x1B) return 0;
      HandleChar(static_cast<wchar_t>(wParam));
      return 0;
    case WM_KEYDOWN:
      if (HandleKeyDown(wParam)) return 0;
      break;
    case WM_IME_STARTCOMPOSITION:
    case WM_IME_COMPOSITION:
    case WM_IME_ENDCOMPOSITION:
      if (HandleIme(message, wParam, lParam)) return 0;
      break;
    case WM_LBUTTONDOWN: {
      // 芯片切组，结果行直接启动；面板其它区域（含空白处）仍然什么都不做。
      if (!scene_) break;
      const int x = GET_X_LPARAM(lParam);
      const int y = GET_Y_LPARAM(lParam);
      const int chip = ChipAtPoint(x, y);
      if (chip >= 0) {
        SelectFilter(chip);
        return 0;
      }
      const int row = RowAtPoint(x, y);
      if (row >= 0) {
        // 按下只记住按下的是哪一行，真正的启动放到抬起：启动后鼠标抬起才不会落进新打开的窗口。
        pressedRow_ = row;
        return 0;
      }
      break;
    }
    case WM_LBUTTONUP: {
      const int pressed = pressedRow_;
      pressedRow_ = -1;
      if (pressed < 0) break;
      if (RowAtPoint(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)) != pressed) break;
      const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
      const bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
      ClickRow(pressed, shift, ctrl);
      return 0;
    }
    case WM_MOUSEMOVE: {
      if (!scene_) break;
      if (!mouseTracking_) {
        TRACKMOUSEEVENT track{};
        track.cbSize = sizeof(track);
        track.dwFlags = TME_LEAVE;
        track.hwndTrack = hwnd_;
        TrackMouseEvent(&track);
        mouseTracking_ = true;
      }
      const int chip = ChipAtPoint(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
      const int row = RowAtPoint(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
      if (chip != hoverChip_ || row != hoverRow_) {
        if (row != hoverRow_ && row >= 0 && row < static_cast<int>(rows_.size())) {
          LogDebug(L"悬停: 行 %d %ls", row + 1, rows_[static_cast<size_t>(row)].title.c_str());
        }
        hoverChip_ = chip;
        hoverRow_ = row;
        // 只改 hover 字段：不重抄 rows_（200 行带字符串的副本），悬停要跟手。
        sceneState_.hoverChip = hoverChip_;
        sceneState_.hoverRow = hoverRow_;
        InvalidateRect(hwnd_, nullptr, FALSE);
      }
      return 0;
    }
    case WM_SETCURSOR: {
      // 指针落在芯片 / 可启动的行上时给手型，其它地方保持箭头。
      if (LOWORD(lParam) != HTCLIENT || !scene_) break;
      POINT cursor{};
      if (!GetCursorPos(&cursor)) break;
      ScreenToClient(hwnd_, &cursor);
      if (ChipAtPoint(cursor.x, cursor.y) >= 0 || RowAtPoint(cursor.x, cursor.y) >= 0) {
        SetCursor(LoadCursorW(nullptr, IDC_HAND));
        return TRUE;
      }
      break;
    }
    case WM_MOUSELEAVE:
      mouseTracking_ = false;
      pressedRow_ = -1;
      if (hoverChip_ != -1 || hoverRow_ != -1) {
        hoverChip_ = -1;
        hoverRow_ = -1;
        sceneState_.hoverChip = hoverChip_;
        sceneState_.hoverRow = hoverRow_;
        InvalidateRect(hwnd_, nullptr, FALSE);
      }
      return 0;
    case WM_MOUSEWHEEL: {
      const int delta = GET_WHEEL_DELTA_WPARAM(wParam);
      ScrollBy(delta > 0 ? -1 : 1);
      return 0;
    }
    case WM_ACTIVATE:
      if (LOWORD(wParam) == WA_INACTIVE && visible_) {
        // 托盘点开的那一下，shell 收尾时会把前台抢回任务栏，紧跟的失活会把刚显示的窗口立刻藏起来
        // ——同样表现为「闪一下就没了」。只在「刚处理过托盘左键」且新前台是 shell 自己的界面时放行它。
        const uint64_t now = GetTickCount64();
        if (lastTrayClickAt_ != 0 && now - lastTrayClickAt_ < kTrayDeactivateGraceMs &&
            IsShellSurface(GetForegroundWindow())) {
          LogDebug(L"托盘左键: 忽略 shell 抢前台导致的失活（%llu ms）", now - lastTrayClickAt_);
          return 0;
        }
        Hide();
      }
      return 0;
    case WM_TIMER:
      if (wParam == kTimerTrayRetry) {
        if (tray_.Create(hwnd_)) {
          KillTimer(hwnd_, kTimerTrayRetry);
          trayRetries_ = 0;
        } else if (++trayRetries_ >= 5) {
          KillTimer(hwnd_, kTimerTrayRetry);
          LogWarn(L"托盘初始化放弃（已重试 5 次）");
        }
        return 0;
      }
      if (wParam == kTimerCaret) {
        caretVisible_ = !caretVisible_;
        UpdateSceneState();
        InvalidateRect(hwnd_, nullptr, FALSE);
        return 0;
      }
      if (wParam == kTimerAnim) {
        TickAnimation();
        return 0;
      }
      if (wParam == kTimerFileRefresh) {
        if (!FileSearchIsEverything() && files_.version() != fileVersionSeen_) {
          fileVersionSeen_ = files_.version();
          RefreshResults();
          StartContentFade();
          UpdateSceneState();
          InvalidateRect(hwnd_, nullptr, FALSE);
          LogDebug(L"文件索引已更新，当前结果 %zu 条", rows_.size());
        }
        return 0;
      }
      if (wParam == kTimerIcons) {
        if (icons_ && icons_->version() != iconVersionSeen_) {
          icons_->DrainCompleted();
          iconVersionSeen_ = icons_->version();
          InvalidateRect(hwnd_, nullptr, FALSE);
        }
        return 0;
      }
      if (onTimer_) onTimer_(wParam);
      return 0;
    case WM_DPICHANGED: {
      const auto* suggested = reinterpret_cast<const RECT*>(lParam);
      if (suggested) {
        dpi_ = HIWORD(wParam);
        widthPx_ = static_cast<UINT>(suggested->right - suggested->left);
        heightPx_ = static_cast<UINT>(suggested->bottom - suggested->top);
        if (renderer_) renderer_->Resize(widthPx_, heightPx_);
        SetWindowPos(hwnd_, HWND_TOP, suggested->left, suggested->top,
                     static_cast<int>(widthPx_), static_cast<int>(heightPx_), SWP_NOACTIVATE);
        InvalidateRect(hwnd_, nullptr, FALSE);
      }
      return 0;
    }
    case WM_HOTKEY:
      if (onHotkey_) onHotkey_();
      return 0;
    case kMsgToggle:
      ShowAndFocus();
      return 0;
    case kMsgEverythingRetry: {
      // 从 WM_COPYDATA 里延后出来的重试：只在「查询 + 组」还没变时补发一次。
      const FilterGroup* filter = CurrentFilterGroup();
      const std::wstring key = input_.text() + L'\n' + (filter ? filter->name : L"");
      if (key == pushdownRetryKey_) RequestEverythingRows();
      return 0;
    }
    case WM_CLOSE:
      DestroyWindow(hwnd_);
      return 0;
    case WM_DESTROY:
      hwnd_ = nullptr;
      PostQuitMessage(0);
      return 0;
    default:
      break;
  }
  return DefWindowProcW(hwnd_, message, wParam, lParam);
}
