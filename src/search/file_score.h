#pragma once

#include <cstdint>
#include <string>
#include <string_view>

// 文件名/路径的评分口径：本地索引（FileIndex）与 Everything 后端共用同一套，
// 保证切换来源后排序规则不变。
bool IsPathBoundary(wchar_t ch);

// 只用查询里的 ASCII 字母/数字做位图预筛（标点、中文被忽略，仍是必要条件）。
uint64_t QueryMask(const std::wstring& needleLower);

// 0 = 没命中，1 = 子串，2 = 前缀，3 = 忽略扩展名一致，4 = 字面完全一致。
uint8_t NameQuality(std::wstring_view lowerName, const std::wstring& needleLower);

// 路径命中只做"目录边界处的子串"匹配，且大幅降权；查询短于 3 个字符直接不命中。
size_t ScorePathOnly(const std::wstring& lowerPath, const std::wstring& needleLower);

struct FileNameScore {
  size_t score = 0;
  uint8_t quality = 0;
};

// 按文件名打分（供 Everything 后端使用）：
// - 单词查询：与本地索引完全同款（ScoreLowerText + NameQuality）；
// - 多词查询：Everything 保证每个词都在文件名里，取各词最高分再降权一半。
FileNameScore ScoreFileName(std::wstring_view lowerName, const std::wstring& queryLower);
