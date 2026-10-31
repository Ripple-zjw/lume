#include "ui/window.h"

#include <dwmapi.h>

#include <cmath>
#include <utility>

#include "core/log.h"
#include "platform/system.h"
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
#ifndef DWMSBT_MAINWINDOW
#define DWMSBT_MAINWINDOW 2
#endif
#ifndef DWMWCP_ROUND
#define DWMWCP_ROUND 2
#endif

namespace {
constexpr wchar_t kClassName[] = L"eToolsLauncherWindow";
constexpr int kRowsForLayout = 3;

double ElapsedMs(const LARGE_INTEGER& start) {
  LARGE_INTEGER now{};
  LARGE_INTEGER frequency{};
  QueryPerformanceCounter(&now);
  QueryPerformanceFrequency(&frequency);
  return static_cast<double>(now.QuadPart - start.QuadPart) * 1000.0 /
         static_cast<double>(frequency.QuadPart);
}
}  // namespace

const wchar_t* WindowClassName() { return kClassName; }

LauncherWindow::~LauncherWindow() {
  if (hwnd_) {
    DestroyWindow(hwnd_);
    hwnd_ = nullptr;
  }
}

bool LauncherWindow::Create(HINSTANCE instance, Renderer* renderer, Scene* scene) {
  instance_ = instance;
  renderer_ = renderer;
  scene_ = scene;
  theme_ = MakeTheme(ThemeMode::Dark);
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
  wc.style = CS_HREDRAW | CS_VREDRAW;
  if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
    Log(L"RegisterClassExW 失败: %lu", GetLastError());
    return false;
  }

  hwnd_ = CreateWindowExW(
      WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOREDIRECTIONBITMAP, kClassName, L"eTools", WS_POPUP,
      0, 0, static_cast<int>(widthPx_), static_cast<int>(heightPx_), nullptr, nullptr, instance,
      this);
  if (!hwnd_) {
    Log(L"CreateWindowExW 失败: %lu", GetLastError());
    return false;
  }
  if (!renderer_->AttachWindow(hwnd_, widthPx_, heightPx_)) {
    Log(L"渲染目标绑定失败");
    return false;
  }
  ApplyBackdrop();
  return true;
}

void LauncherWindow::SetTheme(const Theme& theme) {
  theme_ = theme;
  ApplyBackdrop();
  if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
}

void LauncherWindow::SetSceneState(const SceneState& state) {
  state_ = state;
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
  const float panelHeight = PanelHeight(theme_, kRowsForLayout);
  widthPx_ = static_cast<UINT>(
      std::lround((theme_.panelWidth + theme_.shadowMargin * 2.0f) * scale));
  heightPx_ = static_cast<UINT>(
      std::lround((panelHeight + theme_.shadowMargin * 2.0f) * scale));
}

void LauncherWindow::ApplyBackdrop() {
  if (!hwnd_) return;
  const BOOL dark = theme_.dark ? TRUE : FALSE;
  LogHr(L"DwmSetWindowAttribute(dark)",
        DwmSetWindowAttribute(hwnd_, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark)));
  const int backdrop = DWMSBT_MAINWINDOW;
  const HRESULT backdropHr =
      DwmSetWindowAttribute(hwnd_, DWMWA_SYSTEMBACKDROP_TYPE, &backdrop, sizeof(backdrop));
  if (FAILED(backdropHr)) LogHr(L"DwmSetWindowAttribute(backdrop)", backdropHr);
  const int corner = DWMWCP_ROUND;
  DwmSetWindowAttribute(hwnd_, DWMWA_WINDOW_CORNER_PREFERENCE, &corner, sizeof(corner));
}

void LauncherWindow::PositionOnCursorMonitor() {
  POINT cursor{};
  GetCursorPos(&cursor);
  const RECT work = WorkAreaForPoint(cursor);
  const UINT dpi = DpiForPoint(cursor);
  if (dpi != dpi_) {
    ApplyDpi(dpi);
    if (!renderer_->Resize(widthPx_, heightPx_)) {
      Log(L"窗口尺寸切换失败 (dpi=%u)", dpi);
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

void LauncherWindow::Toggle() {
  if (visible_) {
    Hide();
  } else {
    ShowAndFocus();
  }
}

void LauncherWindow::RenderNow() {
  if (!renderer_ || !scene_ || !renderer_->BeginFrame()) return;
  scene_->Draw(theme_, state_, false, static_cast<float>(dpi_) / 96.0f);
  renderer_->EndFrame();
}

void LauncherWindow::ShowAndFocus() {
  if (visible_) return;
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
  InvalidateRect(hwnd_, nullptr, FALSE);
  UpdateWindow(hwnd_);  // 同步出帧，避免 WM_PAINT 被延后
}

void LauncherWindow::Hide() {
  if (!hwnd_) return;
  ShowWindow(hwnd_, SW_HIDE);
  if (visible_) Log(L"隐藏窗口");
  visible_ = false;
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
  switch (message) {
    case WM_ERASEBKGND:
      return 1;
    case WM_PAINT: {
      PAINTSTRUCT ps;
      BeginPaint(hwnd_, &ps);
      const bool wasFirstFrame = measureFirstFrame_;
      if (wasFirstFrame) Log(L"WM_PAINT 到达: %.1f ms", ElapsedMs(showRequestedAt_));
      RenderNow();
      if (wasFirstFrame) {
        Log(L"首帧耗时: %.1f ms", ElapsedMs(showRequestedAt_));
        measureFirstFrame_ = false;
      }
      EndPaint(hwnd_, &ps);
      return 0;
    }
    case WM_ACTIVATE:
      if (LOWORD(wParam) == WA_INACTIVE && visible_) {
        Hide();
      }
      return 0;
    case WM_KEYDOWN:
      if (wParam == VK_ESCAPE) {
        Hide();
        return 0;
      }
      break;
    case WM_DPICHANGED: {
      const auto* suggested = reinterpret_cast<const RECT*>(lParam);
      if (suggested) {
        dpi_ = HIWORD(wParam);
        widthPx_ = static_cast<UINT>(suggested->right - suggested->left);
        heightPx_ = static_cast<UINT>(suggested->bottom - suggested->top);
        if (renderer_) renderer_->Resize(widthPx_, heightPx_);
        SetWindowPos(hwnd_, HWND_TOP, suggested->left, suggested->top,
                     static_cast<int>(widthPx_), static_cast<int>(heightPx_),
                     SWP_NOACTIVATE);
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
    case WM_TIMER:
      if (onTimer_) onTimer_(wParam);
      return 0;
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
