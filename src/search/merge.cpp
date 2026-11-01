#include "search/merge.h"

#include <algorithm>
#include <cstdint>

#include "search/file_index.h"
#include "search/usage.h"

namespace {

constexpr size_t kMaxFileHits = 80;
constexpr size_t kDirectPathScore = 1000000;

constexpr uint8_t kTierApplication = 0;  // 应用索引 + .exe/.lnk 等可执行文件
constexpr uint8_t kTierFile = 1;         // 其他文件
constexpr uint8_t kTierDirectory = 2;    // 目录
constexpr uint8_t kTierPathOnly = 3;     // 仅路径命中（文件名没命中）

std::wstring FileNameOf(const std::wstring& path) {
  const size_t slash = path.find_last_of(L"\\/");
  return slash == std::wstring::npos ? path : path.substr(slash + 1);
}

// 使用记录里存的是启动目标（应用可执行文件 / AUMID / 打开的文件路径），
// 空查询推荐时先尝试还原成应用行（带图标与副标题），否则当成文件行。
const AppEntry* FindAppByTarget(const std::vector<AppEntry>& apps, const std::wstring& target) {
  for (const AppEntry& entry : apps) {
    if (_wcsicmp(entry.launchTarget.c_str(), target.c_str()) == 0) return &entry;
  }
  return nullptr;
}

bool IsExecutablePath(const std::wstring& path) {
  const size_t slash = path.find_last_of(L"\\/");
  const size_t dot = path.find_last_of(L'.');
  if (dot == std::wstring::npos) return false;
  if (slash != std::wstring::npos && dot < slash + 1) return false;
  const std::wstring extension = path.substr(dot);
  static const wchar_t* kExtensions[] = {L".exe", L".lnk", L".bat", L".cmd",
                                         L".com", L".msi", L".appref-ms"};
  for (const wchar_t* candidate : kExtensions) {
    if (_wcsicmp(extension.c_str(), candidate) == 0) return true;
  }
  return false;
}

std::wstring FirstGlyph(const std::wstring& text) {
  if (text.empty()) return L"?";
  if (text.size() >= 2 && text[0] >= 0xD800 && text[0] <= 0xDBFF) return text.substr(0, 2);
  return text.substr(0, 1);
}

ResultRow MakeAppRow(const AppEntry& entry, size_t score, uint8_t tier, uint8_t quality) {
  ResultRow row;
  row.title = entry.title;
  row.subtitle = entry.detail;
  if (row.subtitle.empty()) {
    row.subtitle = L"应用";
  } else if (row.subtitle.find(L":\\") != std::wstring::npos) {
    // 应用启动路径也常在 C 盘，前缀标明它来自应用索引，避免误以为文件索引收了 C 盘。
    row.subtitle = L"应用 · " + row.subtitle;
  }
  row.glyph = entry.glyph;
  row.iconKey = entry.iconKey;
  row.color = entry.color;
  row.target = entry.launchTarget;
  row.action = entry.IsUwp() ? RowAction::LaunchUwp : RowAction::LaunchApp;
  row.score = score;
  row.tier = tier;
  row.quality = quality;
  return row;
}

ResultRow MakeFileRow(const std::wstring& path, bool isDirectory, size_t score, bool pathOnly,
                      uint8_t quality) {
  ResultRow row;
  row.title = FileNameOf(path);
  row.subtitle = path;
  row.glyph = FirstGlyph(row.title);
  row.iconKey = path;
  row.color = isDirectory ? 0xF59E0B : 0x64748B;
  row.target = path;
  row.action = isDirectory ? RowAction::OpenFolder : RowAction::OpenFile;
  row.score = score;
  row.quality = quality;
  if (pathOnly) {
    row.tier = kTierPathOnly;
  } else if (isDirectory) {
    row.tier = kTierDirectory;
  } else if (IsExecutablePath(path)) {
    row.tier = kTierApplication;
  } else {
    row.tier = kTierFile;
  }
  return row;
}

ResultRow MakePathRow(const std::wstring& path, bool isDirectory) {
  ResultRow row;
  row.title = FileNameOf(path);
  if (row.title.empty()) row.title = path;
  row.subtitle = L"打开此路径";
  row.glyph = FirstGlyph(row.title);
  row.iconKey = path;
  row.color = isDirectory ? 0xF59E0B : 0x3B82F6;
  row.target = path;
  row.action = isDirectory ? RowAction::OpenFolder : RowAction::OpenFile;
  row.score = kDirectPathScore;
  row.quality = 5;
  row.tier = kTierApplication;
  return row;
}

}  // namespace

std::vector<ResultRow> BuildMergedRows(const std::vector<AppEntry>& apps, FileIndex* files,
                                       UsageStore* usage, const std::wstring& query, size_t limit,
                                       bool withPlaceholder) {
  std::vector<ResultRow> rows;

  if (!query.empty()) {
    const DWORD attributes = GetFileAttributesW(query.c_str());
    if (attributes != INVALID_FILE_ATTRIBUTES) {
      rows.push_back(MakePathRow(query, (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0));
    }
  }

  if (query.empty()) {
    // 空查询只推荐“最近最常访问”的条目（使用记录按频次 + 近期排序）。
    // 没有使用记录时保持空列表，面板就只剩一个输入框。
    if (usage) {
      const size_t maxRows = std::min<size_t>(limit, 50);
      for (const UsageRecord& record : usage->Top(maxRows * 3)) {
        if (rows.size() >= maxRows) break;
        if (const AppEntry* app = FindAppByTarget(apps, record.target)) {
          rows.push_back(MakeAppRow(*app, record.score, kTierApplication, 0));
          continue;
        }
        const DWORD attributes = GetFileAttributesW(record.target.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES) continue;  // 记录里的目标已不存在
        const bool isDirectory = (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        rows.push_back(MakeFileRow(record.target, isDirectory, record.score, false, 0));
      }
    }
    return rows;  // 已按使用热度排序，不再按 quality/tier 重排
  }

  for (const AppEntry* entry : FilterEntries(apps, query)) {
    const size_t bonus = usage ? usage->Bonus(entry->launchTarget) : 0;
    bool titleHit = false;
    const size_t score = ScoreEntry(*entry, query, &titleHit);
    // 标题命中=应用优先级；只有详情/启动路径命中时按“路径命中”处理（UWP 的 AUMID 除外）。
    const uint8_t tier =
        (titleHit || entry->IsUwp()) ? kTierApplication : kTierPathOnly;
    uint8_t quality = 0;
    if (titleHit) {
      if (_wcsicmp(entry->title.c_str(), query.c_str()) == 0) {
        quality = 4;
      } else if (_wcsnicmp(entry->title.c_str(), query.c_str(), query.size()) == 0) {
        quality = 2;
      } else {
        quality = 1;
      }
    }
    rows.push_back(MakeAppRow(*entry, score + bonus, tier, quality));
  }
  if (files) {
    for (const FileHit& hit : files->Search(query, kMaxFileHits)) {
      const size_t bonus = usage ? usage->Bonus(hit.path) : 0;
      rows.push_back(MakeFileRow(hit.path, hit.isDirectory, hit.score + bonus, hit.pathOnly,
                                 hit.quality));
    }
  }

  std::stable_sort(rows.begin(), rows.end(), [](const ResultRow& a, const ResultRow& b) {
    if (a.quality != b.quality) return a.quality > b.quality;
    if (a.tier != b.tier) return a.tier < b.tier;
    return a.score > b.score;
  });
  if (rows.size() > limit) rows.resize(limit);

  if (rows.empty() && withPlaceholder && !query.empty()) {
    ResultRow placeholder;
    placeholder.title = L"未找到匹配项";
    placeholder.subtitle = L"换个关键词，或直接输入完整路径";
    placeholder.glyph = L"?";
    placeholder.color = 0x64748B;
    rows.push_back(std::move(placeholder));
  }
  return rows;
}
