#pragma once

#include <string>

#include "ui/theme.h"

struct AppOptions {
  ThemeMode themeMode = ThemeMode::System;
  bool themeSpecified = false;
  std::wstring hotkeyOverride;
  std::wstring accent;  // 空 = 设计默认蓝；system = 跟随系统；RRGGBB = 指定
  bool accentSpecified = false;
  bool autostartLaunch = false;
  std::wstring query;
  std::wstring logPath;
  std::wstring fileRoot;  // 测试用：覆盖文件索引根目录
  bool showAtStart = false;
  int autoExitMs = 0;
  std::wstring capturePath;
  std::wstring keys;  // 逗号分隔的按键序列（up/down/enter/...），用于自动化验证
  std::wstring type;  // 依次投递的字符，用于验证输入
  std::wstring compose;  // 测试钩子：注入组字串，验证 IME 渲染
  int captureDelayMs = 450;
  int selected = 0;  // 截图用：预选中行
  // 测试钩子（1 起，0 = 关闭）：把鼠标消息投到某一行，验证悬停高亮与点击启动。
  int hoverRow = 0;
  int clickRow = 0;
  bool noHotkey = false;
  bool glass = false;
  bool openSettings = false;
  bool clearIndexOnStart = false;
  size_t indexSizeMB = 0;  // 0 = 使用设置里的索引大小上限
  std::wstring indexDir;   // 覆盖索引快照目录（测试用）
  // 覆盖文件搜索来源：local / everything（只影响本次运行，不写盘）
  std::wstring fileSearch;
  bool fileSearchSpecified = false;
  // 覆盖筛选组：--filter=<名称|序号|all|off>（只影响本次运行）
  std::wstring filterSpec;
  bool filterSpecified = false;
  std::wstring testCapture;
};

// 常驻启动器：单实例、热键、显示/隐藏、截图模式。返回进程退出码。
int RunLauncher(const AppOptions& options);
int RunScreenshot(const AppOptions& options, const std::wstring& path, float scale);
int RunScreenshotMatrix(const AppOptions& options, const std::wstring& directory);
