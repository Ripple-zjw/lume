#pragma once

#include <string>
#include <vector>

struct AppSettings {
  std::wstring theme = L"system";  // system/dark/light
  std::wstring accent;             // 空 = 设计蓝；system；#RRGGBB
  std::wstring hotkey = L"Alt+Space";
  bool autostart = false;
  bool tray = true;
  int fileCap = 1000000;              // 文件索引总条目上限（20k–1M）
  std::vector<std::wstring> fileRoots;  // 空 = 桌面/文档/下载
};

// settings.conf：UTF-8 行式 key=value；fileRoot 可重复。
class SettingsStore {
 public:
  static std::wstring Path();
  static bool Load(AppSettings* settings);
  static bool Save(const AppSettings& settings, std::wstring* savedPath = nullptr);
};
