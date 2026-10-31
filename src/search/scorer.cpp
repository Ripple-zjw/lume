#include "search/app_entry.h"

#include <algorithm>
#include <cwctype>

namespace {

std::wstring ToLower(std::wstring text) {
  for (wchar_t& ch : text) {
    ch = static_cast<wchar_t>(std::towlower(static_cast<wint_t>(ch)));
  }
  return text;
}

bool IsWordStart(wchar_t previous) {
  return previous == L' ' || previous == L'-' || previous == L'_' || previous == L'.' ||
         previous == L'\\' || previous == L'/';
}

}  // namespace

size_t ScoreLowerText(std::wstring_view text, std::wstring_view queryLower) {
  if (text.empty() || queryLower.empty()) return 0;

  const size_t substring = text.find(queryLower);
  if (substring != std::wstring_view::npos) {
    size_t score = 1000;
    if (substring == 0) {
      score += 200;
    } else {
      score -= std::min<size_t>(substring * 2, 200);
    }
    if (text.size() > 8) score -= (text.size() - 8) / 4;
    return score;
  }

  // 子序列匹配：连续命中与词首命中加分
  size_t score = 0;
  size_t textIndex = 0;
  size_t queryIndex = 0;
  long lastMatch = -2;
  for (; textIndex < text.size() && queryIndex < queryLower.size(); ++textIndex) {
    if (text[textIndex] != queryLower[queryIndex]) continue;
    score += (lastMatch == static_cast<long>(textIndex) - 1) ? 8 : 2;
    if (textIndex == 0) {
      score += 20;
    } else if (IsWordStart(text[textIndex - 1])) {
      score += 12;
    }
    lastMatch = static_cast<long>(textIndex);
    ++queryIndex;
  }
  if (queryIndex < queryLower.size()) return 0;
  if (text.size() > 8) {
    const size_t penalty = (text.size() - 8) / 4;
    score = score > penalty ? score - penalty : 0;
  }
  return 300 + score;
}

size_t ScoreEntry(const AppEntry& entry, const std::wstring& query) {
  if (query.empty()) return 1;
  const std::wstring needle = ToLower(query);
  const size_t titleScore = ScoreLowerText(ToLower(entry.title), needle);
  if (titleScore > 0) return titleScore;

  // 标题未命中时回退到详情/AUMID（例如 "calc" 命中 WindowsCalculator 的 AUMID），
  // 要求查询至少 3 个字符以避免过多噪声命中。
  if (needle.size() >= 3) {
    const std::wstring aux = ToLower(entry.detail + L" " + entry.launchTarget);
    const size_t position = aux.find(needle);
    if (position != std::wstring::npos) {
      return 180 - std::min<size_t>(position / 4, 120);
    }
  }
  return 0;
}

std::vector<const AppEntry*> FilterEntries(const std::vector<AppEntry>& entries,
                                           const std::wstring& query, size_t limit) {
  std::vector<const AppEntry*> result;
  result.reserve(std::min(entries.size(), limit));

  if (query.empty()) {
    result.reserve(entries.size());
    for (const AppEntry& entry : entries) result.push_back(&entry);
    std::sort(result.begin(), result.end(), [](const AppEntry* a, const AppEntry* b) {
      return ToLower(a->title) < ToLower(b->title);
    });
    if (result.size() > limit) result.resize(limit);
    return result;
  }

  struct Scored {
    const AppEntry* entry;
    size_t score;
  };
  std::vector<Scored> scored;
  scored.reserve(entries.size());
  for (const AppEntry& entry : entries) {
    const size_t score = ScoreEntry(entry, query);
    if (score > 0) scored.push_back({&entry, score});
  }
  std::sort(scored.begin(), scored.end(), [](const Scored& a, const Scored& b) {
    if (a.score != b.score) return a.score > b.score;
    return ToLower(a.entry->title) < ToLower(b.entry->title);
  });
  const size_t count = std::min(scored.size(), limit);
  result.reserve(count);
  for (size_t i = 0; i < count; ++i) result.push_back(scored[i].entry);
  return result;
}
