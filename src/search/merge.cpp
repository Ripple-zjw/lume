#include "search/merge.h"

#include <algorithm>
#include <cstdint>

#include "search/file_index.h"
#include "search/usage.h"

namespace {

constexpr size_t kMaxFileHits = 80;
constexpr size_t kDirectPathScore = 1000000;

std::wstring FileNameOf(const std::wstring& path) {
  const size_t slash = path.find_last_of(L"\\/");
  return slash == std::wstring::npos ? path : path.substr(slash + 1);
}

std::wstring FirstGlyph(const std::wstring& text) {
  if (text.empty()) return L"?";
  if (text.size() >= 2 && text[0] >= 0xD800 && text[0] <= 0xDBFF) return text.substr(0, 2);
  return text.substr(0, 1);
}

ResultRow MakeAppRow(const AppEntry& entry, size_t score) {
  ResultRow row;
  row.title = entry.title;
  row.subtitle = entry.detail;
  row.glyph = entry.glyph;
  row.iconKey = entry.iconKey;
  row.color = entry.color;
  row.target = entry.launchTarget;
  row.action = entry.IsUwp() ? RowAction::LaunchUwp : RowAction::LaunchApp;
  row.score = score;
  return row;
}

ResultRow MakeFileRow(const FileEntry& entry, size_t score) {
  ResultRow row;
  row.title = FileNameOf(entry.path);
  row.subtitle = entry.path;
  row.glyph = FirstGlyph(row.title);
  row.iconKey = entry.path;
  row.color = entry.isDirectory ? 0xF59E0B : 0x64748B;
  row.target = entry.path;
  row.action = entry.isDirectory ? RowAction::OpenFolder : RowAction::OpenFile;
  row.score = score;
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
    for (const AppEntry* entry : FilterEntries(apps, L"")) {
      const size_t bonus = usage ? usage->Bonus(entry->launchTarget) : 0;
      rows.push_back(MakeAppRow(*entry, bonus));
    }
  } else {
    for (const AppEntry* entry : FilterEntries(apps, query)) {
      const size_t bonus = usage ? usage->Bonus(entry->launchTarget) : 0;
      rows.push_back(MakeAppRow(*entry, ScoreEntry(*entry, query) + bonus));
    }
    if (files) {
      for (const FileHit& hit : files->Search(query, kMaxFileHits)) {
        const size_t bonus = usage ? usage->Bonus(hit.entry.path) : 0;
        rows.push_back(MakeFileRow(hit.entry, hit.score + bonus));
      }
    }
  }

  std::stable_sort(rows.begin(), rows.end(), [](const ResultRow& a, const ResultRow& b) {
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
