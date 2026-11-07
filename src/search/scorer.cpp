#include "search/app_entry.h"

#include <algorithm>
#include <array>
#include <cwctype>
#include <vector>

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

// 设置页查询里的命名空间词：用户常写成「设置 蓝牙」「settings bluetooth」。
bool IsNamespaceWord(const std::wstring& word) {
  return word == L"设置" || word == L"settings";
}

// 整条查询只有命名空间词（「设置」「settings」）：那是「设置」应用自己的查询，不该出设置页。
bool IsOnlyNamespaceWords(const std::wstring& query) {
  const std::vector<std::wstring> words = SplitQueryWords(query);
  if (words.empty()) return false;
  for (const std::wstring& word : words) {
    if (!IsNamespaceWord(word)) return false;
  }
  return true;
}

struct KeywordHit {
  size_t score = 0;
  uint8_t quality = 0;
};

// 关键词命中：只接受连续命中（子串），不做模糊——「启动」这类短词做模糊会把整张表拽出来。
KeywordHit MatchKeyword(std::wstring_view keyword, std::wstring_view needle) {
  KeywordHit hit;
  if (keyword.empty() || needle.empty() || needle.size() > keyword.size()) return hit;
  const size_t position = keyword.find(needle);
  if (position == std::wstring_view::npos) return hit;
  if (keyword == needle) {
    hit.score = 780;
    hit.quality = 3;
    return hit;
  }
  if (needle.size() == 1) return hit;  // 单字符只认完整命中的关键词
  if (position == 0) {
    hit.score = 720 - std::min<size_t>(needle.size(), 40);
    hit.quality = 2;
    return hit;
  }
  hit.score = 700 - std::min<size_t>(position * 4, 200);
  hit.quality = 1;
  return hit;
}

bool IsWordStartAt(std::wstring_view text, size_t index) {
  return index == 0 || IsWordStart(text[index - 1]);
}

// 子序列命中的基准与噪声下限：低于下限的松散命中直接丢弃（不显示无意义的模糊结果）。
constexpr size_t kFuzzyBase = 300;
constexpr size_t kMinFuzzyScore = 300;
// 子序列位置用定长数组保存，避免热路径每个候选分配一次 vector。
constexpr size_t kMaxFuzzyQuery = 64;

struct Greedy {
  bool matched = false;
  size_t count = 0;
  size_t score = 0;
  size_t span = 0;
  size_t gaps = 0;
  size_t maxGap = 0;
  size_t wordStarts = 0;
  std::array<size_t, kMaxFuzzyQuery> positions{};
};

// 最早命中贪心：一次前向扫描，不回溯；命中位置记下来供 gap/跨度打分。
Greedy GreedyPositions(std::wstring_view text, std::wstring_view query) {
  Greedy greedy;
  if (query.size() > kMaxFuzzyQuery) return greedy;
  size_t qi = 0;
  for (size_t i = 0; i < text.size() && qi < query.size(); ++i) {
    if (text[i] != query[qi]) continue;
    greedy.positions[qi] = i;
    ++qi;
  }
  if (qi < query.size()) return greedy;
  greedy.matched = true;
  greedy.count = qi;
  return greedy;
}

// 按「连续命中 / 词首命中 / 跳过字符」打分：
// - 连续命中 +8，非连续 +2，词首额外 +12，首字符 +20；
// - 跳字惩罚：词内跳字每个 +6，词首跳转（缩写）每个 +2，另加单次最大跳跃惩罚；
// - 名字越长略微扣分。
Greedy ScorePositions(std::wstring_view text, std::wstring_view query, Greedy greedy) {
  size_t score = kFuzzyBase;
  size_t penalty = 0;
  size_t gapsTotal = 0;
  size_t maxGap = 0;
  size_t wordStarts = 0;
  for (size_t i = 0; i < greedy.count; ++i) {
    const size_t position = greedy.positions[i];
    const bool atWordStart = IsWordStartAt(text, position);
    if (atWordStart) ++wordStarts;
    if (i == 0) {
      if (position == 0) {
        score += 20;
      } else if (atWordStart) {
        score += 12;
      }
      continue;
    }
    const size_t previous = greedy.positions[i - 1];
    const size_t gap = position - previous - 1;
    if (gap == 0) {
      score += 8;
    } else {
      score += 2;
      gapsTotal += gap;
      maxGap = std::max(maxGap, gap);
      penalty += gap * (atWordStart ? 2 : 6);
    }
    if (atWordStart) score += 12;
  }
  // 首个命中之前的字符也算「跳过」（匹配越靠后越弱）。
  if (greedy.count > 0) {
    gapsTotal += greedy.positions[0];
    penalty += greedy.positions[0] * 2;
  }
  if (text.size() > 8) {
    const size_t lengthPenalty = (text.size() - 8) / 4;
    score = score > lengthPenalty ? score - lengthPenalty : 0;
  }
  score = score > penalty ? score - penalty : 0;
  greedy.score = score;
  greedy.gaps = gapsTotal;
  greedy.maxGap = maxGap;
  greedy.wordStarts = wordStarts;
  greedy.span = greedy.count == 0 ? 0
                                  : greedy.positions[greedy.count - 1] - greedy.positions[0] + 1;
  return greedy;
}

}  // namespace

std::vector<std::wstring> SplitQueryWords(const std::wstring& queryLower) {
  std::vector<std::wstring> words;
  std::wstring current;
  for (wchar_t ch : queryLower) {
    if (ch == L' ' || ch == L'\t' || ch == L'\r' || ch == L'\n') {
      if (!current.empty()) {
        words.push_back(current);
        current.clear();
      }
      continue;
    }
    current.push_back(ch);
  }
  if (!current.empty()) words.push_back(current);
  return words;
}

std::wstring StripNamespaceWords(const std::wstring& query) {
  const std::vector<std::wstring> words = SplitQueryWords(query);
  if (words.size() < 2) return query;
  std::wstring stripped;
  size_t kept = 0;
  for (const std::wstring& word : words) {
    if (IsNamespaceWord(word)) continue;
    if (kept > 0) stripped.push_back(L' ');
    stripped += word;
    ++kept;
  }
  // 全被剥掉（「设置 settings」）时回退原查询，否则一次列出整张表。
  return kept == 0 ? query : stripped;
}

size_t ScoreLowerText(std::wstring_view text, std::wstring_view queryLower, TextMatch* match) {
  if (match) *match = TextMatch{};
  if (text.empty() || queryLower.empty()) return 0;

  const size_t substring = text.find(queryLower);
  if (substring != std::wstring_view::npos) {
    // 单字符查询只接受前缀或词首，避免打一个字母就命中整个索引。
    if (queryLower.size() == 1 && substring != 0 && !IsWordStart(text[substring - 1])) {
      return 0;
    }
    size_t score = 1000;
    if (substring == 0) {
      score += 200;
    } else {
      score -= std::min<size_t>(substring * 2, 200);
    }
    if (text.size() > 8) score -= (text.size() - 8) / 4;
    if (match) {
      match->matched = true;
      match->contiguous = true;
      match->span = queryLower.size();
      match->wordStarts = IsWordStartAt(text, substring) ? 1 : 0;
    }
    return score;
  }

  // 模糊（子序列）只对 3 个字符以上的查询开启：短查询的模糊命中几乎全是噪声。
  if (queryLower.size() < 3 || text.size() < queryLower.size()) return 0;

  Greedy greedy = GreedyPositions(text, queryLower);
  if (!greedy.matched) return 0;
  greedy = ScorePositions(text, queryLower, greedy);
  if (greedy.score < kMinFuzzyScore) return 0;
  if (match) {
    match->matched = true;
    match->contiguous = false;
    match->span = greedy.span;
    match->gaps = greedy.gaps;
    match->maxGap = greedy.maxGap;
    match->wordStarts = greedy.wordStarts;
  }
  return greedy.score;
}

size_t ScoreEntry(const AppEntry& entry, const std::wstring& query, bool* matchedTitle,
                  uint8_t* quality) {
  if (matchedTitle) *matchedTitle = false;
  if (quality) *quality = 0;
  if (query.empty()) return 1;
  const std::wstring needle = ToLower(query);
  // 只输入「设置 / settings」时交给「设置」应用那一条，不在结果里铺设置页
  // （标题里带「设置」的页面也不出，避免同一个查询被设置页和应用抢）。
  if (entry.IsSettings() && IsOnlyNamespaceWords(needle)) return 0;
  const std::wstring title = ToLower(entry.title);
  TextMatch match;
  const size_t titleScore = ScoreLowerText(title, needle, &match);
  if (titleScore > 0) {
    if (matchedTitle) *matchedTitle = true;
    if (quality) {
      if (title == needle) {
        *quality = 4;
      } else if (title.size() >= needle.size() && title.compare(0, needle.size(), needle) == 0) {
        *quality = 2;
      } else if (match.contiguous) {
        *quality = 1;
      } else {
        *quality = 0;  // 子序列命中不能当作普通子串
      }
    }
    return titleScore;
  }

  const std::vector<std::wstring> words = SplitQueryWords(needle);
  if (words.size() > 1) {
    size_t total = 0;
    bool allContiguous = true;
    bool allMatched = true;
    for (const std::wstring& word : words) {
      TextMatch wordMatch;
      const size_t score = ScoreLowerText(title, word, &wordMatch);
      if (score == 0) {
        allMatched = false;
        break;
      }
      total += score;
      allContiguous = allContiguous && wordMatch.contiguous;
    }
    if (allMatched) {
      if (matchedTitle) *matchedTitle = true;
      if (quality) *quality = allContiguous ? 1 : 0;
      return total / words.size();
    }
  }

  // 系统设置页：标题没命中时看关键词（中英别名）。「设置 蓝牙」这类写法先剥命名空间词。
  // 设置页不参与下面的 detail/URI 兜底，否则 URI 里的 ms-settings 会把整张表拽出来。
  if (entry.IsSettings()) {
    const std::wstring settingsNeedle = StripNamespaceWords(needle);
    const std::vector<std::wstring> settingsWords = SplitQueryWords(settingsNeedle);
    if (settingsWords.size() == 1) {
      const std::wstring& field = settingsWords.front();
      size_t bestScore = ScoreLowerText(title, field, &match);
      uint8_t bestQuality = 0;
      if (bestScore > 0) {
        if (title == field) {
          bestQuality = 4;
        } else if (title.size() >= field.size() && title.compare(0, field.size(), field) == 0) {
          bestQuality = 2;
        } else {
          bestQuality = match.contiguous ? 1 : 0;
        }
      }
      for (const std::wstring& keyword : entry.keywords) {
        const KeywordHit hit = MatchKeyword(ToLower(keyword), field);
        if (hit.score > bestScore) {
          bestScore = hit.score;
          bestQuality = hit.quality;
        }
      }
      if (bestScore > 0) {
        if (matchedTitle) *matchedTitle = true;
        if (quality) *quality = bestQuality;
        return bestScore;
      }
    } else if (settingsWords.size() > 1) {
      // 多词：每个词都要在「标题或某个关键词」里连续命中，顺序无关。
      size_t total = 0;
      uint8_t minQuality = 3;
      bool allMatched = true;
      for (const std::wstring& word : settingsWords) {
        TextMatch wordMatch;
        size_t wordScore = ScoreLowerText(title, word, &wordMatch);
        uint8_t wordQuality = wordScore > 0 ? (wordMatch.contiguous ? 1 : 0) : 0;
        for (const std::wstring& keyword : entry.keywords) {
          const KeywordHit hit = MatchKeyword(ToLower(keyword), word);
          if (hit.score > wordScore) {
            wordScore = hit.score;
            wordQuality = hit.quality;
          }
        }
        if (wordScore == 0) {
          allMatched = false;
          break;
        }
        total += wordScore;
        if (wordQuality < minQuality) minQuality = wordQuality;
      }
      if (allMatched) {
        if (matchedTitle) *matchedTitle = true;
        if (quality) *quality = std::min<uint8_t>(minQuality, 2);
        return 600 + std::min<size_t>(total / settingsWords.size() / 10, 60);
      }
    }
    return 0;
  }

  // 标题未命中时回退到详情/AUMID（例如 "calc" 命中 WindowsCalculator 的 AUMID），
  // 要求查询至少 3 个字符以避免过多噪声命中；多词要求每个词都出现。
  if (needle.size() < 3) return 0;
  const std::wstring aux = ToLower(entry.detail + L" " + entry.launchTarget);
  size_t position = std::wstring::npos;
  if (words.size() <= 1) {
    position = aux.find(needle);
  } else {
    size_t from = 0;
    bool allMatched = true;
    for (const std::wstring& word : words) {
      const size_t found = aux.find(word, from);
      if (found == std::wstring::npos) {
        allMatched = false;
        break;
      }
      position = found;
      from = found + word.size();
    }
    if (!allMatched) return 0;
  }
  if (position == std::wstring::npos) return 0;
  return 180 - std::min<size_t>(position / 4, 120);
}

std::vector<const AppEntry*> FilterEntries(const std::vector<AppEntry>& entries,
                                           const std::wstring& query, size_t limit) {
  std::vector<const AppEntry*> result;
  result.reserve(std::min(entries.size(), limit));

  if (query.empty()) {
    result.reserve(entries.size());
    for (const AppEntry& entry : entries) result.push_back(&entry);
    std::sort(result.begin(), result.end(), [](const AppEntry* a, const AppEntry* b) {
      const std::wstring titleA = ToLower(a->title);
      const std::wstring titleB = ToLower(b->title);
      if (titleA != titleB) return titleA < titleB;
      return _wcsicmp(a->launchTarget.c_str(), b->launchTarget.c_str()) < 0;
    });
    if (result.size() > limit) result.resize(limit);
    return result;
  }

  struct Scored {
    const AppEntry* entry;
    size_t score;
    std::wstring titleLower;
  };
  std::vector<Scored> scored;
  scored.reserve(entries.size());
  for (const AppEntry& entry : entries) {
    const size_t score = ScoreEntry(entry, query);
    if (score > 0) scored.push_back({&entry, score, ToLower(entry.title)});
  }
  std::sort(scored.begin(), scored.end(), [](const Scored& a, const Scored& b) {
    if (a.score != b.score) return a.score > b.score;
    if (a.titleLower != b.titleLower) return a.titleLower < b.titleLower;
    return _wcsicmp(a.entry->launchTarget.c_str(), b.entry->launchTarget.c_str()) < 0;
  });
  const size_t count = std::min(scored.size(), limit);
  result.reserve(count);
  for (size_t i = 0; i < count; ++i) result.push_back(scored[i].entry);
  return result;
}
