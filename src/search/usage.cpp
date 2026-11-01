#include "search/usage.h"

#include <windows.h>

#include <algorithm>
#include <ctime>

#include "core/log.h"

namespace {

std::wstring GetEnv(const wchar_t* name) {
  wchar_t buffer[MAX_PATH * 2] = {};
  const DWORD length = GetEnvironmentVariableW(name, buffer, ARRAYSIZE(buffer));
  return (length > 0 && length < ARRAYSIZE(buffer)) ? std::wstring(buffer, length) : std::wstring();
}

std::wstring ExeDirectory() {
  wchar_t path[MAX_PATH] = {};
  if (GetModuleFileNameW(nullptr, path, ARRAYSIZE(path)) == 0) return L".";
  std::wstring dir(path);
  const size_t slash = dir.find_last_of(L"\\/");
  if (slash != std::wstring::npos) dir.resize(slash);
  return dir;
}

std::wstring Trim(const std::wstring& value) {
  const size_t first = value.find_first_not_of(L" \t\r\n");
  if (first == std::wstring::npos) return {};
  const size_t last = value.find_last_not_of(L" \t\r\n");
  return value.substr(first, last - first + 1);
}

std::wstring Sanitize(const std::wstring& value) {
  std::wstring out = value;
  for (wchar_t& ch : out) {
    if (ch == L'\t' || ch == L'\n' || ch == L'\r') ch = L' ';
  }
  return out;
}

size_t UsageScore(uint32_t count, uint64_t last, uint64_t now) {
  constexpr uint64_t kDay = 24 * 60 * 60;
  const uint64_t age = now > last ? now - last : 0;
  size_t score = std::min<size_t>(120, static_cast<size_t>(count) * 6);
  if (age <= kDay) {
    score += 80;
  } else if (age <= 7 * kDay) {
    score += 40;
  } else if (age <= 30 * kDay) {
    score += 10;
  }
  return score;
}

}  // namespace

std::wstring UsageStore::Path() const {
  const std::wstring base = GetEnv(L"APPDATA");
  if (!base.empty()) {
    const std::wstring dir = base + L"\\Lume";
    if (EnsureDirectory(dir)) return dir + L"\\usage.conf";
  }
  return ExeDirectory() + L"\\usage.conf";
}

bool UsageStore::Load() {
  std::wstring source = Path();
  HANDLE file = CreateFileW(source.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                            FILE_ATTRIBUTE_NORMAL, nullptr);
  bool legacy = false;
  if (file == INVALID_HANDLE_VALUE) {
    const std::wstring base = GetEnv(L"APPDATA");
    if (!base.empty()) {
      source = base + L"\\eTools\\usage.conf";
      file = CreateFileW(source.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                         FILE_ATTRIBUTE_NORMAL, nullptr);
      legacy = file != INVALID_HANDLE_VALUE;
    }
  }
  if (file == INVALID_HANDLE_VALUE) return false;
  LARGE_INTEGER size{};
  if (!GetFileSizeEx(file, &size) || size.QuadPart <= 0 || size.QuadPart > 4 * 1024 * 1024) {
    CloseHandle(file);
    return false;
  }
  std::string data(static_cast<size_t>(size.QuadPart), '\0');
  DWORD read = 0;
  const BOOL ok = ReadFile(file, data.data(), static_cast<DWORD>(data.size()), &read, nullptr);
  CloseHandle(file);
  if (!ok) return false;

  const std::wstring text = Utf8ToWide(data);
  size_t start = 0;
  while (start < text.size()) {
    size_t end = text.find(L'\n', start);
    if (end == std::wstring::npos) end = text.size();
    const std::wstring line = Trim(text.substr(start, end - start));
    start = end + 1;
    if (line.empty()) continue;
    const size_t tab1 = line.find(L'\t');
    if (tab1 == std::wstring::npos) continue;
    const size_t tab2 = line.find(L'\t', tab1 + 1);
    if (tab2 == std::wstring::npos) continue;
    Entry entry;
    entry.count = static_cast<uint32_t>(wcstoul(line.substr(0, tab1).c_str(), nullptr, 10));
    entry.last = static_cast<uint64_t>(wcstoull(line.substr(tab1 + 1, tab2 - tab1 - 1).c_str(),
                                                nullptr, 10));
    const std::wstring target = line.substr(tab2 + 1);
    if (!target.empty()) entries_[target] = entry;
  }
  if (legacy) {
    dirty_ = true;
    Save();
    Log(L"使用记录迁移: eTools -> Lume");
  }
  return true;
}

bool UsageStore::Save() const {
  if (!dirty_) return true;
  std::wstring text;
  for (const auto& pair : entries_) {
    wchar_t buffer[64];
    swprintf(buffer, ARRAYSIZE(buffer), L"%u\t%llu\t", pair.second.count,
             static_cast<unsigned long long>(pair.second.last));
    text += buffer + Sanitize(pair.first) + L"\n";
  }
  HANDLE file = CreateFileW(Path().c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                            FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    Log(L"使用记录写入失败: %lu", GetLastError());
    return false;
  }
  const std::string utf8 = WideToUtf8(text);
  DWORD written = 0;
  const BOOL ok = WriteFile(file, utf8.data(), static_cast<DWORD>(utf8.size()), &written, nullptr);
  CloseHandle(file);
  dirty_ = false;
  return ok != 0;
}

void UsageStore::Record(const std::wstring& target) {
  if (target.empty()) return;
  Entry& entry = entries_[target];
  ++entry.count;
  entry.last = static_cast<uint64_t>(std::time(nullptr));
  dirty_ = true;
}

size_t UsageStore::Bonus(const std::wstring& target) const {
  if (target.empty()) return 0;
  const auto found = entries_.find(target);
  if (found == entries_.end()) return 0;
  const uint64_t now = static_cast<uint64_t>(std::time(nullptr));
  return UsageScore(found->second.count, found->second.last, now);
}

std::vector<UsageRecord> UsageStore::Top(size_t limit) const {
  const uint64_t now = static_cast<uint64_t>(std::time(nullptr));
  std::vector<UsageRecord> records;
  records.reserve(entries_.size());
  for (const auto& pair : entries_) {
    UsageRecord record;
    record.target = pair.first;
    record.count = pair.second.count;
    record.last = pair.second.last;
    record.score = UsageScore(pair.second.count, pair.second.last, now);
    records.push_back(std::move(record));
  }
  std::stable_sort(records.begin(), records.end(), [](const UsageRecord& a, const UsageRecord& b) {
    if (a.score != b.score) return a.score > b.score;
    return a.last > b.last;  // 同分时更近的在前
  });
  if (records.size() > limit) records.resize(limit);
  return records;
}
