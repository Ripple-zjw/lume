#include "ui/settings_window.h"

#include <commctrl.h>
#include <dwmapi.h>
#include <shlobj.h>
#include <uxtheme.h>

#include <algorithm>

#include "core/log.h"
#include "platform/hotkey.h"
#include "platform/system.h"

namespace {

constexpr wchar_t kClassName[] = L"eToolsSettingsWindow";
constexpr wchar_t kCaptureClass[] = L"eToolsHotkeyCapture";
constexpr int kWindowWidth = 540;
constexpr int kWindowHeight = 545;

constexpr COLORREF kBackground = RGB(0x20, 0x20, 0x24);
constexpr COLORREF kControl = RGB(0x2A, 0x2A, 0x30);
constexpr COLORREF kText = RGB(0xF0, 0xF0, 0xF2);
constexpr COLORREF kAccent = RGB(0x6E, 0xA8, 0xFF);
constexpr COLORREF kBorder = RGB(0x50, 0x50, 0x58);

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
                          SaveCallback onSave, std::function<void()> onClearIndex) {
  settings_ = current;
  roots_ = current.fileRoots;
  onSave_ = std::move(onSave);
  onClearIndex_ = std::move(onClearIndex);

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

  hwnd_ = CreateWindowExW(WS_EX_DLGMODALFRAME, kClassName, L"eTools 设置",
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

  label(L"主题", 20, 22, 80);
  themeButton_ = button(L"跟随系统", kIdTheme, 110, 16, 300, 30);
  label(L"强调色", 20, 60, 80);
  accentButton_ = button(L"设计蓝 #6EA8FF", kIdAccent, 110, 54, 300, 30);
  label(L"热键", 20, 98, 80);
  hotkeyCapture_ = CreateWindowExW(0, kCaptureClass, L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                                   Scale(110), Scale(94), Scale(300), Scale(30), hwnd_,
                                   reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdHotkeyCaptured)),
                                   nullptr, nullptr);
  applyFont(hotkeyCapture_);

  autostartCheck_ = CreateWindowExW(0, L"BUTTON", L"开机自动启动",
                                    WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX |
                                        BS_OWNERDRAW,
                                    Scale(20), Scale(136), Scale(300), Scale(26), hwnd_,
                                    reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdAutostart)),
                                    nullptr, nullptr);
  applyFont(autostartCheck_);

  label(L"索引上限", 20, 172, 80);
  capEdit_ = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                             WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_NUMBER | ES_AUTOHSCROLL,
                             Scale(110), Scale(168), Scale(140), Scale(26), hwnd_,
                             reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdCap)), nullptr,
                             nullptr);
  applyFont(capEdit_);
  SendMessageW(capEdit_, EM_SETLIMITTEXT, 7, 0);
  label(L"条（20,000–1,000,000）", 258, 174, 260);

  label(L"文件索引目录（留空 = 桌面 / 文档 / 下载）", 20, 208, 460);
  rootsList_ = CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", nullptr,
                               WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL |
                                   LBS_NOINTEGRALHEIGHT | LBS_NOTIFY,
                               Scale(20), Scale(232), Scale(370), Scale(150), hwnd_,
                               reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdRoots)), nullptr,
                               nullptr);
  applyFont(rootsList_);
  SetWindowTheme(rootsList_, L"DarkMode_Explorer", nullptr);
  button(L"添加…", kIdAdd, 406, 232, 100, 30);
  button(L"移除", kIdRemove, 406, 268, 100, 30);

  status_ = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_LEFT, Scale(20),
                            Scale(394), Scale(486), Scale(44), hwnd_,
                            reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdStatus)), nullptr,
                            nullptr);
  applyFont(status_);

  button(L"清除索引并重建", kIdClearIndex, 120, 462, 160, 32);
  button(L"保存", kIdSave, 300, 462, 100, 32);
  button(L"取消", kIdCancel, 410, 462, 96, 32);

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
  SetWindowTextW(capEdit_, std::to_wstring(settings_.fileCap).c_str());
  SendMessageW(autostartCheck_, BM_SETCHECK, settings_.autostart ? BST_CHECKED : BST_UNCHECKED, 0);

  SendMessageW(rootsList_, LB_RESETCONTENT, 0, 0);
  for (const std::wstring& root : roots_) {
    SendMessageW(rootsList_, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(root.c_str()));
  }
  SetWindowTextW(status_, L"");
}

void SettingsWindow::AddRootViaDialog() {
  BROWSEINFOW info{};
  info.hwndOwner = hwnd_;
  info.lpszTitle = L"选择要索引的文件夹";
  info.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
  PIDLIST_ABSOLUTE item = SHBrowseForFolderW(&info);
  if (!item) return;
  wchar_t path[MAX_PATH] = {};
  if (SHGetPathFromIDListW(item, path)) {
    roots_.push_back(path);
    SendMessageW(rootsList_, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(path));
  }
  CoTaskMemFree(item);
}

void SettingsWindow::RemoveSelectedRoot() {
  const LRESULT selection = SendMessageW(rootsList_, LB_GETCURSEL, 0, 0);
  if (selection == LB_ERR) return;
  SendMessageW(rootsList_, LB_DELETESTRING, static_cast<WPARAM>(selection), 0);
  if (selection < static_cast<LRESULT>(roots_.size())) roots_.erase(roots_.begin() + selection);
}

void SettingsWindow::OnSave() {
  settings_.theme = ThemeNameFromIndex(themeIndex_);
  settings_.accent = AccentNameFromIndex(accentIndex_);

  wchar_t hotkeyText[128] = {};
  GetWindowTextW(hotkeyCapture_, hotkeyText, ARRAYSIZE(hotkeyText));
  HotkeyBinding binding;
  if (!ParseHotkey(hotkeyText, &binding)) {
    SetWindowTextW(status_, L"热键无效：需要至少一个修饰键（Ctrl/Alt/Shift/Win）与一个按键");
    return;
  }
  settings_.hotkey = binding.name;
  if (hotkeyConflict_) {
    SetWindowTextW(status_, L"当前热键不可用，请重新捕获或换一个组合");
    return;
  }
  wchar_t capText[32] = {};
  GetWindowTextW(capEdit_, capText, ARRAYSIZE(capText));
  const long cap = wcstol(capText, nullptr, 10);
  if (cap < 20000 || cap > 1000000) {
    SetWindowTextW(status_, L"索引上限需在 20,000–1,000,000 之间");
    return;
  }
  settings_.fileCap = static_cast<int>(cap);
  settings_.autostart = SendMessageW(autostartCheck_, BM_GETCHECK, 0, 0) == BST_CHECKED;

  std::vector<std::wstring> roots;
  for (const std::wstring& root : roots_) {
    if (GetFileAttributesW(root.c_str()) != INVALID_FILE_ATTRIBUTES) roots.push_back(root);
  }
  settings_.fileRoots = roots;

  if (onSave_) onSave_(settings_);
  DestroyWindow(hwnd_);
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
    RECT box = rect;
    box.right = box.left + Scale(18);
    box.top = rect.top + (rect.bottom - rect.top - Scale(18)) / 2;
    box.bottom = box.top + Scale(18);
    HPEN pen = CreatePen(PS_SOLID, 1, kBorder);
    HGDIOBJ oldPen = SelectObject(dc, pen);
    HGDIOBJ oldBrush = SelectObject(dc, GetStockObject(NULL_BRUSH));
    RoundRect(dc, box.left, box.top, box.right, box.bottom, 4, 4);
    SelectObject(dc, oldBrush);
    SelectObject(dc, oldPen);
    DeleteObject(pen);
    if (SendMessageW(item->hwndItem, BM_GETCHECK, 0, 0) == BST_CHECKED) {
      HPEN checkPen = CreatePen(PS_SOLID, 2, kAccent);
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
    DrawTextW(dc, L"开机自动启动", -1, &text, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    SelectObject(dc, oldFont);
    return;
  }

  const bool primary = (id == kIdSave);
  HBRUSH fill = CreateSolidBrush(primary ? (pressed ? RGB(0x4C, 0x86, 0xDD) : kAccent)
                                         : (pressed ? RGB(0x22, 0x22, 0x28) : kControl));
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
  SetTextColor(dc, primary ? RGB(0x10, 0x18, 0x28) : kText);
  HGDIOBJ oldFont = SelectObject(dc, font_);
  DrawTextW(dc, text, -1, &rect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
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
      SetTextColor(dc, kText);
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
    case WM_DRAWITEM:
      DrawButton(reinterpret_cast<const DRAWITEMSTRUCT*>(lParam));
      return TRUE;
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
      if (id == kIdClearIndex) {
        if (onClearIndex_) onClearIndex_();
        SetWindowTextW(status_, L"已清除文件索引，正在后台重建…");
        return 0;
      }
      if (id == kIdSave) {
        OnSave();
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
      capEdit_ = nullptr;
      break;
    case WM_NCDESTROY:
      hwnd_ = nullptr;
      break;
    default:
      break;
  }
  return DefWindowProcW(hwnd_, message, wParam, lParam);
}
