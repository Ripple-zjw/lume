#include "app/settings.h"

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

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

bool ReadWholeFile(const std::wstring& path, std::string* data) {
  HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                            FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) return false;
  LARGE_INTEGER size{};
  if (!GetFileSizeEx(file, &size) || size.QuadPart <= 0 || size.QuadPart > 1024 * 1024) {
    CloseHandle(file);
    return false;
  }
  data->resize(static_cast<size_t>(size.QuadPart));
  DWORD read = 0;
  const BOOL ok = ReadFile(file, data->data(), static_cast<DWORD>(data->size()), &read, nullptr);
  CloseHandle(file);
  return ok != 0 && static_cast<size_t>(read) == data->size();
}

bool WriteWholeFile(const std::wstring& path, const std::string& data) {
  HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                            FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) return false;
  DWORD written = 0;
  const BOOL ok = WriteFile(file, data.data(), static_cast<DWORD>(data.size()), &written, nullptr);
  CloseHandle(file);
  return ok != 0 && static_cast<size_t>(written) == data.size();
}

std::wstring Trim(const std::wstring& value) {
  const size_t first = value.find_first_not_of(L" \t\r\n");
  if (first == std::wstring::npos) return {};
  const size_t last = value.find_last_not_of(L" \t\r\n");
  return value.substr(first, last - first + 1);
}

}  // namespace

std::wstring SettingsStore::Path() {
  const std::wstring base = GetEnv(L"APPDATA");
  if (!base.empty()) {
    const std::wstring dir = base + L"\\Lume";
    if (EnsureDirectory(dir)) return dir + L"\\settings.conf";
  }
  return ExeDirectory() + L"\\settings.conf";
}

namespace {
std::vector<std::wstring> Paths() {
  std::vector<std::wstring> paths;
  const std::wstring base = GetEnv(L"APPDATA");
  if (!base.empty()) {
    const std::wstring dir = base + L"\\Lume";
    if (EnsureDirectory(dir)) paths.push_back(dir + L"\\settings.conf");
  }
  const std::wstring fallback = ExeDirectory() + L"\\settings.conf";
  if (std::find(paths.begin(), paths.end(), fallback) == paths.end()) paths.push_back(fallback);
  return paths;
}

std::vector<std::wstring> LegacyPaths() {
  std::vector<std::wstring> paths;
  const std::wstring base = GetEnv(L"APPDATA");
  if (!base.empty()) {
    paths.push_back(base + L"\\eTools\\settings.conf");
  }
  return paths;
}
}  // namespace

bool SettingsStore::Load(AppSettings* settings) {
  std::string data;
  bool loaded = false;
  bool legacy = false;
  auto tryLoad = [&](const std::vector<std::wstring>& paths, bool isLegacy) {
    for (const std::wstring& path : paths) {
      if (ReadWholeFile(path, &data)) {
        loaded = true;
        legacy = isLegacy;
        return true;
      }
    }
    return false;
  };
  if (!tryLoad(Paths(), false)) tryLoad(LegacyPaths(), true);
  if (!loaded) return false;
  const std::wstring text = Utf8ToWide(data);
  bool sawFilterGroup = false;
  size_t start = 0;
  while (start < text.size()) {
    size_t end = text.find(L'\n', start);
    if (end == std::wstring::npos) end = text.size();
    const std::wstring line = Trim(text.substr(start, end - start));
    start = end + 1;
    if (line.empty() || line[0] == L'#') continue;
    const size_t equals = line.find(L'=');
    if (equals == std::wstring::npos) continue;
    const std::wstring key = Trim(line.substr(0, equals));
    const std::wstring value = Trim(line.substr(equals + 1));
    if (key == L"version") {
      // 预留版本字段，当前忽略。
    } else if (key == L"theme") {
      settings->theme = value;
    } else if (key == L"accent") {
      settings->accent = value;
    } else if (key == L"hotkey") {
      settings->hotkey = value;
    } else if (key == L"autostart") {
      settings->autostart = (value == L"1" || value == L"true");
    } else if (key == L"tray") {
      settings->tray = (value == L"1" || value == L"true");
    } else if (key == L"indexSizeMB") {
      const int megabytes = _wtoi(value.c_str());
      if (megabytes >= 64 && megabytes <= 262144) settings->indexSizeMB = megabytes;
    } else if (key == L"indexDir" && !value.empty()) {
      settings->indexDir = value;
    } else if (key == L"indexDirHistory" && !value.empty()) {
      if (settings->indexDirHistory.size() < 5) settings->indexDirHistory.push_back(value);
    } else if (key == L"fileRoot" && !value.empty()) {
      settings->fileRoots.push_back(value);
    } else if (key == L"fileSearch") {
      // 只接受已知取值，避免手写配置把来源写坏。
      settings->fileSearch = (value == L"everything") ? L"everything" : L"local";
    } else if (key == L"filterEnabled") {
      settings->filters.enabled =
          !(value == L"0" || value == L"false" || value == L"off" || value == L"no");
    } else if (key == L"filterActive") {
      settings->filters.active = _wtoi(value.c_str());
    } else if (key == L"filterGroup") {
      // 组行按出现顺序排列；第一条组行出现时丢掉默认组，避免默认 + 自定义混在一起。
      if (!sawFilterGroup) {
        settings->filters.groups.clear();
        sawFilterGroup = true;
      }
      FilterGroup group;
      if (!DecodeFilterGroup(value, &group)) {
        Log(L"设置: 忽略无法解析的筛选组 \"%ls\"", value.c_str());
      } else {
        bool duplicate = false;
        for (const FilterGroup& existing : settings->filters.groups) {
          if (_wcsicmp(existing.name.c_str(), group.name.c_str()) == 0) duplicate = true;
        }
        if (duplicate) {
          Log(L"设置: 忽略重名的筛选组 \"%ls\"", group.name.c_str());
        } else if (settings->filters.groups.size() >= kMaxFilterGroups) {
          Log(L"设置: 筛选组超过上限 %zu，忽略 \"%ls\"", kMaxFilterGroups, group.name.c_str());
        } else {
          settings->filters.groups.push_back(std::move(group));
        }
      }
    }
  }
  ClampFilterActive(&settings->filters);
  if (legacy) {
    Log(L"设置迁移: eTools -> Lume");
    Save(*settings);
  }
  return true;
}

bool SettingsStore::Save(const AppSettings& settings, std::wstring* savedPath) {
  std::wstring text;
  text += L"# Lume settings\n";
  text += L"version=3\n";
  text += L"theme=" + settings.theme + L"\n";
  text += L"accent=" + settings.accent + L"\n";
  text += L"hotkey=" + settings.hotkey + L"\n";
  text += std::wstring(L"autostart=") + (settings.autostart ? L"1" : L"0") + L"\n";
  text += std::wstring(L"tray=") + (settings.tray ? L"1" : L"0") + L"\n";
  text += L"indexSizeMB=" + std::to_wstring(settings.indexSizeMB) + L"\n";
  if (!settings.indexDir.empty()) text += L"indexDir=" + settings.indexDir + L"\n";
  for (const std::wstring& dir : settings.indexDirHistory) {
    if (!dir.empty()) text += L"indexDirHistory=" + dir + L"\n";
  }
  text += std::wstring(L"fileSearch=") +
          (settings.fileSearch == L"everything" ? L"everything" : L"local") + L"\n";
  text += std::wstring(L"filterEnabled=") + (settings.filters.enabled ? L"1" : L"0") + L"\n";
  text += L"filterActive=" + std::to_wstring(settings.filters.active) + L"\n";
  for (const FilterGroup& group : settings.filters.groups) {
    text += L"filterGroup=" + EncodeFilterGroup(group) + L"\n";
  }
  for (const std::wstring& root : settings.fileRoots) {
    text += L"fileRoot=" + root + L"\n";
  }
  for (const std::wstring& path : Paths()) {
    if (WriteWholeFile(path, WideToUtf8(text))) {
      Log(L"设置已保存: %ls", path.c_str());
      if (savedPath) *savedPath = path;
      return true;
    }
  }
  Log(L"设置写入失败: %ls", Path().c_str());
  return false;
}
