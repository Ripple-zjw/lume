#pragma once

#include <windows.h>

#include <string>
#include <vector>

// 搜索结果行（显示层模型；由 AppEntry 过滤后生成）。
struct ResultRow {
  std::wstring title;
  std::wstring subtitle;
  std::wstring glyph;
  std::wstring iconKey;  // shell 解析名；为空时用首字色块兜底
  UINT32 color = 0x6EA8FF;
};
