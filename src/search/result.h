#pragma once

#include <windows.h>

#include <cstdint>
#include <string>
#include <vector>

enum class RowAction {
  None,        // 占位行
  LaunchApp,   // Shell 应用
  LaunchUwp,   // 商店应用（AUMID）
  OpenFile,    // 文件
  OpenFolder,  // 文件夹
  OpenSettings,  // Windows 设置页（ms-settings 深链）；加在末尾，保持既有枚举数值稳定
};

// 诊断输出用的动作名（--search-rows 打印 action=...）。
inline const wchar_t* RowActionName(RowAction action) {
  switch (action) {
    case RowAction::LaunchApp:
      return L"app";
    case RowAction::LaunchUwp:
      return L"uwp";
    case RowAction::OpenFile:
      return L"file";
    case RowAction::OpenFolder:
      return L"folder";
    case RowAction::OpenSettings:
      return L"settings";
    case RowAction::None:
      break;
  }
  return L"none";
}

// 显示层结果行（应用与文件共用）。
struct ResultRow {
  std::wstring title;
  std::wstring subtitle;
  std::wstring glyph;
  std::wstring iconKey;  // shell 解析名；为空时用首字色块兜底
  std::wstring target;   // 启动目标 / 文件路径
  UINT32 color = 0x6EA8FF;
  RowAction action = RowAction::None;
  size_t score = 0;
  // 匹配质量：5 直接路径，4 字面完全一致，3 忽略扩展名一致，2 前缀，1 子串，0 模糊/仅路径。
  uint8_t quality = 0;
  // 类型层级：应用/可执行文件 0，其他文件 1，目录 2，仅路径命中 3。
  uint8_t tier = 0;
};
