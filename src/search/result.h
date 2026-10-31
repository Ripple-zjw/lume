#pragma once

#include <windows.h>

#include <string>
#include <vector>

// 搜索结果行（P2 用固定样例，P3 起由搜索源填充）。
struct ResultRow {
  std::wstring title;
  std::wstring subtitle;
  std::wstring glyph;
  UINT32 color = 0x6EA8FF;
};

std::vector<ResultRow> SampleResults();
