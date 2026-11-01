#pragma once

#include <string>
#include <vector>

#include "search/filter.h"

struct AppSettings {
  std::wstring theme = L"system";  // system/dark/light
  std::wstring accent;             // 空 = 设计蓝；system；#RRGGBB
  std::wstring hotkey = L"Alt+Space";
  bool autostart = false;
  bool tray = true;
  int indexSizeMB = 512;              // 索引快照最大大小（MB，64–262144）
  std::wstring indexDir;              // 快照目录；空 = %LOCALAPPDATA%\Lume（失败回退 exe 目录）
  std::vector<std::wstring> indexDirHistory;  // 最近用过的自定义快照目录（最多 5 条）
  std::vector<std::wstring> fileRoots;  // 空 = 桌面/文档/下载
  std::wstring fileSearch = L"local";   // local = Lume 本地索引；everything = 全盘走 Everything
  FilterSet filters = DefaultFilterSet();  // 搜索框下方的筛选组（含内置「全部」）
};

// settings.conf：UTF-8 行式 key=value；fileRoot / filterGroup 可重复。
class SettingsStore {
 public:
  static std::wstring Path();
  static bool Load(AppSettings* settings);
  static bool Save(const AppSettings& settings, std::wstring* savedPath = nullptr);
};
