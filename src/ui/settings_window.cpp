#include "ui/settings_window.h"

#include <commctrl.h>
#include <dwmapi.h>
#include <shlobj.h>

#include <algorithm>

#include "core/log.h"
#include "platform/hotkey.h"
#include "platform/system.h"

namespace {

constexpr wchar_t kClassName[] = L"eToolsSettingsWindow";
constexpr int kWindowWidth = 520;
constexpr int kWindowHeight = 470;

enum ControlId {
  kIdTheme = 100,
  kIdAccent = 101,
  kIdHotkey = 102,
  kIdAutostart = 103,
  kIdRoots = 104,
  kIdAdd = 105,
  kIdRemove = 106,
  kIdSave = 107,
  kIdCancel = 108,
  kIdStatus = 109,
};

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

int AccentIndexFromName(const std::wstring& name) {
  if (name == L"system") return 1;
  return 0;
}

std::wstring AccentNameFromIndex(int index) {
  return index == 1 ? L"system" : L"";
}

}  // namespace

SettingsWindow::~SettingsWindow() {
  if (hwnd_) DestroyWindow(hwnd_);
  if (font_) DeleteObject(font_);
}

int SettingsWindow::Scale(int value) const {
  return MulDiv(value, dpi_, 96);
}

bool SettingsWindow::Show(HINSTANCE instance, HWND owner, const AppSettings& current,
                          SaveCallback onSave) {
  settings_ = current;
  roots_ = current.fileRoots;
  onSave_ = std::move(onSave);

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
  wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
  if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
    Log(L"设置窗注册失败: %lu", GetLastError());
    return false;
  }

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
  Log(L"设置窗已打开");
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
  font_ = CreateFontW(-MulDiv(10, dpi_, 72), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                      DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                      DEFAULT_PITCH, L"Segoe UI Variable Text");

  auto label = [&](const wchar_t* text, int x, int y, int w) {
    HWND control = CreateWindowExW(0, L"STATIC", text, WS_CHILD | WS_VISIBLE, Scale(x), Scale(y),
                                   Scale(w), Scale(20), hwnd_, nullptr, nullptr, nullptr);
    SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
    return control;
  };
  auto combo = [&](int id, int x, int y, int w) {
    HWND control = CreateWindowExW(0, L"COMBOBOX", nullptr,
                                   WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST, Scale(x),
                                   Scale(y), Scale(w), Scale(200), hwnd_,
                                   reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), nullptr, nullptr);
    SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
    return control;
  };
  auto button = [&](const wchar_t* text, int id, int x, int y, int w) {
    HWND control = CreateWindowExW(0, L"BUTTON", text, WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                                   Scale(x), Scale(y), Scale(w), Scale(28), hwnd_,
                                   reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), nullptr, nullptr);
    SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
    return control;
  };

  label(L"主题", 20, 20, 80);
  themeCombo_ = combo(kIdTheme, 110, 16, 200);
  label(L"强调色", 20, 56, 80);
  accentCombo_ = combo(kIdAccent, 110, 52, 200);
  label(L"热键", 20, 92, 80);
  hotkeyCombo_ = combo(kIdHotkey, 110, 88, 200);

  autostartCheck_ = CreateWindowExW(0, L"BUTTON", L"开机自动启动",
                                    WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX, Scale(20),
                                    Scale(128), Scale(300), Scale(24), hwnd_,
                                    reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdAutostart)),
                                    nullptr, nullptr);
  SendMessageW(autostartCheck_, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);

  label(L"文件索引目录（留空 = 桌面 / 文档 / 下载）", 20, 164, 440);
  rootsList_ = CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", nullptr,
                               WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | LBS_NOINTEGRALHEIGHT,
                               Scale(20), Scale(188), Scale(360), Scale(160), hwnd_,
                               reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdRoots)), nullptr,
                               nullptr);
  SendMessageW(rootsList_, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
  button(L"添加…", kIdAdd, 396, 188, 100);
  button(L"移除", kIdRemove, 396, 222, 100);

  status_ = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE, Scale(20), Scale(356),
                            Scale(470), Scale(40), hwnd_,
                            reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdStatus)), nullptr,
                            nullptr);
  SendMessageW(status_, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);

  button(L"保存", kIdSave, 300, 404, 100);
  button(L"取消", kIdCancel, 410, 404, 90);

  const BOOL dark = TRUE;
  DwmSetWindowAttribute(hwnd_, 20 /*DWMWA_USE_IMMERSIVE_DARK_MODE*/, &dark, sizeof(dark));
}

void SettingsWindow::Populate() {
  SendMessageW(themeCombo_, CB_RESETCONTENT, 0, 0);
  SendMessageW(themeCombo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"跟随系统"));
  SendMessageW(themeCombo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"深色"));
  SendMessageW(themeCombo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"浅色"));
  SendMessageW(themeCombo_, CB_SETCURSEL, ThemeIndexFromName(settings_.theme), 0);

  SendMessageW(accentCombo_, CB_RESETCONTENT, 0, 0);
  SendMessageW(accentCombo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"设计蓝 #6EA8FF"));
  SendMessageW(accentCombo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"跟随系统强调色"));
  SendMessageW(accentCombo_, CB_SETCURSEL, AccentIndexFromName(settings_.accent), 0);

  SendMessageW(hotkeyCombo_, CB_RESETCONTENT, 0, 0);
  for (size_t i = 0; i < HotkeyBindingCount(); ++i) {
    SendMessageW(hotkeyCombo_, CB_ADDSTRING, 0,
                 reinterpret_cast<LPARAM>(DefaultHotkeyBinding(i)->name));
  }
  SendMessageW(hotkeyCombo_, CB_SETCURSEL, 0, 0);
  for (size_t i = 0; i < HotkeyBindingCount(); ++i) {
    if (settings_.hotkey == DefaultHotkeyBinding(i)->name) {
      SendMessageW(hotkeyCombo_, CB_SETCURSEL, static_cast<WPARAM>(i), 0);
      break;
    }
  }

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
  if (selection < static_cast<LRESULT>(roots_.size())) {
    roots_.erase(roots_.begin() + selection);
  }
}

void SettingsWindow::OnSave() {
  const LRESULT themeIndex = SendMessageW(themeCombo_, CB_GETCURSEL, 0, 0);
  const LRESULT accentIndex = SendMessageW(accentCombo_, CB_GETCURSEL, 0, 0);
  const LRESULT hotkeyIndex = SendMessageW(hotkeyCombo_, CB_GETCURSEL, 0, 0);
  settings_.theme = ThemeNameFromIndex(themeIndex < 0 ? 0 : static_cast<int>(themeIndex));
  settings_.accent = AccentNameFromIndex(accentIndex < 0 ? 0 : static_cast<int>(accentIndex));
  if (hotkeyIndex >= 0 && hotkeyIndex < static_cast<LRESULT>(HotkeyBindingCount())) {
    settings_.hotkey = DefaultHotkeyBinding(static_cast<size_t>(hotkeyIndex))->name;
  }
  settings_.autostart = SendMessageW(autostartCheck_, BM_GETCHECK, 0, 0) == BST_CHECKED;

  std::vector<std::wstring> roots;
  for (const std::wstring& root : roots_) {
    if (GetFileAttributesW(root.c_str()) != INVALID_FILE_ATTRIBUTES) roots.push_back(root);
  }
  settings_.fileRoots = roots;

  if (onSave_) onSave_(settings_);
  DestroyWindow(hwnd_);
}

LRESULT SettingsWindow::Handle(UINT message, WPARAM wParam, LPARAM lParam) {
  switch (message) {
    case WM_COMMAND: {
      const int id = LOWORD(wParam);
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
      break;
    }
    case WM_CLOSE:
      DestroyWindow(hwnd_);
      return 0;
    case WM_DESTROY:
      status_ = nullptr;
      themeCombo_ = accentCombo_ = hotkeyCombo_ = autostartCheck_ = rootsList_ = nullptr;
      break;
    case WM_NCDESTROY:
      hwnd_ = nullptr;
      break;
    default:
      break;
  }
  return DefWindowProcW(hwnd_, message, wParam, lParam);
}
