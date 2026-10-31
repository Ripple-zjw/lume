#include "platform/tray.h"

#include "core/log.h"

namespace {

HICON CreateAppIcon() {
  const int width = GetSystemMetrics(SM_CXSMICON);
  const int height = GetSystemMetrics(SM_CYSMICON);
  HICON icon = static_cast<HICON>(LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(1),
                                            IMAGE_ICON, width, height, LR_DEFAULTCOLOR));
  return icon ? icon : LoadIconW(nullptr, IDI_APPLICATION);
}

}  // namespace

TrayIcon::~TrayIcon() { Destroy(); }

bool TrayIcon::Create(HWND owner) {
  icon_ = CreateAppIcon();
  data_.cbSize = sizeof(data_);
  data_.hWnd = owner;
  data_.uID = 1;
  data_.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
  data_.uCallbackMessage = kTrayMessage;
  data_.hIcon = icon_;
  lstrcpynW(data_.szTip, L"eTools（Alt+Space 唤出）", ARRAYSIZE(data_.szTip));
  data_.uVersion = NOTIFYICON_VERSION_4;
  if (!Shell_NotifyIconW(NIM_ADD, &data_)) {
    Log(L"托盘图标创建失败");
    return false;
  }
  Shell_NotifyIconW(NIM_SETVERSION, &data_);
  added_ = true;
  Log(L"托盘图标已创建");
  return true;
}

void TrayIcon::Destroy() {
  if (added_) {
    Shell_NotifyIconW(NIM_DELETE, &data_);
    added_ = false;
  }
  if (icon_) {
    DestroyIcon(icon_);
    icon_ = nullptr;
  }
}

void TrayIcon::SetTooltip(const std::wstring& text) {
  lstrcpynW(data_.szTip, text.c_str(), ARRAYSIZE(data_.szTip));
  if (added_) Shell_NotifyIconW(NIM_MODIFY, &data_);
}

void TrayIcon::ReAdd() {
  if (!added_) return;
  Shell_NotifyIconW(NIM_ADD, &data_);
  Shell_NotifyIconW(NIM_SETVERSION, &data_);
}

UINT TrayIcon::ShowMenu(bool autostartChecked) {
  HMENU menu = CreatePopupMenu();
  if (!menu) return 0;
  AppendMenuW(menu, MF_STRING, kTrayShow, L"显示 eTools");
  AppendMenuW(menu, MF_STRING | (autostartChecked ? MF_CHECKED : MF_UNCHECKED), kTrayAutostart,
              L"开机自启");
  AppendMenuW(menu, MF_STRING, kTraySettings, L"设置…");
  AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(menu, MF_STRING, kTrayExit, L"退出");

  POINT cursor{};
  GetCursorPos(&cursor);
  if (data_.hWnd) SetForegroundWindow(data_.hWnd);
  const UINT command =
      TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY, cursor.x, cursor.y, 0,
                     data_.hWnd, nullptr);
  DestroyMenu(menu);
  if (data_.hWnd) PostMessageW(data_.hWnd, WM_NULL, 0, 0);
  return command;
}
