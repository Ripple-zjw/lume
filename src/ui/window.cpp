#include "ui/window.h"

#include <dwmapi.h>
#include <imm.h>

#include <algorithm>
#include <cmath>
#include <utility>

#include "core/log.h"
#include "launch/launcher.h"
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
constexpr UINT_PTR kTimerCaret = 0x20;
constexpr UINT_PTR kTimerAnim = 0x21;
constexpr UINT kCaretBlinkMs = 530;
constexpr double kShowAnimMs = 90.0;
constexpr double kSelectionAnimMs = 120.0;
constexpr double kContentAnimMs = 80.0;

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
                            IconCache* icons) {
  instance_ = instance;
  renderer_ = renderer;
  scene_ = scene;
  icons_ = icons;
  theme_ = MakeTheme(ThemeMode::Dark);
  visibleRows_ = Scene::kMaxVisibleRows;
  input_.SetText(L"");

  LARGE_INTEGER indexStart{};
  QueryPerformanceCounter(&indexStart);
  apps_ = CollectAppEntries();
  Log(L"应用索引耗时: %.1f ms", ElapsedMs(indexStart));
  filtered_ = FilterEntries(apps_, L"");
  RebuildRows();

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
  if (scene_) scene_->SetTheme(theme);
  ApplyBackdrop();
  UpdateSceneState();
  if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
}

void LauncherWindow::SetQuery(const std::wstring& query) {
  input_.SetText(query);
  contentAlpha_ = 1.0f;
  filtered_ = FilterEntries(apps_, input_.text());
  RebuildRows();
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
  const float panelHeight = PanelHeight(theme_, visibleRows_);
  widthPx_ = static_cast<UINT>(std::lround((theme_.panelWidth + theme_.shadowMargin * 2.0f) * scale));
  heightPx_ =
      static_cast<UINT>(std::lround((panelHeight + theme_.shadowMargin * 2.0f) * scale));
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

void LauncherWindow::PreloadIcons() {
  if (!icons_) return;
  const size_t count = std::min(rows_.size(), static_cast<size_t>(visibleRows_));
  for (size_t i = 0; i < count; ++i) {
    if (!rows_[i].iconKey.empty()) icons_->Get(rows_[i].iconKey);
  }
}

void LauncherWindow::RebuildRows() {
  rows_ = BuildRowsFromEntries(filtered_, !input_.text().empty());
  selected_ = 0;
  scroll_ = 0;
  selectionRow_ = 0.0f;
  EnsureSelectionVisible();
  PreloadIcons();
  UpdateSceneState();
}

void LauncherWindow::Refilter() {
  filtered_ = FilterEntries(apps_, input_.text());
  RebuildRows();
  StartContentFade();
  UpdateSceneState();
  if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
  Log(L"过滤: \"%ls\" -> %zu 条", input_.text().c_str(), filtered_.size());
}

const AppEntry* LauncherWindow::SelectedEntry() const {
  if (selected_ < 0 || selected_ >= static_cast<int>(filtered_.size())) return nullptr;
  return filtered_[static_cast<size_t>(selected_)];
}

void LauncherWindow::UpdateSceneState() {
  sceneState_.displayText = input_.DisplayText();  sceneState_.caretIndex = input_.DisplayCaret();
  sceneState_.compositionStart = input_.composing() ? input_.compositionStart() : 0;
  sceneState_.compositionLength = input_.composing() ? input_.composition().size() : 0;
  sceneState_.caretVisible = caretVisible_;
  sceneState_.rows = rows_;
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
  Log(L"选中: [%d] %ls", selected_, rows_[static_cast<size_t>(selected_)].title.c_str());
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
  filtered_ = FilterEntries(apps_, L"");
  RebuildRows();
}

void LauncherWindow::RenderNow() {
  if (!renderer_ || !scene_ || !renderer_->BeginFrame()) return;
  scene_->Draw(theme_, sceneState_, false, static_cast<float>(dpi_) / 96.0f);
  renderer_->EndFrame();
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
  InvalidateRect(hwnd_, nullptr, FALSE);
  UpdateWindow(hwnd_);
  UpdateImeWindow();
}

void LauncherWindow::Hide() {
  if (!hwnd_) return;
  ShowWindow(hwnd_, SW_HIDE);
  if (visible_) Log(L"隐藏窗口");
  visible_ = false;
  KillTimer(hwnd_, kTimerCaret);
  KillTimer(hwnd_, kTimerAnim);
  animating_ = false;
  showAnimating_ = false;
  selectionAnimating_ = false;
  contentAnimating_ = false;
  showAlpha_ = 1.0f;
  ResetQuery();
}

bool LauncherWindow::HandleKeyDown(WPARAM key) {
  const bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
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
      const AppEntry* entry = SelectedEntry();
      if (!entry) {
        Log(L"回车: 当前没有可启动项");
        return true;
      }
      const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
      const LaunchMode mode =
          shift ? LaunchMode::Reveal : (ctrl ? LaunchMode::Elevated : LaunchMode::Normal);
      std::wstring error;
      if (LaunchEntry(*entry, mode, &error)) {
        Log(L"启动: %ls (%ls)", entry->title.c_str(),
            mode == LaunchMode::Reveal ? L"定位"
                                       : (mode == LaunchMode::Elevated ? L"管理员" : L"普通"));
        Hide();
      } else {
        Log(L"启动失败: %ls -> %ls", entry->title.c_str(), error.c_str());
      }
      return true;
    }
    case VK_BACK:
      if (input_.Backspace()) {
        Refilter();
        UpdateImeWindow();
        Log(L"查询: \"%ls\"", input_.text().c_str());
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
          Log(L"粘贴: \"%ls\"", input_.text().c_str());
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
  Log(L"查询: \"%ls\" (caret=%zu)", input_.text().c_str(), input_.caret());
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
      Log(L"IME 上屏: \"%ls\"", input_.text().c_str());
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
    case WM_MOUSEWHEEL: {
      const int delta = GET_WHEEL_DELTA_WPARAM(wParam);
      ScrollBy(delta > 0 ? -1 : 1);
      return 0;
    }
    case WM_ACTIVATE:
      if (LOWORD(wParam) == WA_INACTIVE && visible_) {
        Hide();
      }
      return 0;
    case WM_TIMER:
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
