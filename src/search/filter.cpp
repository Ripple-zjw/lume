#include "search/filter.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cwctype>

namespace {

std::wstring Trim(const std::wstring& value) {
  const size_t first = value.find_first_not_of(L" \t\r\n");
  if (first == std::wstring::npos) return {};
  const size_t last = value.find_last_not_of(L" \t\r\n");
  return value.substr(first, last - first + 1);
}

wchar_t Lower(wchar_t ch) { return static_cast<wchar_t>(std::towlower(static_cast<wint_t>(ch))); }

bool EqualsIgnoreCase(const std::wstring& a, const std::wstring& b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i) {
    if (Lower(a[i]) != Lower(b[i])) return false;
  }
  return true;
}

bool IsExtensionChar(wchar_t ch) {
  // 允许连字符与下划线：Windows 里确实存在 appref-ms、h_in 这类扩展名。
  return (ch >= L'a' && ch <= L'z') || (ch >= L'0' && ch <= L'9') || ch == L'-' || ch == L'_';
}

// "*.PNG" / ".png" / "png" 都归一成 "png"；含非法字符或过长返回空。
std::wstring NormalizeExtensionToken(const std::wstring& token) {
  std::wstring text = Trim(token);
  while (!text.empty() && (text.front() == L'*' || text.front() == L'.')) text.erase(text.begin());
  while (!text.empty() && text.back() == L'.') text.pop_back();
  if (text.empty() || text.size() > kMaxExtensionLength) return {};
  for (wchar_t& ch : text) {
    ch = Lower(ch);
    if (!IsExtensionChar(ch)) return {};
  }
  return text;
}

std::vector<std::wstring> SplitTokens(const std::wstring& text) {
  std::vector<std::wstring> tokens;
  std::wstring current;
  for (wchar_t ch : text) {
    if (ch == L',' || ch == L';' || ch == L' ' || ch == L'\t' || ch == L'\r' || ch == L'\n') {
      if (!current.empty()) {
        tokens.push_back(current);
        current.clear();
      }
      continue;
    }
    current.push_back(ch);
  }
  if (!current.empty()) tokens.push_back(current);
  return tokens;
}

std::vector<std::wstring> SplitFields(const std::wstring& text) {
  std::vector<std::wstring> fields;
  size_t start = 0;
  while (true) {
    const size_t bar = text.find(L'|', start);
    if (bar == std::wstring::npos) {
      fields.push_back(text.substr(start));
      break;
    }
    fields.push_back(text.substr(start, bar - start));
    start = bar + 1;
  }
  return fields;
}

}  // namespace

FilterSet DefaultFilterSet() {
  FilterSet set;
  auto make = [](const wchar_t* name, std::initializer_list<const wchar_t*> extensions,
                 bool includeApps) {
    FilterGroup group;
    group.name = name;
    group.includeApps = includeApps;
    for (const wchar_t* extension : extensions) group.extensions.push_back(extension);
    return group;
  };
  set.groups.push_back(make(L"应用程序",
                            {L"exe", L"lnk", L"bat", L"cmd", L"com", L"msi", L"appref-ms", L"ps1"},
                            true));
  set.groups.push_back(make(L"图片",
                            {L"png", L"jpg", L"jpeg", L"gif", L"bmp", L"webp", L"svg", L"ico",
                             L"tif", L"tiff", L"heic", L"avif"},
                            false));
  set.groups.push_back(make(L"文档",
                            {L"pdf", L"doc", L"docx", L"xls", L"xlsx", L"ppt", L"pptx", L"txt",
                             L"md", L"rtf", L"csv"},
                            false));
  set.groups.push_back(make(L"音频", {L"mp3", L"flac", L"wav", L"m4a", L"aac", L"ogg", L"wma"},
                            false));
  set.groups.push_back(make(L"视频", {L"mp4", L"mkv", L"avi", L"mov", L"wmv", L"webm", L"flv",
                                      L"m4v"},
                            false));
  set.groups.push_back(make(L"压缩包", {L"zip", L"7z", L"rar", L"tar", L"gz", L"tgz", L"bz2",
                                        L"xz", L"iso"},
                            false));
  set.active = 0;
  return set;
}

FilterGroup SanitizeFilterGroup(FilterGroup group) {
  group.name = Trim(group.name);
  group.name.erase(std::remove(group.name.begin(), group.name.end(), L'|'), group.name.end());
  if (group.name.size() > kMaxFilterNameLength) group.name.resize(kMaxFilterNameLength);

  std::vector<std::wstring> extensions;
  for (const std::wstring& token : group.extensions) {
    std::wstring extension = NormalizeExtensionToken(token);
    if (extension.empty()) continue;
    if (std::find(extensions.begin(), extensions.end(), extension) != extensions.end()) continue;
    if (extensions.size() >= kMaxFilterExtensions) break;
    extensions.push_back(std::move(extension));
  }
  group.extensions = std::move(extensions);
  return group;
}

std::vector<std::wstring> ParseExtensionList(const std::wstring& text) {
  FilterGroup parsed;
  for (const std::wstring& token : SplitTokens(text)) parsed.extensions.push_back(token);
  return SanitizeFilterGroup(std::move(parsed)).extensions;
}

void ClampFilterActive(FilterSet* set) {
  if (!set) return;
  const int chips = static_cast<int>(set->groups.size()) + 1;
  if (set->active < 0) set->active = 0;
  if (set->active >= chips) set->active = chips - 1;
}

bool SanitizeFilterSet(FilterSet* set, std::wstring* error) {
  if (!set) return false;
  auto fail = [error](std::wstring message) {
    if (error) *error = std::move(message);
    return false;
  };
  if (set->groups.size() > kMaxFilterGroups) {
    wchar_t message[128] = {};
    swprintf(message, sizeof(message) / sizeof(message[0]), L"筛选组最多 %zu 个（当前 %zu 个）",
             kMaxFilterGroups, set->groups.size());
    return fail(message);
  }

  std::vector<FilterGroup> groups;
  groups.reserve(set->groups.size());
  for (const FilterGroup& raw : set->groups) {
    FilterGroup group = SanitizeFilterGroup(raw);
    if (group.name.empty()) return fail(L"筛选组名称不能为空");
    if (EqualsIgnoreCase(group.name, L"全部")) return fail(L"「全部」是内置组名，请换一个名称");
    for (const FilterGroup& existing : groups) {
      if (EqualsIgnoreCase(existing.name, group.name)) {
        return fail(L"筛选组名称重复：" + group.name);
      }
    }
    if (group.extensions.empty() && !group.includeApps && !group.includeFolders &&
        !group.includeNoExtension) {
      return fail(L"「" + group.name + L"」没有任何规则（至少一个扩展名或一个开关）");
    }
    groups.push_back(std::move(group));
  }

  set->groups = std::move(groups);
  ClampFilterActive(set);
  return true;
}

const FilterGroup* ActiveFilterGroup(const FilterSet& set) {
  if (!set.enabled) return nullptr;
  if (set.active <= 0 || set.active > static_cast<int>(set.groups.size())) return nullptr;
  return &set.groups[static_cast<size_t>(set.active - 1)];
}

std::vector<std::wstring> FilterChipLabels(const FilterSet& set) {
  std::vector<std::wstring> labels;
  labels.reserve(set.groups.size() + 1);
  labels.push_back(L"全部");
  for (const FilterGroup& group : set.groups) labels.push_back(group.name);
  return labels;
}

bool FilterMatchesPath(const FilterGroup& group, std::wstring_view path, bool isDirectory) {
  if (isDirectory) return group.includeFolders;
  const size_t slash = path.find_last_of(L"\\/");
  const std::wstring_view name = (slash == std::wstring_view::npos) ? path : path.substr(slash + 1);
  const size_t dot = name.find_last_of(L'.');
  // 没有点、只有隐藏文件前缀的点、或以点结尾，都按「无扩展名」处理。
  if (dot == std::wstring_view::npos || dot == 0 || dot + 1 >= name.size()) {
    return group.includeNoExtension;
  }
  const std::wstring_view tail = name.substr(dot + 1);
  if (tail.size() > kMaxExtensionLength) return false;
  wchar_t extension[kMaxExtensionLength + 1] = {};
  for (size_t i = 0; i < tail.size(); ++i) extension[i] = Lower(tail[i]);
  for (const std::wstring& candidate : group.extensions) {
    if (candidate.size() != tail.size()) continue;
    if (wcscmp(candidate.c_str(), extension) == 0) return true;
  }
  return false;
}

bool FilterIsExtensionWhitelist(const FilterGroup& group) {
  return !group.extensions.empty() && !group.includeFolders && !group.includeNoExtension;
}

std::wstring FilterEverythingSpec(const FilterGroup& group) {
  std::wstring spec;
  for (const std::wstring& extension : group.extensions) {
    if (!spec.empty()) spec.push_back(L';');
    for (wchar_t ch : extension) spec.push_back(Lower(ch));
  }
  return spec;
}

std::wstring EncodeFilterGroup(const FilterGroup& group) {
  std::wstring encoded = group.name;
  encoded.erase(std::remove(encoded.begin(), encoded.end(), L'|'), encoded.end());
  encoded.push_back(L'|');
  for (size_t i = 0; i < group.extensions.size(); ++i) {
    if (i > 0) encoded.push_back(L';');
    encoded += group.extensions[i];
  }
  encoded.push_back(L'|');
  encoded += group.includeApps ? L'1' : L'0';
  encoded.push_back(L'|');
  encoded += group.includeFolders ? L'1' : L'0';
  encoded.push_back(L'|');
  encoded += group.includeNoExtension ? L'1' : L'0';
  return encoded;
}

bool DecodeFilterGroup(const std::wstring& value, FilterGroup* group) {
  if (!group) return false;
  const std::vector<std::wstring> fields = SplitFields(value);
  if (fields.size() != 5) return false;

  FilterGroup decoded;
  decoded.name = Trim(fields[0]);
  if (decoded.name.empty()) return false;
  for (const std::wstring& token : SplitTokens(fields[1])) decoded.extensions.push_back(token);
  auto flag = [](const std::wstring& field) {
    const std::wstring text = Trim(field);
    return text == L"1" || EqualsIgnoreCase(text, L"true") || EqualsIgnoreCase(text, L"on") ||
           EqualsIgnoreCase(text, L"yes");
  };
  decoded.includeApps = flag(fields[2]);
  decoded.includeFolders = flag(fields[3]);
  decoded.includeNoExtension = flag(fields[4]);

  decoded = SanitizeFilterGroup(std::move(decoded));
  if (decoded.name.empty()) return false;
  *group = std::move(decoded);
  return true;
}

bool ParseFilterSpec(const FilterSet& set, const std::wstring& spec, bool* enabled, int* index) {
  const std::wstring text = Trim(spec);
  if (text.empty()) return false;
  if (EqualsIgnoreCase(text, L"off") || text == L"0") {
    if (enabled) *enabled = false;
    if (index) *index = 0;
    return true;
  }

  const std::vector<std::wstring> labels = FilterChipLabels(set);
  bool digits = true;
  for (wchar_t ch : text) {
    if (ch < L'0' || ch > L'9') {
      digits = false;
      break;
    }
  }
  if (digits) {
    const int number = static_cast<int>(wcstol(text.c_str(), nullptr, 10));
    if (number < 1 || number > static_cast<int>(labels.size())) return false;
    if (enabled) *enabled = true;
    if (index) *index = number - 1;
    return true;
  }
  for (size_t i = 0; i < labels.size(); ++i) {
    if (EqualsIgnoreCase(labels[i], text)) {
      if (enabled) *enabled = true;
      if (index) *index = static_cast<int>(i);
      return true;
    }
  }
  return false;
}
