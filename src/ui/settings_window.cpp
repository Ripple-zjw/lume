#include "ui/settings_window.h"

#include <commctrl.h>
#include <dwmapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <uxtheme.h>

#include <algorithm>

#include "core/log.h"
#include "platform/hotkey.h"
#include "platform/system.h"

namespace {

constexpr wchar_t kClassName[] = L"LumeSettingsWindow";
constexpr wchar_t kCaptureClass[] = L"LumeHotkeyCapture";
constexpr int kWindowWidth = 540;
constexpr int kWindowHeight = 578;

constexpr COLORREF kBackground = RGB(0x20, 0x20, 0x24);
constexpr COLORREF kControl = RGB(0x2A, 0x2A, 0x30);
constexpr COLORREF kText = RGB(0xF0, 0xF0, 0xF2);
constexpr COLORREF kAccent = RGB(0x6E, 0xA8, 0xFF);
constexpr COLORREF kBorder = RGB(0x50, 0x50, 0x58);
constexpr COLORREF kMuted = RGB(0x9A, 0x9A, 0xA4);

enum ControlId {
  kIdTheme = 100,
  kIdAccent = 101,
  kIdAutostart = 103,
  kIdRoots = 104,
  kIdAdd = 105,
  kIdRemove = 106,
  kIdSave = 107,
  kIdCancel = 108,
  kIdStatus = 109,
  kIdHotkeyCaptured = 110,
  kIdCap = 111,
  kIdClearIndex = 112,
  kIdTabGeneral = 120,
  kIdTabIndex = 121,
  kIdBrowseIndex = 122,
  kIdIndexDefault = 123,
  kIdRebuildIndex = 124,
  kIdOpenIndexDir = 125,
  kIdIndexPath = 126,
  kIdIndexInfo = 127,
  kIdGeneralHint = 128,
  kIdIndexRefresh = 129,
  kIdApply = 130,
  kIdFileSearch = 131,
  kIdTabAbout = 132,
  kIdCopyInfo = 133,
};

constexpr UINT kCaptureComboMessage = WM_APP + 20;

HHOOK g_keyboardHook = nullptr;
HWND g_captureHwnd = nullptr;
UINT g_swallowKeyUp = 0;

bool IsModifierVk(UINT virtualKey) {
  return virtualKey == VK_CONTROL || virtualKey == VK_LCONTROL || virtualKey == VK_RCONTROL ||
         virtualKey == VK_MENU || virtualKey == VK_LMENU || virtualKey == VK_RMENU ||
         virtualKey == VK_SHIFT || virtualKey == VK_LSHIFT || virtualKey == VK_RSHIFT ||
         virtualKey == VK_LWIN || virtualKey == VK_RWIN;
}

void ApplyCapturedCombo(HWND hwnd, UINT modifiers, UINT virtualKey) {
  if (modifiers == 0) {
    SetWindowTextW(hwnd, L"需要 Ctrl / Alt / Shift / Win");
    InvalidateRect(hwnd, nullptr, TRUE);
    return;
  }
  const std::wstring name = FormatHotkey(modifiers, virtualKey);
  if (name.empty()) {
    SetWindowTextW(hwnd, L"该按键不支持");
    InvalidateRect(hwnd, nullptr, TRUE);
    return;
  }
  SetWindowTextW(hwnd, name.c_str());
  InvalidateRect(hwnd, nullptr, TRUE);
  SendMessageW(GetParent(hwnd), WM_COMMAND, MAKEWPARAM(kIdHotkeyCaptured, 1),
               reinterpret_cast<LPARAM>(hwnd));
}

void InstallKeyboardHook(HWND capture) {
  if (g_keyboardHook) return;
  g_captureHwnd = capture;
  g_swallowKeyUp = 0;
  g_keyboardHook =
      SetWindowsHookExW(WH_KEYBOARD_LL,
                        [](int code, WPARAM wParam, LPARAM lParam) -> LRESULT {
                          if (code == HC_ACTION && g_captureHwnd) {
                            const auto* info = reinterpret_cast<KBDLLHOOKSTRUCT*>(lParam);
                            const UINT virtualKey = info->vkCode;
                            if (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN) {
                              if (!IsModifierVk(virtualKey) && virtualKey != VK_PROCESSKEY) {
                                UINT modifiers = 0;
                                if (GetAsyncKeyState(VK_CONTROL) & 0x8000) modifiers |= MOD_CONTROL;
                                if (GetAsyncKeyState(VK_MENU) & 0x8000) modifiers |= MOD_ALT;
                                if (GetAsyncKeyState(VK_SHIFT) & 0x8000) modifiers |= MOD_SHIFT;
                                if ((GetAsyncKeyState(VK_LWIN) | GetAsyncKeyState(VK_RWIN)) &
                                    0x8000) {
                                  modifiers |= MOD_WIN;
                                }
                                // 放行系统组合键，避免捕获时把 Alt+Tab / Alt+F4 / Esc 吞掉。
                                const bool systemCombo =
                                    virtualKey == VK_TAB || virtualKey == VK_ESCAPE ||
                                    (virtualKey == VK_F4 && (modifiers & MOD_ALT) != 0);
                                if (modifiers != 0 && !systemCombo) {
                                  PostMessageW(g_captureHwnd, kCaptureComboMessage, modifiers,
                                               virtualKey);
                                  g_swallowKeyUp = virtualKey;
                                  return 1;  // 吞掉这次按键，避免触发输入法/系统组合
                                }
                              }
                            } else if ((wParam == WM_KEYUP || wParam == WM_SYSKEYUP) &&
                                       g_swallowKeyUp != 0 && virtualKey == g_swallowKeyUp) {
                              g_swallowKeyUp = 0;
                              return 1;
                            }
                          }
                          return CallNextHookEx(nullptr, code, wParam, lParam);
                        },
                        GetModuleHandleW(nullptr), 0);
  if (!g_keyboardHook) {
    Log(L"热键捕获钩子安装失败: %lu（回退到普通按键消息）", GetLastError());
    g_captureHwnd = nullptr;
  }
}

void UninstallKeyboardHook() {
  if (g_keyboardHook) {
    UnhookWindowsHookEx(g_keyboardHook);
    g_keyboardHook = nullptr;
  }
  g_captureHwnd = nullptr;
  g_swallowKeyUp = 0;
}

int ThemeIndexFromName(const std::wstring& name) {
  if (name == L"dark") return 1;
  if (name == L"light") return 2;
  return 0;
}

std::wstring Trim(const std::wstring& value) {
  const size_t first = value.find_first_not_of(L" \t\r\n");
  if (first == std::wstring::npos) return {};
  const size_t last = value.find_last_not_of(L" \t\r\n");
  return value.substr(first, last - first + 1);
}

std::wstring TrimTrailingSlashes(std::wstring value) {
  while (value.size() > 1 && (value.back() == L'\\' || value.back() == L'/')) value.pop_back();
  return value;
}

std::wstring NormalizeDirectory(const std::wstring& value) { return TrimTrailingSlashes(Trim(value)); }

bool IsExistingDirectory(const std::wstring& path) {
  const DWORD attributes = GetFileAttributesW(path.c_str());
  return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

std::wstring ThemeNameFromIndex(int index) {
  if (index == 1) return L"dark";
  if (index == 2) return L"light";
  return L"system";
}

int AccentIndexFromName(const std::wstring& name) { return name == L"system" ? 1 : 0; }

std::wstring AccentNameFromIndex(int index) { return index == 1 ? L"system" : L""; }

LRESULT CALLBACK HotkeyCaptureProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
  switch (message) {
    case kCaptureComboMessage:
      ApplyCapturedCombo(hwnd, static_cast<UINT>(wParam), static_cast<UINT>(lParam));
      return 0;
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN: {
      const UINT virtualKey = static_cast<UINT>(wParam);
      // 输入法处理按键时窗口只会收到 VK_PROCESSKEY，忽略以免拼出 "Ctrl" 这类残缺组合。
      if (IsModifierVk(virtualKey) || virtualKey == VK_PROCESSKEY || virtualKey == VK_ESCAPE) {
        return 0;
      }
      UINT modifiers = 0;
      if (GetKeyState(VK_CONTROL) & 0x8000) modifiers |= MOD_CONTROL;
      if (GetKeyState(VK_MENU) & 0x8000) modifiers |= MOD_ALT;
      if (GetKeyState(VK_SHIFT) & 0x8000) modifiers |= MOD_SHIFT;
      if ((GetKeyState(VK_LWIN) & 0x8000) || (GetKeyState(VK_RWIN) & 0x8000)) modifiers |= MOD_WIN;
      ApplyCapturedCombo(hwnd, modifiers, virtualKey);
      return 0;
    }
    case WM_LBUTTONDOWN:
      SetFocus(hwnd);
      SetWindowTextW(hwnd, L"请按下组合键…");
      InvalidateRect(hwnd, nullptr, TRUE);
      return 0;
    case WM_SETFOCUS:
      SetWindowTextW(hwnd, L"请按下组合键…");
      InstallKeyboardHook(hwnd);
      InvalidateRect(hwnd, nullptr, TRUE);
      return 0;
    case WM_KILLFOCUS:
      UninstallKeyboardHook();
      InvalidateRect(hwnd, nullptr, TRUE);
      return 0;
    case WM_SETFONT:
      SetWindowLongPtrW(hwnd, GWLP_USERDATA, static_cast<LONG_PTR>(wParam));
      InvalidateRect(hwnd, nullptr, TRUE);
      return 0;
    case WM_GETDLGCODE:
      return DLGC_WANTALLKEYS;
    case WM_NCDESTROY:
      UninstallKeyboardHook();
      break;
    case WM_PAINT: {
      PAINTSTRUCT ps;
      HDC dc = BeginPaint(hwnd, &ps);
      RECT rect{};
      GetClientRect(hwnd, &rect);
      HBRUSH background = CreateSolidBrush(kControl);
      FillRect(dc, &rect, background);
      DeleteObject(background);
      const bool focused = GetFocus() == hwnd;
      HPEN pen = CreatePen(PS_SOLID, 1, focused ? kAccent : kBorder);
      HGDIOBJ oldPen = SelectObject(dc, pen);
      HGDIOBJ oldBrush = SelectObject(dc, GetStockObject(NULL_BRUSH));
      RoundRect(dc, rect.left, rect.top, rect.right - 1, rect.bottom - 1, 6, 6);
      SelectObject(dc, oldBrush);
      SelectObject(dc, oldPen);
      DeleteObject(pen);

      wchar_t text[128] = {};
      GetWindowTextW(hwnd, text, ARRAYSIZE(text));
      SetBkMode(dc, TRANSPARENT);
      SetTextColor(dc, kText);
      auto font = reinterpret_cast<HFONT>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
      HGDIOBJ oldFont = font ? SelectObject(dc, font) : nullptr;
      DrawTextW(dc, text, -1, &rect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
      if (oldFont) SelectObject(dc, oldFont);
      EndPaint(hwnd, &ps);
      return 0;
    }
    default:
      break;
  }
  return DefWindowProcW(hwnd, message, wParam, lParam);
}

void RegisterCaptureClass(HINSTANCE instance) {
  WNDCLASSEXW wc{};
  wc.cbSize = sizeof(wc);
  wc.lpfnWndProc = HotkeyCaptureProc;
  wc.hInstance = instance;
  wc.lpszClassName = kCaptureClass;
  wc.hCursor = LoadCursorW(nullptr, IDC_HAND);
  RegisterClassExW(&wc);
}

}  // namespace

SettingsWindow::~SettingsWindow() {
  if (hwnd_) DestroyWindow(hwnd_);
  if (font_) DeleteObject(font_);
  if (backgroundBrush_) DeleteObject(backgroundBrush_);
  if (controlBrush_) DeleteObject(controlBrush_);
}

int SettingsWindow::Scale(int value) const { return MulDiv(value, dpi_, 96); }

bool SettingsWindow::Show(HINSTANCE instance, HWND owner, const AppSettings& current,
                          Callbacks callbacks) {
  settings_ = current;
  roots_ = current.fileRoots;
  callbacks_ = std::move(callbacks);

  if (hwnd_) {
    ShowWindow(hwnd_, SW_SHOW);
    SetForegroundWindow(hwnd_);
    Populate();
    return true;
  }

  WNDCLASSEXW wc{};
  wc.cbSize = sizeof(wc);
  wc.lpfnWndProc = Thunk;
  wc.hInstance = instance;
  wc.lpszClassName = kClassName;
  wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
    Log(L"设置窗注册失败: %lu", GetLastError());
    return false;
  }
  RegisterCaptureClass(instance);

  owner_ = owner;
  POINT cursor{};
  GetCursorPos(&cursor);
  const RECT work = WorkAreaForPoint(cursor);
  dpi_ = DpiForPoint(cursor);
  const int width = Scale(kWindowWidth);
  const int height = Scale(kWindowHeight);
  const int x = work.left + ((work.right - work.left) - width) / 2;
  const int y = work.top + ((work.bottom - work.top) - height) / 2;

  hwnd_ = CreateWindowExW(WS_EX_DLGMODALFRAME, kClassName, L"Lume 设置",
                          WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_VISIBLE, x, y, width,
                          height, owner, nullptr, instance, this);
  if (!hwnd_) {
    Log(L"设置窗创建失败: %lu", GetLastError());
    return false;
  }
  CreateControls();
  Populate();
  SetForegroundWindow(hwnd_);
  RECT windowRect{};
  GetWindowRect(hwnd_, &windowRect);
  Log(L"设置窗已打开: %d,%d %dx%d", windowRect.left, windowRect.top,
      windowRect.right - windowRect.left, windowRect.bottom - windowRect.top);
  return true;
}

LRESULT CALLBACK SettingsWindow::Thunk(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
  if (message == WM_NCCREATE) {
    auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
    auto* self = static_cast<SettingsWindow*>(create->lpCreateParams);
    self->hwnd_ = hwnd;
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
  }
  auto* self = reinterpret_cast<SettingsWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  if (self && self->hwnd_ == hwnd) return self->Handle(message, wParam, lParam);
  return DefWindowProcW(hwnd, message, wParam, lParam);
}

void SettingsWindow::CreateControls() {
  if (font_) DeleteObject(font_);
  if (backgroundBrush_) DeleteObject(backgroundBrush_);
  if (controlBrush_) DeleteObject(controlBrush_);
  font_ = CreateFontW(-MulDiv(10, dpi_, 72), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                      DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                      DEFAULT_PITCH, L"Segoe UI Variable Text");
  backgroundBrush_ = CreateSolidBrush(kBackground);
  controlBrush_ = CreateSolidBrush(kControl);

  auto applyFont = [&](HWND control) {
    SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
  };
  auto label = [&](const wchar_t* text, int x, int y, int w) {
    HWND control = CreateWindowExW(0, L"STATIC", text, WS_CHILD | WS_VISIBLE | SS_LEFT, Scale(x),
                                   Scale(y), Scale(w), Scale(20), hwnd_, nullptr, nullptr, nullptr);
    applyFont(control);
    return control;
  };
  auto button = [&](const wchar_t* text, int id, int x, int y, int w, int h) {
    HWND control = CreateWindowExW(0, L"BUTTON", text,
                                   WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW, Scale(x),
                                   Scale(y), Scale(w), Scale(h), hwnd_,
                                   reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), nullptr,
                                   nullptr);
    applyFont(control);
    return control;
  };

  auto addGeneral = [this](HWND control) {
    generalControls_.push_back(control);
    return control;
  };
  auto addIndex = [this](HWND control) {
    indexControls_.push_back(control);
    return control;
  };
  // 只在「本地索引」来源下可用的控件：Everything 模式下整组禁用。
  auto addIndexLocal = [this](HWND control) {
    indexControls_.push_back(control);
    localOnlyControls_.push_back(control);
    return control;
  };
  auto addAbout = [this](HWND control) {
    aboutControls_.push_back(control);
    return control;
  };

  // 页签（共用，不属于任何一页）
  tabGeneral_ = button(L"常规", kIdTabGeneral, 20, 12, 84, 30);
  tabIndex_ = button(L"索引", kIdTabIndex, 108, 12, 84, 30);
  tabAbout_ = button(L"关于", kIdTabAbout, 196, 12, 84, 30);

  // ---- 常规页 ----
  addGeneral(label(L"主题", 20, 70, 80));
  themeButton_ = addGeneral(button(L"跟随系统", kIdTheme, 110, 64, 300, 30));
  addGeneral(label(L"强调色", 20, 108, 80));
  accentButton_ = addGeneral(button(L"设计蓝 #6EA8FF", kIdAccent, 110, 102, 300, 30));
  addGeneral(label(L"热键", 20, 146, 80));
  hotkeyCapture_ = addGeneral(CreateWindowExW(
      0, kCaptureClass, L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP, Scale(110), Scale(142),
      Scale(300), Scale(30), hwnd_,
      reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdHotkeyCaptured)), nullptr, nullptr));
  applyFont(hotkeyCapture_);

  // BS_AUTOCHECKBOX 与 BS_OWNERDRAW 共用低位类型字段：两者按位或后类型变成 BS_OWNERDRAW，
  // 控件既不再自动切换勾选，也不再响应 BM_GETCHECK/BM_SETCHECK。所以这里只用 BS_OWNERDRAW，
  // 勾选状态由 settings_.autostart 维护（见 Handle 的 WM_COMMAND 与 DrawButton）。
  autostartCheck_ = addGeneral(CreateWindowExW(
      0, L"BUTTON", L"开机自动启动", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
      Scale(20), Scale(184), Scale(300), Scale(26), hwnd_,
      reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdAutostart)), nullptr, nullptr));
  applyFont(autostartCheck_);
  addGeneral(CreateWindowExW(
      0, L"STATIC",
      L"设置保存后立即生效；搜索热键在任何窗口下都可用。\n"
      L"索引大小上限、快照目录、清除 / 重建索引都在「索引」页；版本与数据目录在「关于」页。",
      WS_CHILD | WS_VISIBLE | SS_LEFT | SS_NOPREFIX, Scale(20), Scale(228), Scale(500),
      Scale(48), hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdGeneralHint)), nullptr,
      nullptr));

  // ---- 索引页 ----
  // 文件搜索来源：Lume 本地索引 <-> Everything（点了只改待应用值，应用/保存才生效）
  addIndex(label(L"文件搜索来源", 20, 64, 90));
  searchSourceButton_ = addIndex(button(L"Lume 本地索引", kIdFileSearch, 110, 60, 300, 30));

  addIndexLocal(label(L"索引大小", 20, 100, 80));
  capEdit_ = addIndexLocal(CreateWindowExW(
      WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_NUMBER | ES_AUTOHSCROLL,
      Scale(110), Scale(96), Scale(120), Scale(26), hwnd_,
      reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdCap)), nullptr, nullptr));
  applyFont(capEdit_);
  SendMessageW(capEdit_, EM_SETLIMITTEXT, 6, 0);
  addIndexLocal(label(L"MB（64–262144，默认 512）", 242, 102, 280));

  addIndexLocal(label(L"索引文件目录", 20, 136, 90));
  // 下拉框（CBS_DROPDOWNLIST）：不能手打，只能从「当前目录 / 默认目录 / 最近 5 个仍然存在的目录」里选。
  indexDirCombo_ = addIndexLocal(CreateWindowExW(
      0, L"COMBOBOX", nullptr,
      WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWNLIST | CBS_OWNERDRAWFIXED |
          CBS_HASSTRINGS,
      Scale(110), Scale(132), Scale(390), Scale(200), hwnd_,
      reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdIndexPath)), nullptr, nullptr));
  applyFont(indexDirCombo_);
  SetWindowTheme(indexDirCombo_, L"DarkMode_CFD", nullptr);
  // 目录相关的三个动作紧跟在输入框下面一行。
  addIndexLocal(button(L"修改目录", kIdBrowseIndex, 110, 166, 92, 26));
  addIndexLocal(button(L"打开索引目录", kIdOpenIndexDir, 208, 166, 120, 26));
  addIndexLocal(button(L"恢复默认", kIdIndexDefault, 334, 166, 88, 26));

  indexInfo_ = addIndex(CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_LEFT | SS_NOPREFIX,
                                     Scale(20), Scale(202), Scale(500), Scale(72), hwnd_,
                                     reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdIndexInfo)),
                                     nullptr, nullptr));
  applyFont(indexInfo_);

  addIndexLocal(label(L"文件索引目录（留空 = 桌面 / 文档 / 下载）", 20, 280, 460));
  rootsList_ = addIndexLocal(CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", nullptr,
                                     WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL |
                                         LBS_NOINTEGRALHEIGHT | LBS_NOTIFY,
                                     Scale(20), Scale(302), Scale(370), Scale(104), hwnd_,
                                     reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdRoots)),
                                     nullptr, nullptr));
  applyFont(rootsList_);
  SetWindowTheme(rootsList_, L"DarkMode_Explorer", nullptr);
  addIndexLocal(button(L"添加…", kIdAdd, 406, 302, 100, 30));
  addIndexLocal(button(L"移除", kIdRemove, 406, 338, 100, 30));
  clearIndexButton_ = addIndexLocal(button(L"清除索引", kIdClearIndex, 20, 414, 104, 30));
  rebuildIndexButton_ = addIndexLocal(button(L"重建索引", kIdRebuildIndex, 132, 414, 104, 30));
  addIndex(button(L"刷新状态", kIdIndexRefresh, 244, 414, 100, 30));

  // ---- 关于页 ----
  aboutInfo_ = addAbout(CreateWindowExW(0, L"STATIC", L"", WS_CHILD | SS_LEFT | SS_NOPREFIX,
                                        Scale(20), Scale(64), Scale(500), Scale(330), hwnd_,
                                        nullptr, nullptr, nullptr));
  applyFont(aboutInfo_);
  copyInfoButton_ = addAbout(button(L"复制版本信息", kIdCopyInfo, 20, 412, 140, 30));
  aboutHint_ = addAbout(CreateWindowExW(
      0, L"STATIC", L"含版本、系统、Everything 状态与数据目录", WS_CHILD | SS_LEFT | SS_NOPREFIX,
      Scale(170), Scale(418), Scale(340), Scale(20), hwnd_, nullptr, nullptr, nullptr));
  applyFont(aboutHint_);

  // ---- 共用 ----
  status_ = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_LEFT, Scale(20),
                            Scale(454), Scale(500), Scale(40), hwnd_,
                            reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdStatus)), nullptr,
                            nullptr);
  applyFont(status_);

  button(L"应用", kIdApply, 208, 498, 84, 32);
  button(L"保存", kIdSave, 300, 498, 100, 32);
  button(L"取消", kIdCancel, 410, 498, 96, 32);

  const BOOL dark = TRUE;
  DwmSetWindowAttribute(hwnd_, 20 /*DWMWA_USE_IMMERSIVE_DARK_MODE*/, &dark, sizeof(dark));
}

void SettingsWindow::Populate() {
  themeIndex_ = ThemeIndexFromName(settings_.theme);
  accentIndex_ = AccentIndexFromName(settings_.accent);
  const wchar_t* themeNames[] = {L"跟随系统", L"深色", L"浅色"};
  SetWindowTextW(themeButton_, themeNames[themeIndex_]);
  SetWindowTextW(accentButton_, accentIndex_ == 1 ? L"跟随系统强调色" : L"设计蓝 #6EA8FF");

  SetWindowTextW(hotkeyCapture_, settings_.hotkey.c_str());
  hotkeyConflict_ = false;
  pendingFileSearch_ = _wcsicmp(settings_.fileSearch.c_str(), L"everything") == 0 ? L"everything"
                                                                                   : L"local";
  SetWindowTextW(capEdit_, std::to_wstring(settings_.indexSizeMB).c_str());
  // 索引目录下拉框：当前生效目录 + 默认目录 + 最近使用过的目录（已不存在的会被过滤掉）。
  indexDirHistory_ = settings_.indexDirHistory;
  std::wstring currentDir = settings_.indexDir;
  if (currentDir.empty() && callbacks_.queryIndex) {
    const IndexStatus status = callbacks_.queryIndex();
    currentDir = status.directory.empty() ? status.defaultDirectory : status.directory;
  }
  RebuildIndexDirChoices(currentDir);
  // 该控件是 BS_OWNERDRAW：BM_SETCHECK/BM_GETCHECK 对它无效，勾选状态直接取自设置。
  InvalidateRect(autostartCheck_, nullptr, FALSE);

  SendMessageW(rootsList_, LB_RESETCONTENT, 0, 0);
  for (const std::wstring& root : roots_) {
    SendMessageW(rootsList_, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(root.c_str()));
  }
  SetWindowTextW(status_, L"");
  SetPage(0);
  UpdateSearchSourceLayout();
  RefreshIndexInfo();
  RefreshAboutInfo();
}

void SettingsWindow::UpdateSearchSourceLayout() {
  const bool local = _wcsicmp(pendingFileSearch_.c_str(), L"everything") != 0;
  for (HWND control : localOnlyControls_) {
    if (control) EnableWindow(control, local ? TRUE : FALSE);
  }
  if (searchSourceButton_) {
    SetWindowTextW(searchSourceButton_, local ? L"Lume 本地索引" : L"Everything 全盘索引");
    InvalidateRect(searchSourceButton_, nullptr, FALSE);
  }
}

void SettingsWindow::SetPage(int page) {
  currentPage_ = (page == 1 || page == 2) ? page : 0;
  for (HWND control : generalControls_) {
    ShowWindow(control, currentPage_ == 0 ? SW_SHOW : SW_HIDE);
  }
  for (HWND control : indexControls_) {
    ShowWindow(control, currentPage_ == 1 ? SW_SHOW : SW_HIDE);
  }
  for (HWND control : aboutControls_) {
    ShowWindow(control, currentPage_ == 2 ? SW_SHOW : SW_HIDE);
  }
  InvalidateRect(tabGeneral_, nullptr, FALSE);
  InvalidateRect(tabIndex_, nullptr, FALSE);
  InvalidateRect(tabAbout_, nullptr, FALSE);
  // 只在这一刻读一次状态，不自动轮询（避免状态块一直闪）。
  if (currentPage_ == 1) RefreshIndexInfo();
  if (currentPage_ == 2) RefreshAboutInfo();
}

std::wstring SettingsWindow::BuildAboutText() const {
  AboutInfo info;
  if (callbacks_.queryAbout) info = callbacks_.queryAbout();
  if (info.version.empty()) info.version = L"?";
  if (info.arch.empty()) info.arch = L"x64";

  const std::wstring search =
      _wcsicmp(info.fileSearch.c_str(), L"everything") == 0 ? L"Everything 全盘索引"
                                                            : L"Lume 本地索引";
  std::wstring text;
  text += L"Lume " + info.version + L"（免安装单 exe，静态链接）\n";
  text += L"\n";
  text += L"运行环境　" + info.os + L" · " + info.arch;
  if (!info.dpi.empty()) text += L" · DPI " + info.dpi;
  text += L"\n";
  text += L"搜索热键　" + info.hotkey + L"\n";
  text += L"文件搜索　" + search + L"\n";
  text += L"Everything　" + (info.everything.empty() ? L"未使用" : info.everything) + L"\n";
  text += L"\n";
  text += L"设置文件　" + info.settingsPath + L"\n";
  text += L"使用记录　" + info.usagePath + L"\n";
  text += L"日志　　　" + info.logPath + L"\n";
  text += L"索引目录　" + info.indexDir + L"\n";
  text += L"\n";
  text += L"组件　Direct2D / DirectWrite / DirectComposition；Everything SDK v2 源码内嵌\n";
  text += L"许可　Everything SDK © David Carpenter（MIT）；未链接 PCRE\n";
  return text;
}

void SettingsWindow::RefreshAboutInfo() {
  if (!aboutInfo_) return;
  SetWindowTextW(aboutInfo_, BuildAboutText().c_str());
}

void SettingsWindow::CopyAboutInfo() {
  const std::wstring text = BuildAboutText();
  if (!OpenClipboard(hwnd_)) {
    SetWindowTextW(status_, L"复制失败：剪贴板被占用，请稍后重试");
    return;
  }
  EmptyClipboard();
  const size_t bytes = (text.size() + 1) * sizeof(wchar_t);
  HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
  if (memory) {
    if (void* target = GlobalLock(memory)) {
      memcpy(target, text.c_str(), bytes);
      GlobalUnlock(memory);
      if (!SetClipboardData(CF_UNICODETEXT, memory)) {
        GlobalFree(memory);
        memory = nullptr;
      }
    } else {
      GlobalFree(memory);
      memory = nullptr;
    }
  }
  CloseClipboard();
  SetWindowTextW(status_, memory ? L"版本信息已复制到剪贴板" : L"复制失败：内存不足");
}

void SettingsWindow::RefreshIndexInfo() {
  if (!indexInfo_) return;
  IndexStatus status;
  if (callbacks_.queryIndex) {
    status = callbacks_.queryIndex();
  } else {
    status.limitMB = settings_.indexSizeMB;
  }

  const bool everythingMode = _wcsicmp(pendingFileSearch_.c_str(), L"everything") == 0;
  const bool snapshotPresent = status.snapshotBytes > 0;
  const double snapshotMb = static_cast<double>(status.snapshotBytes) / (1024.0 * 1024.0);

  if (everythingMode) {
    // Everything 模式：显示连接状态与本地快照去留，不再显示本地索引详情。
    EverythingStatus everything;
    if (callbacks_.queryEverything) everything = callbacks_.queryEverything();

    wchar_t value[512] = {};
    if (!everything.available) {
      std::wstring reason = everything.error;
      if (reason.empty()) reason = L"未检测到正在运行的 Everything";
      swprintf(value, ARRAYSIZE(value), L"连接：%ls", reason.c_str());
    } else if (!everything.dbLoaded) {
      swprintf(value, ARRAYSIZE(value), L"连接：已连接 · Everything %ls（索引未就绪，稍后自动可用）",
               everything.version.c_str());
    } else {
      swprintf(value, ARRAYSIZE(value), L"连接：已连接 · Everything %ls · 索引就绪",
               everything.version.c_str());
    }

    std::wstring text = L"来源：Everything（全盘索引，不使用本地索引）\n";
    text += value;
    text += L"\n";
    if (snapshotPresent) {
      wchar_t snapshotLine[192] = {};
      swprintf(snapshotLine, ARRAYSIZE(snapshotLine),
               L"本地快照：保留 %.1f MB（连上 Everything 后删除）", snapshotMb);
      text += snapshotLine;
    } else {
      text += L"本地快照：已删除（此模式不写本地索引）";
    }
    text += L"\n提示：切回「本地索引」需要重建（512 MB 预算约 20.8 s）。";
    SetWindowTextW(indexInfo_, text.c_str());
    return;
  }

  const double megabytes = snapshotMb;
  const double percent = status.limitMB > 0 ? 100.0 * megabytes / status.limitMB : 0.0;
  wchar_t sizeLine[256] = {};
  if (status.snapshotBytes == 0) {
    swprintf(sizeLine, ARRAYSIZE(sizeLine), L"大小：尚未生成快照（上限 %d MB）", status.limitMB);
  } else {
    swprintf(sizeLine, ARRAYSIZE(sizeLine),
             L"大小：%.1f MB（上限 %d MB，占用 %.1f%%）　条目：%zu 条", megabytes, status.limitMB,
             percent, status.entries);
  }

  wchar_t stateLine[192] = {};
  if (status.rebuilding) {
    swprintf(stateLine, ARRAYSIZE(stateLine), L"状态：正在重建索引…　条目：%zu 条", status.entries);
  } else if (status.lastRebuildDurationMs > 0) {
    swprintf(stateLine, ARRAYSIZE(stateLine), L"状态：空闲（上次重建 %.1f s）",
             static_cast<double>(status.lastRebuildDurationMs) / 1000.0);
  } else {
    wcscpy_s(stateLine, ARRAYSIZE(stateLine), L"状态：空闲");
  }

  std::wstring text = L"来源：Lume 本地索引　快照：files.idx";
  if (!status.snapshotWritable) text += L"（目录不可用）";
  text += L"\n";
  text += sizeLine;
  text += L"\n";
  text += stateLine;
  text += L"\n";
  if (!status.directory.empty()) {
    text += L"目录：" + status.directory;
  } else if (!status.defaultDirectory.empty()) {
    text += L"目录：默认（" + status.defaultDirectory + L"）";
  } else {
    text += L"目录：默认（%LOCALAPPDATA%\\Lume）";
  }
  if (!status.directory.empty() && !status.defaultDirectory.empty() &&
      _wcsicmp(status.directory.c_str(), status.defaultDirectory.c_str()) == 0) {
    text += L"（默认）";
  }
  SetWindowTextW(indexInfo_, text.c_str());
}

bool SettingsWindow::ValidateIndexDirectory(const std::wstring& path, std::wstring* error) const {
  if (PathIsRelativeW(path.c_str())) {
    if (error) *error = L"索引文件目录需要绝对路径";
    return false;
  }
  const DWORD attributes = GetFileAttributesW(path.c_str());
  if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
    if (error) *error = L"索引文件目录指向的是文件，请选择文件夹";
    return false;
  }
  if (!EnsureDirectory(path)) {
    if (error) *error = L"索引文件目录无法创建：" + path;
    return false;
  }
  const std::wstring probe = path + L"\\.lume-write-test.tmp";
  HANDLE file = CreateFileW(probe.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                            FILE_FLAG_DELETE_ON_CLOSE, nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    if (error) *error = L"索引文件目录不可写：" + path;
    return false;
  }
  CloseHandle(file);
  return true;
}

std::wstring SettingsWindow::PickFolder(const wchar_t* title) {
  BROWSEINFOW info{};
  info.hwndOwner = hwnd_;
  info.lpszTitle = title;
  info.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
  PIDLIST_ABSOLUTE item = SHBrowseForFolderW(&info);
  if (!item) return {};
  wchar_t path[MAX_PATH] = {};
  std::wstring result;
  if (SHGetPathFromIDListW(item, path)) result = path;
  CoTaskMemFree(item);
  return result;
}

void SettingsWindow::AddRootViaDialog() {
  const std::wstring picked = PickFolder(L"选择要索引的文件夹");
  if (picked.empty()) return;
  roots_.push_back(picked);
  SendMessageW(rootsList_, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(picked.c_str()));
}

void SettingsWindow::RemoveSelectedRoot() {
  const LRESULT selection = SendMessageW(rootsList_, LB_GETCURSEL, 0, 0);
  if (selection == LB_ERR) return;
  SendMessageW(rootsList_, LB_DELETESTRING, static_cast<WPARAM>(selection), 0);
  if (selection < static_cast<LRESULT>(roots_.size())) roots_.erase(roots_.begin() + selection);
}

std::wstring SettingsWindow::DefaultIndexDir() const {
  if (!callbacks_.queryIndex) return {};
  return NormalizeDirectory(callbacks_.queryIndex().defaultDirectory);
}

std::wstring SettingsWindow::SelectedIndexDir() const {
  if (!indexDirCombo_) return {};
  const LRESULT selection = SendMessageW(indexDirCombo_, CB_GETCURSEL, 0, 0);
  if (selection == CB_ERR) return {};
  const size_t index = static_cast<size_t>(selection);
  return index < indexDirChoices_.size() ? indexDirChoices_[index] : std::wstring();
}

void SettingsWindow::RememberIndexDir(const std::wstring& directory) {
  const std::wstring path = NormalizeDirectory(directory);
  if (path.empty()) return;
  std::vector<std::wstring> history;
  history.push_back(path);
  for (const std::wstring& entry : indexDirHistory_) {
    if (history.size() >= 5) break;
    const std::wstring existing = NormalizeDirectory(entry);
    if (existing.empty() || _wcsicmp(existing.c_str(), path.c_str()) == 0) continue;
    history.push_back(existing);
  }
  indexDirHistory_ = history;
}

void SettingsWindow::RebuildIndexDirChoices(const std::wstring& selected) {
  if (!indexDirCombo_) return;
  const std::wstring selectedPath = NormalizeDirectory(selected);
  const std::wstring defaultDir = DefaultIndexDir();

  indexDirChoices_.clear();
  auto add = [&](const std::wstring& value) {
    const std::wstring path = NormalizeDirectory(value);
    if (path.empty()) return;
    for (const std::wstring& existing : indexDirChoices_) {
      if (_wcsicmp(existing.c_str(), path.c_str()) == 0) return;
    }
    indexDirChoices_.push_back(path);
  };

  add(selectedPath);                       // 当前值（目录缺失时也要显示）
  add(defaultDir);                         // 默认目录，随时可以切回
  for (const std::wstring& entry : indexDirHistory_) {
    if (IsExistingDirectory(NormalizeDirectory(entry))) add(entry);  // 已不存在的不进下拉
  }

  SendMessageW(indexDirCombo_, CB_RESETCONTENT, 0, 0);
  int selection = 0;
  for (size_t i = 0; i < indexDirChoices_.size(); ++i) {
    SendMessageW(indexDirCombo_, CB_ADDSTRING, 0,
                 reinterpret_cast<LPARAM>(indexDirChoices_[i].c_str()));
    if (!selectedPath.empty() && _wcsicmp(indexDirChoices_[i].c_str(), selectedPath.c_str()) == 0) {
      selection = static_cast<int>(i);
    }
  }
  SendMessageW(indexDirCombo_, CB_SETCURSEL, static_cast<WPARAM>(selection), 0);
}

bool SettingsWindow::ApplyConfiguration(bool close) {
  AppSettings updated = settings_;
  updated.theme = ThemeNameFromIndex(themeIndex_);
  updated.accent = AccentNameFromIndex(accentIndex_);
  updated.fileSearch = pendingFileSearch_;

  wchar_t hotkeyText[128] = {};
  GetWindowTextW(hotkeyCapture_, hotkeyText, ARRAYSIZE(hotkeyText));
  HotkeyBinding binding;
  if (!ParseHotkey(hotkeyText, &binding)) {
    SetWindowTextW(status_, L"热键无效：需要至少一个修饰键（Ctrl/Alt/Shift/Win）与一个按键");
    return false;
  }
  if (hotkeyConflict_) {
    SetWindowTextW(status_, L"当前热键不可用，请重新捕获或换一个组合");
    return false;
  }
  updated.hotkey = binding.name;

  wchar_t capText[32] = {};
  GetWindowTextW(capEdit_, capText, ARRAYSIZE(capText));
  const long cap = wcstol(capText, nullptr, 10);
  if (cap < 64 || cap > 262144) {
    SetWindowTextW(status_, L"索引大小需在 64–262144 MB（最大 256GB）之间");
    return false;
  }
  updated.indexSizeMB = static_cast<int>(cap);
  // updated.autostart 已在点击「开机自动启动」时切换，无需再从控件读回勾选状态。

  std::wstring indexDir = SelectedIndexDir();
  const std::wstring defaultDir = DefaultIndexDir();
  if (!indexDir.empty()) {
    std::wstring error;
    if (!ValidateIndexDirectory(indexDir, &error)) {
      SetWindowTextW(status_, error.c_str());
      Log(L"索引目录校验失败: %ls", error.c_str());
      return false;
    }
    // 与默认目录相同时存空值，保持“跟随默认目录”的语义（默认目录随环境变化）。
    if (!defaultDir.empty() && _wcsicmp(indexDir.c_str(), defaultDir.c_str()) == 0) {
      indexDir.clear();
    }
  }
  updated.indexDir = indexDir;
  if (!indexDir.empty()) RememberIndexDir(indexDir);
  // 历史里已经不存在的目录不再持久化，避免占满 5 个位置。
  std::vector<std::wstring> history;
  for (const std::wstring& entry : indexDirHistory_) {
    const std::wstring path = NormalizeDirectory(entry);
    if (path.empty() || !IsExistingDirectory(path)) continue;
    if (history.size() >= 5) break;
    history.push_back(path);
  }
  updated.indexDirHistory = history;

  std::vector<std::wstring> roots;
  for (const std::wstring& root : roots_) {
    if (GetFileAttributesW(root.c_str()) != INVALID_FILE_ATTRIBUTES) roots.push_back(root);
  }
  updated.fileRoots = roots;

  settings_ = updated;
  if (callbacks_.onSave) callbacks_.onSave(settings_);
  RefreshIndexInfo();
  SetWindowTextW(status_, L"已应用");
  if (close) DestroyWindow(hwnd_);
  return true;
}

void SettingsWindow::DrawButton(const DRAWITEMSTRUCT* item) {
  if (!item) return;
  const int id = static_cast<int>(item->CtlID);
  HDC dc = item->hDC;
  RECT rect = item->rcItem;
  const bool pressed = (item->itemState & ODS_SELECTED) != 0;
  const bool disabled = (item->itemState & ODS_DISABLED) != 0;

  HBRUSH background = CreateSolidBrush(kBackground);
  FillRect(dc, &rect, background);
  DeleteObject(background);

  if (id == kIdAutostart) {
    // 自绘勾选框：勾选状态来自 settings_.autostart，未勾选时按下也立即用强调色描边，
    // 让点击有明确反馈（BS_OWNERDRAW 控件不会自己切换状态）。
    const bool checked = settings_.autostart;
    RECT box = rect;
    box.right = box.left + Scale(18);
    box.top = rect.top + (rect.bottom - rect.top - Scale(18)) / 2;
    box.bottom = box.top + Scale(18);

    HBRUSH boxFill = CreateSolidBrush(checked ? (pressed ? RGB(0x4C, 0x86, 0xDD) : kAccent)
                                              : kControl);
    HGDIOBJ oldBrush = SelectObject(dc, boxFill);
    HPEN pen = CreatePen(PS_SOLID, 1, (checked || pressed) ? kAccent : kBorder);
    HGDIOBJ oldPen = SelectObject(dc, pen);
    RoundRect(dc, box.left, box.top, box.right, box.bottom, 4, 4);
    SelectObject(dc, oldPen);
    SelectObject(dc, oldBrush);
    DeleteObject(pen);
    DeleteObject(boxFill);

    if (checked) {
      HPEN checkPen = CreatePen(PS_SOLID, 2, RGB(0x10, 0x18, 0x28));
      oldPen = SelectObject(dc, checkPen);
      MoveToEx(dc, box.left + 4, box.top + 9, nullptr);
      LineTo(dc, box.left + 8, box.top + 13);
      LineTo(dc, box.left + 14, box.top + 5);
      SelectObject(dc, oldPen);
      DeleteObject(checkPen);
    }
    RECT text = rect;
    text.left = box.right + Scale(10);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, disabled ? RGB(0x80, 0x80, 0x88) : kText);
    HGDIOBJ oldFont = SelectObject(dc, font_);
    wchar_t label[64] = {};
    GetWindowTextW(item->hwndItem, label, ARRAYSIZE(label));
    DrawTextW(dc, label, -1, &text, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    if (item->itemState & ODS_FOCUS) {
      RECT focus = text;
      DrawTextW(dc, label, -1, &focus, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_CALCRECT);
      InflateRect(&focus, Scale(2), Scale(1));
      DrawFocusRect(dc, &focus);
    }
    SelectObject(dc, oldFont);
    return;
  }

  if (id == kIdTabGeneral || id == kIdTabIndex || id == kIdTabAbout) {
    // 页签：选中项文字高亮 + 底部强调色指示条，未选中为暗灰，自绘以保持深色一致。
    int tab = 1;
    if (id == kIdTabGeneral) tab = 0;
    else if (id == kIdTabAbout) tab = 2;
    const bool active = currentPage_ == tab;
    wchar_t label[32] = {};
    GetWindowTextW(item->hwndItem, label, ARRAYSIZE(label));
    RECT text = rect;
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, active ? kText : kMuted);
    HGDIOBJ oldFont = SelectObject(dc, font_);
    DrawTextW(dc, label, -1, &text, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    SelectObject(dc, oldFont);
    if (active) {
      RECT bar = rect;
      bar.top = rect.bottom - Scale(2);
      HBRUSH fill = CreateSolidBrush(kAccent);
      FillRect(dc, &bar, fill);
      DeleteObject(fill);
    }
    return;
  }

  const bool primary = (id == kIdSave) && !disabled;
  HBRUSH fill = CreateSolidBrush(disabled ? kBackground
                                          : (primary ? (pressed ? RGB(0x4C, 0x86, 0xDD) : kAccent)
                                                     : (pressed ? RGB(0x22, 0x22, 0x28) : kControl)));
  HGDIOBJ oldBrush = SelectObject(dc, fill);
  HPEN pen = CreatePen(PS_SOLID, 1, primary ? kAccent : kBorder);
  HGDIOBJ oldPen = SelectObject(dc, pen);
  RoundRect(dc, rect.left, rect.top, rect.right - 1, rect.bottom - 1, 6, 6);
  SelectObject(dc, oldPen);
  SelectObject(dc, oldBrush);
  DeleteObject(pen);
  DeleteObject(fill);

  wchar_t text[64] = {};
  GetWindowTextW(item->hwndItem, text, ARRAYSIZE(text));
  SetBkMode(dc, TRANSPARENT);
  SetTextColor(dc, disabled ? kMuted : (primary ? RGB(0x10, 0x18, 0x28) : kText));
  HGDIOBJ oldFont = SelectObject(dc, font_);
  DrawTextW(dc, text, -1, &rect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
  SelectObject(dc, oldFont);
}

void SettingsWindow::DrawComboItem(const DRAWITEMSTRUCT* item) {
  if (!item) return;
  HDC dc = item->hDC;
  RECT rect = item->rcItem;
  const bool selected = (item->itemState & ODS_SELECTED) != 0;
  const bool disabled = (item->itemState & ODS_DISABLED) != 0;

  HBRUSH fill = CreateSolidBrush(disabled ? kBackground
                                          : (selected ? RGB(0x35, 0x3A, 0x46) : kControl));
  FillRect(dc, &rect, fill);
  DeleteObject(fill);

  wchar_t text[MAX_PATH * 2] = {};
  if (item->itemID != static_cast<UINT>(-1)) {
    SendMessageW(indexDirCombo_, CB_GETLBTEXT, static_cast<WPARAM>(item->itemID),
                 reinterpret_cast<LPARAM>(text));
  }
  RECT textRect = rect;
  textRect.left += Scale(6);
  textRect.right -= Scale(4);
  SetBkMode(dc, TRANSPARENT);
  SetTextColor(dc, disabled ? kMuted : kText);
  HGDIOBJ oldFont = SelectObject(dc, font_);
  DrawTextW(dc, text, -1, &textRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
  SelectObject(dc, oldFont);
}

LRESULT SettingsWindow::Handle(UINT message, WPARAM wParam, LPARAM lParam) {
  switch (message) {
    case WM_ERASEBKGND: {
      RECT rect{};
      GetClientRect(hwnd_, &rect);
      FillRect(reinterpret_cast<HDC>(wParam), &rect, backgroundBrush_);
      return 1;
    }
    case WM_CTLCOLORSTATIC: {
      HDC dc = reinterpret_cast<HDC>(wParam);
      const HWND control = reinterpret_cast<HWND>(lParam);
      SetTextColor(dc, GetDlgCtrlID(control) == kIdGeneralHint ? kMuted : kText);
      SetBkColor(dc, kBackground);
      SetBkMode(dc, TRANSPARENT);
      return reinterpret_cast<LRESULT>(backgroundBrush_);
    }
    case WM_CTLCOLORBTN: {
      HDC dc = reinterpret_cast<HDC>(wParam);
      SetTextColor(dc, kText);
      SetBkColor(dc, kBackground);
      return reinterpret_cast<LRESULT>(backgroundBrush_);
    }
    case WM_CTLCOLOREDIT: {
      HDC dc = reinterpret_cast<HDC>(wParam);
      SetTextColor(dc, kText);
      SetBkColor(dc, kControl);
      return reinterpret_cast<LRESULT>(controlBrush_);
    }
    case WM_CTLCOLORLISTBOX: {
      HDC dc = reinterpret_cast<HDC>(wParam);
      SetTextColor(dc, kText);
      SetBkColor(dc, kControl);
      return reinterpret_cast<LRESULT>(controlBrush_);
    }
    case WM_DRAWITEM: {
      const auto* item = reinterpret_cast<const DRAWITEMSTRUCT*>(lParam);
      if (item && item->CtlType == ODT_COMBOBOX) {
        DrawComboItem(item);
      } else {
        DrawButton(item);
      }
      return TRUE;
    }
    case WM_MEASUREITEM: {
      auto* measure = reinterpret_cast<MEASUREITEMSTRUCT*>(lParam);
      if (measure && measure->CtlType == ODT_COMBOBOX) {
        measure->itemHeight = static_cast<UINT>(Scale(24));
        return TRUE;
      }
      break;
    }
    case WM_COMMAND: {
      const int id = LOWORD(wParam);
      if (id == kIdTheme) {
        themeIndex_ = (themeIndex_ + 1) % 3;
        const wchar_t* names[] = {L"跟随系统", L"深色", L"浅色"};
        SetWindowTextW(themeButton_, names[themeIndex_]);
        InvalidateRect(themeButton_, nullptr, TRUE);
        return 0;
      }
      if (id == kIdAccent) {
        accentIndex_ = accentIndex_ == 1 ? 0 : 1;
        SetWindowTextW(accentButton_,
                       accentIndex_ == 1 ? L"跟随系统强调色" : L"设计蓝 #6EA8FF");
        InvalidateRect(accentButton_, nullptr, TRUE);
        return 0;
      }
      if (id == kIdAutostart) {
        settings_.autostart = !settings_.autostart;
        InvalidateRect(autostartCheck_, nullptr, FALSE);
        Log(L"设置窗自启开关: %ls", settings_.autostart ? L"开" : L"关");
        return 0;
      }
      if (id == kIdClearIndex) {
        if (callbacks_.onClearIndex) callbacks_.onClearIndex();
        SetWindowTextW(status_, L"已清除索引（未重建）：搜索暂时为空，点「重建索引」可立即重建");
        RefreshIndexInfo();
        return 0;
      }
      if (id == kIdRebuildIndex) {
        if (callbacks_.onRebuildIndex) callbacks_.onRebuildIndex();
        SetWindowTextW(status_, L"正在后台重建索引，完成后会写回快照…");
        RefreshIndexInfo();
        return 0;
      }
      if (id == kIdFileSearch) {
        pendingFileSearch_ = _wcsicmp(pendingFileSearch_.c_str(), L"everything") == 0
                                  ? L"local"
                                  : L"everything";
        UpdateSearchSourceLayout();
        RefreshIndexInfo();
        SetWindowTextW(status_, pendingFileSearch_ == L"everything"
                                    ? L"已切到 Everything（点「应用」或「保存」生效）：本地索引会被停用，"
                                      L"连上 Everything 后删除 files.idx"
                                    : L"已切回本地索引（点「应用」或「保存」生效）");
        return 0;
      }
      if (id == kIdIndexRefresh) {
        RefreshIndexInfo();
        SetWindowTextW(status_, L"索引状态已刷新");
        return 0;
      }
      if (id == kIdOpenIndexDir) {
        if (callbacks_.onOpenIndexDirectory) callbacks_.onOpenIndexDirectory();
        SetWindowTextW(status_, L"已打开索引文件目录");
        return 0;
      }
      if (id == kIdTabGeneral || id == kIdTabIndex || id == kIdTabAbout) {
        int page = 1;
        if (id == kIdTabGeneral) page = 0;
        else if (id == kIdTabAbout) page = 2;
        SetPage(page);
        SetWindowTextW(status_, L"");
        return 0;
      }
      if (id == kIdCopyInfo) {
        CopyAboutInfo();
        return 0;
      }
      if (id == kIdBrowseIndex) {
        const std::wstring picked = PickFolder(L"修改索引文件目录");
        if (!picked.empty()) {
          RememberIndexDir(picked);
          RebuildIndexDirChoices(picked);
          SetWindowTextW(status_, L"索引文件目录已改，点「应用」或「保存」生效");
        }
        return 0;
      }
      if (id == kIdIndexDefault) {
        const std::wstring defaults = DefaultIndexDir();
        if (defaults.empty()) return 0;
        RebuildIndexDirChoices(defaults);
        wchar_t message[512];
        swprintf(message, ARRAYSIZE(message), L"已切回默认索引目录（点「应用」或「保存」生效）：%ls",
                 defaults.c_str());
        SetWindowTextW(status_, message);
        return 0;
      }
      if (id == kIdIndexPath && HIWORD(wParam) == CBN_SELCHANGE) {
        SetWindowTextW(status_, L"索引文件目录已改，点「应用」或「保存」生效");
        return 0;
      }
      if (id == kIdApply) {
        ApplyConfiguration(false);
        return 0;
      }
      if (id == kIdSave) {
        ApplyConfiguration(true);
        return 0;
      }
      if (id == kIdCancel) {
        DestroyWindow(hwnd_);
        return 0;
      }
      if (id == kIdAdd) {
        AddRootViaDialog();
        return 0;
      }
      if (id == kIdRemove) {
        RemoveSelectedRoot();
        return 0;
      }
      if (id == kIdHotkeyCaptured) {
        wchar_t text[128] = {};
        GetWindowTextW(hotkeyCapture_, text, ARRAYSIZE(text));
        HotkeyBinding binding;
        if (!ParseHotkey(text, &binding)) {
          SetWindowTextW(status_, L"热键无效：需要至少一个修饰键与一个按键");
          hotkeyConflict_ = true;
          return 0;
        }
        if (binding.name == settings_.hotkey) {
          SetWindowTextW(status_, L"");
          hotkeyConflict_ = false;
          Log(L"热键捕获: %ls（当前值）", binding.name.c_str());
          return 0;
        }
        if (RegisterHotKey(hwnd_, 0x66, binding.modifiers | MOD_NOREPEAT, binding.virtualKey)) {
          UnregisterHotKey(hwnd_, 0x66);
          SetWindowTextW(status_, L"");
          hotkeyConflict_ = false;
          Log(L"热键捕获: %ls（可用）", binding.name.c_str());
        } else {
          wchar_t message[160];
          swprintf(message, ARRAYSIZE(message), L"%ls 已被其他程序占用，请换一个组合",
                   binding.name.c_str());
          SetWindowTextW(status_, message);
          hotkeyConflict_ = true;
          Log(L"热键捕获: %ls（被占用）", binding.name.c_str());
        }
        return 0;
      }
      break;
    }
    case WM_CLOSE:
      DestroyWindow(hwnd_);
      return 0;
    case WM_DESTROY:
      status_ = nullptr;
      themeButton_ = accentButton_ = hotkeyCapture_ = autostartCheck_ = rootsList_ = nullptr;
      capEdit_ = indexInfo_ = nullptr;
      indexDirCombo_ = nullptr;
      clearIndexButton_ = rebuildIndexButton_ = tabGeneral_ = tabIndex_ = nullptr;
      indexDirChoices_.clear();
      indexDirHistory_.clear();
      generalControls_.clear();
      indexControls_.clear();
      break;
    case WM_NCDESTROY:
      hwnd_ = nullptr;
      break;
    default:
      break;
  }
  return DefWindowProcW(hwnd_, message, wParam, lParam);
}
