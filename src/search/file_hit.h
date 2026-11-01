#pragma once

#include <windows.h>

#include <cstdint>
#include <string>

// 一次文件搜索命中的统一表示（本地索引与 Everything 后端共用）。
struct FileHit {
  std::wstring path;
  bool isDirectory = false;
  size_t score = 0;
  bool pathOnly = false;  // true = 仅路径命中（文件名没命中）
  // 0 = 模糊/仅路径，1 = 子串，2 = 前缀，3 = 忽略扩展名一致，4 = 字面完全一致
  uint8_t quality = 0;
};

// 合并层最多消费多少条文件命中（应用 + 文件一起再截断到列表上限）。
constexpr size_t kMaxFileHits = 80;
