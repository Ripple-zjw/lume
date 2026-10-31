#include "platform/tray.h"

#include <vector>

#include "core/log.h"

namespace {

bool IsLowIntegrityProcess() {
  HANDLE token = nullptr;
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
  DWORD size = 0;
  GetTokenInformation(token, TokenIntegrityLevel, nullptr, 0, &size);
  bool low = false;
  if (size > 0) {
    std::vector<BYTE> buffer(size);
    if (GetTokenInformation(token, TokenIntegrityLevel, buffer.data(), size, &size)) {
      auto* label = reinterpret_cast<TOKEN_MANDATORY_LABEL*>(buffer.data());
      const DWORD count = *GetSidSubAuthorityCount(label->Label.Sid);
      if (count > 0) {
        const DWORD rid = *GetSidSubAuthority(label->Label.Sid, count - 1);
        low = rid < SECURITY_MANDATORY_MEDIUM_RID;
      }
    }
  }
  CloseHandle(token);
  return low;
}

constexpr wchar_t kHostClass[] = L"LumeTrayHost";

HICON LoadIconResource(int id) {
  const int width = GetSystemMetrics(SM_CXSMICON);
  const int height = GetSystemMetrics(SM_CYSMICON);
  return static_cast<HICON>(LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(id),
                                       IMAGE_ICON, width, height, LR_DEFAULTCOLOR));
}

HICON CreateAppIcon() {
  // 2 = 高对比托盘图标（蓝底白镜头），1 = 主程序图标（深色）。
  HICON icon = LoadIconResource(2);
  if (!icon) icon = LoadIconResource(1);
  if (!icon) {
    Log(L"托盘图标资源加载失败，改用系统图标");
    icon = LoadIconW(nullptr, IDI_APPLICATION);
  }
  return icon;
}

LRESULT CALLBACK TrayHostProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
  if (message == WM_NCCREATE) {
    auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
  }
  if (message == kTrayMessage) {
    HWND owner = reinterpret_cast<HWND>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (owner) {
      PostMessageW(owner, message, wParam, lParam);
      return 0;
    }
  }
  return DefWindowProcW(hwnd, message, wParam, lParam);
}

HWND CreateHostWindow(HWND owner) {
  WNDCLASSEXW wc{};
  wc.cbSize = sizeof(wc);
  wc.lpfnWndProc = TrayHostProc;
  wc.hInstance = GetModuleHandleW(nullptr);
  wc.lpszClassName = kHostClass;
  if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
    Log(L"托盘宿主窗口注册失败: %lu", GetLastError());
    return nullptr;
  }
  HWND host = CreateWindowExW(0, kHostClass, L"LumeTray", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr,
                              GetModuleHandleW(nullptr), owner);
  if (!host) {
    Log(L"托盘宿主窗口创建失败: %lu", GetLastError());
  }
  return host;
}

}  // namespace

TrayIcon::~TrayIcon() { Destroy(); }

bool TrayIcon::Create(HWND owner) {
  if (added_) return true;
  owner_ = owner;
  // 托盘回调放在独立的隐藏宿主窗口上，避免主窗口的 DComp/TOOLWINDOW 影响 shell。
  if (!host_) host_ = CreateHostWindow(owner_);
  if (icon_) {
    DestroyIcon(icon_);
    icon_ = nullptr;
  }
  icon_ = CreateAppIcon();
  data_ = {};
  data_.cbSize = sizeof(data_);
  data_.hWnd = host_ ? host_ : owner_;
  data_.uID = 1;
  data_.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
  data_.uCallbackMessage = kTrayMessage;
  data_.hIcon = icon_;
  lstrcpynW(data_.szTip, L"Lume（Alt+Space 唤出）", ARRAYSIZE(data_.szTip));
  data_.uVersion = NOTIFYICON_VERSION_4;
  Log(L"托盘: icon=%p host=%p owner=%p shell=%p", static_cast<void*>(icon_),
      static_cast<void*>(host_), static_cast<void*>(owner_),
      static_cast<void*>(GetShellWindow()));
  if (!Shell_NotifyIconW(NIM_ADD, &data_)) {
    const DWORD firstError = GetLastError();
    // 少数 shell 版本对 NIF_SHOWTIP/版本 4 不兼容，退一步再试。
    data_.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    if (!Shell_NotifyIconW(NIM_ADD, &data_)) {
      Log(L"托盘图标创建失败 (Win32 %lu/%lu, shell=%p)", firstError, GetLastError(),
          static_cast<void*>(GetShellWindow()));
      if (IsLowIntegrityProcess()) {
        Log(L"当前进程是低完整性 (Low IL)：UIPI 会阻止托盘。请把 Lume.exe 复制/解压到普通目录"
            L"（如桌面或 C:\\Apps\\Lume）再运行，不要直接在受限工作区目录里运行。");
      }
      return false;
    }
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
  if (host_) {
    DestroyWindow(host_);
    host_ = nullptr;
  }
  if (icon_) {
    DestroyIcon(icon_);
    icon_ = nullptr;
  }
  owner_ = nullptr;
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
  AppendMenuW(menu, MF_STRING, kTrayShow, L"显示 Lume");
  AppendMenuW(menu, MF_STRING | (autostartChecked ? MF_CHECKED : MF_UNCHECKED), kTrayAutostart,
              L"开机自启");
  AppendMenuW(menu, MF_STRING, kTraySettings, L"设置…");
  AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(menu, MF_STRING, kTrayExit, L"退出");

  POINT cursor{};
  GetCursorPos(&cursor);
  HWND foreground = owner_ ? owner_ : data_.hWnd;
  if (foreground) SetForegroundWindow(foreground);
  const UINT command =
      TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY, cursor.x, cursor.y, 0,
                     data_.hWnd, nullptr);
  DestroyMenu(menu);
  if (data_.hWnd) PostMessageW(data_.hWnd, WM_NULL, 0, 0);
  return command;
}
