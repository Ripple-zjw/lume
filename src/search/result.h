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
};

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
  // 排序主键：应用/可执行文件 0，其他文件 1，目录 2，仅路径命中 3。
  uint8_t tier = 0;
};
