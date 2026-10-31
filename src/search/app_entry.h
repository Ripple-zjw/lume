#pragma once

#include <windows.h>

#include <string>
#include <vector>

#include "search/result.h"

enum class LaunchKind { Shell, AppUserModelId };

struct AppEntry {
  std::wstring title;
  std::wstring detail;
  std::wstring launchTarget;
  std::wstring iconKey;  // SHCreateItemFromParsingName 可解析的路径/名称
  LaunchKind kind = LaunchKind::Shell;
  std::wstring glyph;  // 无图标时的首字回退
  UINT32 color = 0x6EA8FF;

  bool IsUwp() const { return kind == LaunchKind::AppUserModelId; }
};

// 枚举开始菜单快捷方式、shell:AppsFolder（含商店应用）与 App Paths 注册表项，去重后按名称排序。
std::vector<AppEntry> CollectAppEntries();

size_t ScoreEntry(const AppEntry& entry, const std::wstring& query);
// 返回按得分降序（同分按标题）的条目指针；空查询返回全部（按标题）。
std::vector<const AppEntry*> FilterEntries(const std::vector<AppEntry>& entries,
                                           const std::wstring& query, size_t limit = 200);
// 显示层转换；entries 为空且 withPlaceholder 时给出一条"未找到"占位行。
std::vector<ResultRow> BuildRowsFromEntries(const std::vector<const AppEntry*>& entries,
                                            bool withPlaceholder);
