#pragma once

#include <windows.h>
#include <shellapi.h>

#include <string>

constexpr UINT kTrayMessage = WM_APP + 3;
enum TrayCommand : UINT {
  kTrayShow = 1001,
  kTrayAutostart = 1002,
  kTraySettings = 1003,
  kTrayExit = 1004,
};

class TrayIcon {
 public:
  ~TrayIcon();
  bool Create(HWND owner);
  void Destroy();
  void SetTooltip(const std::wstring& text);
  void ReAdd();
  // 返回用户选择的命令（0 表示取消）。
  UINT ShowMenu(bool autostartChecked);

 private:
  NOTIFYICONDATAW data_{};
  HICON icon_ = nullptr;
  bool added_ = false;
};
