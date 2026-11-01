#include "search/file_score.h"

#include <algorithm>

#include "search/app_entry.h"

bool IsPathBoundary(wchar_t ch) {
  return ch == L'\\' || ch == L'/' || ch == L' ' || ch == L'-' || ch == L'_' || ch == L'.';
}

uint64_t QueryMask(const std::wstring& needle) {
  uint64_t mask = 0;
  for (wchar_t ch : needle) {
    if (ch >= L'a' && ch <= L'z') {
      mask |= 1ull << (ch - L'a');
    } else if (ch >= L'0' && ch <= L'9') {
      mask |= 1ull << (26 + (ch - L'0'));
    }
  }
  return mask;
}

uint8_t NameQuality(std::wstring_view name, const std::wstring& needle) {
  const std::wstring_view query(needle);
  if (name == query) return 4;  // 字面完全一致（例如目录名就是 game）
  const size_t dot = name.find_last_of(L'.');
  if (dot != std::wstring_view::npos && dot > 0 && name.substr(0, dot) == query) return 3;
  if (name.size() >= query.size() && name.compare(0, query.size(), query) == 0) return 2;
  if (name.find(query) != std::wstring_view::npos) return 1;
  return 0;
}

size_t ScorePathOnly(const std::wstring& lowerPath, const std::wstring& needle) {
  if (needle.size() < 3) return 0;
  const size_t position = lowerPath.find(needle);
  if (position == std::wstring::npos) return 0;
  if (position > 0 && !IsPathBoundary(lowerPath[position - 1])) return 0;
  size_t score = 1000;
  if (position == 0) {
    score += 200;
  } else {
    score -= std::min<size_t>(position * 2, 200);
  }
  return std::max<size_t>(1, score) / 6;
}

FileNameScore ScoreFileName(std::wstring_view lowerName, const std::wstring& queryLower) {
  FileNameScore result;
  if (lowerName.empty() || queryLower.empty()) return result;

  // 单词查询：和本地索引走完全相同的评分与质量判定。
  if (queryLower.find(L' ') == std::wstring::npos) {
    result.score = ScoreLowerText(lowerName, queryLower);
    result.quality = NameQuality(lowerName, queryLower);
    return result;
  }

  // 多词查询：Everything 已保证每个词都出现在文件名里，取各词最高分再降权一半，
  // 避免"词越多分数越高"破坏与应用结果的相对排序。
  std::wstring_view query(queryLower);
  size_t best = 0;
  size_t start = 0;
  while (start < query.size()) {
    const size_t space = query.find(L' ', start);
    const size_t end = space == std::wstring_view::npos ? query.size() : space;
    if (end > start) best = std::max(best, ScoreLowerText(lowerName, query.substr(start, end - start)));
    if (space == std::wstring_view::npos) break;
    start = space + 1;
  }
  if (best == 0) return result;
  result.score = std::max<size_t>(1, best / 2);
  // 整串连续命中时保留更高等级，否则按"分词命中"记为子串。
  result.quality = NameQuality(lowerName, queryLower);
  if (result.quality == 0) result.quality = 1;
  return result;
}
