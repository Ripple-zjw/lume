#pragma once

#include <string>

#include "ui/theme.h"

struct AppOptions {
  ThemeMode themeMode = ThemeMode::System;
  std::wstring accent;  // 空 = 设计默认蓝；system = 跟随系统；RRGGBB = 指定
  std::wstring query;
  std::wstring logPath;
  bool showAtStart = false;
  int autoExitMs = 0;
  std::wstring capturePath;
  bool noHotkey = false;
  bool glass = false;
};

// 常驻启动器：单实例、热键、显示/隐藏、截图模式。返回进程退出码。
int RunLauncher(const AppOptions& options);
int RunScreenshot(const AppOptions& options, const std::wstring& path, float scale);
int RunScreenshotMatrix(const AppOptions& options, const std::wstring& directory);
