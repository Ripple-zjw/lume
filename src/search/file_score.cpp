#include "search/file_score.h"

#include <algorithm>
#include <vector>

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

FileNameScore ScoreNameForQuery(std::wstring_view lowerName, const std::wstring& queryLower) {
  FileNameScore result;
  if (lowerName.empty() || queryLower.empty()) return result;

  const std::vector<std::wstring> words = SplitQueryWords(queryLower);
  if (words.empty()) return result;

  if (words.size() == 1) {
    const size_t score = ScoreLowerText(lowerName, queryLower);
    if (score == 0) return result;
    result.score = score;
    result.quality = NameQuality(lowerName, queryLower);
    return result;
  }

  // 多词查询：每个词都必须命中，顺序不限；分数取各词平均，避免"词越多分越高"。
  size_t total = 0;
  bool allContiguous = true;
  for (const std::wstring& word : words) {
    TextMatch match;
    const size_t score = ScoreLowerText(lowerName, word, &match);
    if (score == 0) return result;
    total += score;
    allContiguous = allContiguous && match.contiguous;
  }
  result.score = total / words.size();

  const std::wstring_view whole(queryLower);
  if (lowerName.find(whole) != std::wstring_view::npos) {
    // 整串连续命中（例如 "visual studio" 命中 "Visual Studio Code"）：质量按整串算，分数混合。
    result.quality = NameQuality(lowerName, queryLower);
    result.score = (result.score + ScoreLowerText(lowerName, queryLower)) / 2;
  } else {
    result.quality = allContiguous ? 1 : 0;
  }
  return result;
}
