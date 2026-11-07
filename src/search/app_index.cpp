#include "search/app_entry.h"

#include <shlobj.h>
#include <shobjidl.h>
#include <shlwapi.h>

#include <algorithm>
#include <cstdio>
#include <cwchar>
#include <cwctype>
#include <filesystem>
#include <set>
#include <string>
#include <system_error>
#include <vector>

#include "core/com_ptr.h"
#include "core/log.h"
#include "search/settings_pages.h"

namespace {

// PKEY_AppUserModel_ID（避免引入 propkey.h 的 GUID 定义问题）
const PROPERTYKEY kPkeyAppUserModelId = {
    {0x9F4C2855, 0x9F79, 0x4B39, {0xA8, 0xD0, 0xE1, 0xD4, 0x2D, 0xE1, 0xD5, 0xF3}}, 5};

const UINT32 kPalette[] = {0x14C463, 0x2F80ED, 0x8B5CF6, 0xF59E0B,
                           0x10B981, 0x6366F1, 0xEF4444, 0x0EA5E9};

std::wstring ToLower(std::wstring text) {
  for (wchar_t& ch : text) ch = static_cast<wchar_t>(std::towlower(static_cast<wint_t>(ch)));
  return text;
}

bool FileExists(const std::wstring& path) {
  return !path.empty() && GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

std::wstring GetEnv(const wchar_t* name) {
  wchar_t buffer[MAX_PATH * 2] = {};
  const DWORD length = GetEnvironmentVariableW(name, buffer, ARRAYSIZE(buffer));
  return (length > 0 && length < ARRAYSIZE(buffer)) ? std::wstring(buffer, length) : std::wstring();
}

std::wstring ExpandEnv(const std::wstring& value) {
  wchar_t buffer[MAX_PATH * 4] = {};
  const DWORD length = ExpandEnvironmentStringsW(value.c_str(), buffer, ARRAYSIZE(buffer));
  return (length > 0 && length < ARRAYSIZE(buffer)) ? std::wstring(buffer, length - 1) : value;
}

std::wstring FirstGlyph(const std::wstring& title) {
  if (title.empty()) return L"?";
  if (title.size() >= 2 && title[0] >= 0xD800 && title[0] <= 0xDBFF) return title.substr(0, 2);
  return title.substr(0, 1);
}

UINT32 ColorFor(const std::wstring& title) {
  size_t hash = 0;
  for (wchar_t ch : title) hash = hash * 131 + static_cast<size_t>(ch);
  return kPalette[hash % ARRAYSIZE(kPalette)];
}

struct Collector {
  std::vector<AppEntry> entries;
  std::set<std::wstring> titles;
  std::set<std::wstring> targets;

  void Add(AppEntry entry, const std::wstring& dedupTarget = std::wstring()) {
    if (entry.title.empty()) return;
    const std::wstring titleKey = ToLower(entry.title);
    if (titles.count(titleKey) != 0) return;
    const std::wstring targetKey = ToLower(dedupTarget.empty() ? entry.launchTarget : dedupTarget);
    if (!targetKey.empty()) {
      if (targets.count(targetKey) != 0) return;
      targets.insert(targetKey);
    }
    titles.insert(titleKey);
    entry.glyph = FirstGlyph(entry.title);
    entry.color = ColorFor(entry.title);
    entries.push_back(std::move(entry));
  }
};

bool ResolveShortcutTarget(const std::wstring& shortcut, std::wstring* target) {
  ComPtr<IShellLinkW> link;
  HRESULT hr = CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
                                __uuidof(IShellLinkW), link.PutVoid());
  if (FAILED(hr) || !link) return false;
  ComPtr<IPersistFile> file;
  if (FAILED(link->QueryInterface(__uuidof(IPersistFile), file.PutVoid())) || !file) return false;
  if (FAILED(file->Load(shortcut.c_str(), STGM_READ))) return false;
  wchar_t path[MAX_PATH * 2] = {};
  WIN32_FIND_DATAW data{};
  if (FAILED(link->GetPath(path, ARRAYSIZE(path), &data, SLGP_RAWPATH))) return false;
  if (path[0] == L'\0') return false;
  *target = path;
  return true;
}

void CollectStartMenu(Collector* collector) {
  const wchar_t* roots[] = {L"ProgramData", L"APPDATA"};
  for (const wchar_t* root : roots) {
    const std::wstring base = GetEnv(root);
    if (base.empty()) continue;
    const std::wstring programs = base + L"\\Microsoft\\Windows\\Start Menu\\Programs";
    if (!FileExists(programs)) continue;

    std::error_code error;
    for (auto it = std::filesystem::recursive_directory_iterator(programs, error);
         !error && it != std::filesystem::recursive_directory_iterator(); it.increment(error)) {
      std::error_code entryError;
      if (!it->is_regular_file(entryError)) continue;
      const std::filesystem::path& path = it->path();
      std::wstring extension = path.extension().wstring();
      extension = ToLower(std::move(extension));
      if (extension != L".lnk") continue;

      AppEntry entry;
      entry.title = path.stem().wstring();
      entry.detail = L"应用";
      entry.launchTarget = path.wstring();
      entry.iconKey = entry.launchTarget;
      entry.kind = LaunchKind::Shell;

      std::wstring target;
      if (ResolveShortcutTarget(entry.launchTarget, &target)) {
        entry.detail = ExpandEnv(target);
      }
      collector->Add(std::move(entry), target);
    }
  }
}

void CollectAppsFolder(Collector* collector) {
  PIDLIST_ABSOLUTE folderPidl = nullptr;
  if (FAILED(SHParseDisplayName(L"shell:AppsFolder", nullptr, &folderPidl, 0, nullptr))) {
    LogError(L"AppsFolder 解析失败");
    return;
  }
  ComPtr<IShellFolder> folder;
  const HRESULT bindHr =
      SHBindToObject(nullptr, folderPidl, nullptr, __uuidof(IShellFolder), folder.PutVoid());
  CoTaskMemFree(folderPidl);
  if (FAILED(bindHr) || !folder) {
    LogHr(L"SHBindToObject(AppsFolder)", bindHr);
    return;
  }

  ComPtr<IEnumIDList> items;
  if (FAILED(folder->EnumObjects(nullptr, SHCONTF_FOLDERS | SHCONTF_NONFOLDERS, items.Put()))) {
    return;
  }
  LPITEMIDLIST child = nullptr;
  ULONG fetched = 0;
  while (items->Next(1, &child, &fetched) == S_OK && fetched == 1) {
    ComPtr<IShellItem2> item;
    if (SUCCEEDED(SHCreateItemWithParent(nullptr, folder.Get(), child,
                                         __uuidof(IShellItem2), item.PutVoid()))) {
      std::wstring title;
      LPWSTR name = nullptr;
      if (SUCCEEDED(item->GetDisplayName(SIGDN_NORMALDISPLAY, &name)) && name) {
        title = name;
        CoTaskMemFree(name);
      }

      std::wstring aumid;
      ComPtr<IPropertyStore> store;
      if (!title.empty() &&
          SUCCEEDED(item->GetPropertyStore(GPS_DEFAULT, __uuidof(IPropertyStore),
                                           store.PutVoid()))) {
        PROPVARIANT value{};
        if (SUCCEEDED(store->GetValue(kPkeyAppUserModelId, &value)) && value.vt == VT_LPWSTR &&
            value.pwszVal) {
          aumid = value.pwszVal;
        }
        PropVariantClear(&value);
      }

      if (!title.empty() && !aumid.empty()) {
        AppEntry entry;
        entry.title = title;
        entry.detail = L"应用";
        entry.launchTarget = aumid;
        entry.iconKey = L"shell:AppsFolder\\" + aumid;
        entry.kind = LaunchKind::AppUserModelId;
        collector->Add(std::move(entry), aumid);
      } else if (!title.empty()) {
        LPWSTR parsing = nullptr;
        std::wstring path;
        if (SUCCEEDED(item->GetDisplayName(SIGDN_DESKTOPABSOLUTEPARSING, &parsing)) && parsing) {
          path = parsing;
          CoTaskMemFree(parsing);
        }
        if (FileExists(path)) {
          AppEntry entry;
          entry.title = title;
          entry.detail = ExpandEnv(path);
          entry.launchTarget = path;
          entry.iconKey = path;
          entry.kind = LaunchKind::Shell;
          collector->Add(std::move(entry), path);
        }
      }
    }
    CoTaskMemFree(child);
    child = nullptr;
  }
}

void CollectAppPaths(Collector* collector) {
  const HKEY roots[] = {HKEY_LOCAL_MACHINE, HKEY_CURRENT_USER};
  for (HKEY root : roots) {
    HKEY key = nullptr;
    if (RegOpenKeyExW(root, L"Software\\Microsoft\\Windows\\CurrentVersion\\App Paths", 0,
                      KEY_READ, &key) != ERROR_SUCCESS) {
      continue;
    }
    for (DWORD index = 0;; ++index) {
      wchar_t name[256] = {};
      DWORD nameLength = ARRAYSIZE(name);
      const LONG status = RegEnumKeyExW(key, index, name, &nameLength, nullptr, nullptr, nullptr,
                                        nullptr);
      if (status != ERROR_SUCCESS) break;

      HKEY subKey = nullptr;
      if (RegOpenKeyExW(key, name, 0, KEY_READ, &subKey) != ERROR_SUCCESS) continue;
      DWORD type = 0;
      DWORD bytes = 0;
      if (RegQueryValueExW(subKey, nullptr, nullptr, &type, nullptr, &bytes) == ERROR_SUCCESS &&
          type == REG_SZ && bytes > sizeof(wchar_t)) {
        std::wstring path(bytes / sizeof(wchar_t), L'\0');
        if (RegQueryValueExW(subKey, nullptr, nullptr, &type,
                             reinterpret_cast<LPBYTE>(path.data()), &bytes) == ERROR_SUCCESS) {
          path = path.c_str();
          if (FileExists(path)) {
            const size_t slash = path.find_last_of(L"\\/");
            std::wstring title = (slash == std::wstring::npos) ? path : path.substr(slash + 1);
            const size_t dot = title.find_last_of(L'.');
            if (dot != std::wstring::npos) title.resize(dot);
            AppEntry entry;
            entry.title = title;
            entry.detail = ExpandEnv(path);
            entry.launchTarget = path;
            entry.iconKey = path;
            entry.kind = LaunchKind::Shell;
            collector->Add(std::move(entry), path);
          }
        }
      }
      RegCloseKey(subKey);
    }
    RegCloseKey(key);
  }
}

std::wstring ExeDirectory() {
  wchar_t path[MAX_PATH] = {};
  if (GetModuleFileNameW(nullptr, path, ARRAYSIZE(path)) == 0) return L".";
  std::wstring dir(path);
  const size_t slash = dir.find_last_of(L"\\/");
  if (slash != std::wstring::npos) dir.resize(slash);
  return dir;
}

std::vector<std::wstring> CachePaths() {
  static std::vector<std::wstring> paths;
  if (!paths.empty()) return paths;
  const std::wstring base = GetEnv(L"LOCALAPPDATA");
  if (!base.empty()) {
    const std::wstring dir = base + L"\\Lume";
    if (EnsureDirectory(dir)) paths.push_back(dir + L"\\apps.idx");
  }
  const std::wstring fallback = ExeDirectory() + L"\\apps.idx";
  if (std::find(paths.begin(), paths.end(), fallback) == paths.end()) {
    paths.push_back(fallback);
  }
  // 读取旧 eTools 缓存做一次迁移，写回时仍优先 Lume。
  if (!base.empty()) paths.push_back(base + L"\\eTools\\apps.idx");
  return paths;
}

FILETIME NewestStartMenuTime() {
  FILETIME newest{};
  const wchar_t* roots[] = {L"ProgramData", L"APPDATA"};
  for (const wchar_t* root : roots) {
    const std::wstring base = GetEnv(root);
    if (base.empty()) continue;
    const std::wstring programs = base + L"\\Microsoft\\Windows\\Start Menu\\Programs";
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (GetFileAttributesExW(programs.c_str(), GetFileExInfoStandard, &data) &&
        CompareFileTime(&data.ftLastWriteTime, &newest) > 0) {
      newest = data.ftLastWriteTime;
    }
  }
  return newest;
}

std::wstring SanitizeField(const std::wstring& value) {
  std::wstring out = value;
  for (wchar_t& ch : out) {
    if (ch == L'\t' || ch == L'\n' || ch == L'\r') ch = L' ';
  }
  return out;
}

std::vector<std::wstring> SplitFields(const std::wstring& line) {
  std::vector<std::wstring> fields;
  size_t start = 0;
  while (start <= line.size()) {
    const size_t tab = line.find(L'\t', start);
    fields.push_back(line.substr(start, tab == std::wstring::npos ? std::wstring::npos : tab - start));
    if (tab == std::wstring::npos) break;
    start = tab + 1;
  }
  return fields;
}

bool LoadCache(std::vector<AppEntry>* entries) {
  const FILETIME newest = NewestStartMenuTime();
  std::string data;
  bool loaded = false;
  for (const std::wstring& path : CachePaths()) {
    WIN32_FILE_ATTRIBUTE_DATA cacheData{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &cacheData)) continue;
    if (newest.dwLowDateTime != 0 &&
        CompareFileTime(&cacheData.ftLastWriteTime, &newest) < 0) {
      LogDebug(L"索引缓存早于开始菜单变更，重建: %ls", path.c_str());
      continue;
    }
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) continue;
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file, &size) || size.QuadPart <= 0 || size.QuadPart > 8 * 1024 * 1024) {
      CloseHandle(file);
      continue;
    }
    data.resize(static_cast<size_t>(size.QuadPart));
    DWORD read = 0;
    const BOOL ok = ReadFile(file, data.data(), static_cast<DWORD>(data.size()), &read, nullptr);
    CloseHandle(file);
    if (ok && static_cast<size_t>(read) == data.size()) {
      loaded = true;
      break;
    }
  }
  if (!loaded) return false;

  const std::wstring text = Utf8ToWide(data);
  size_t lineStart = 0;
  bool headerSeen = false;
  while (lineStart < text.size()) {
    size_t lineEnd = text.find(L'\n', lineStart);
    if (lineEnd == std::wstring::npos) lineEnd = text.size();
    const std::wstring line = text.substr(lineStart, lineEnd - lineStart);
    lineStart = lineEnd + 1;
    if (line.empty()) continue;
    if (!headerSeen) {
      if (line != L"v2") return false;
      headerSeen = true;
      continue;
    }
    const std::vector<std::wstring> fields = SplitFields(line);
    if (fields.size() != 6) continue;
    AppEntry entry;
    entry.kind = (fields[0] == L"uwp") ? LaunchKind::AppUserModelId : LaunchKind::Shell;
    entry.color = static_cast<UINT32>(wcstoul(fields[1].c_str(), nullptr, 16));
    entry.title = fields[2];
    entry.detail = fields[3];
    entry.iconKey = fields[4];
    entry.launchTarget = fields[5];
    entry.glyph = FirstGlyph(entry.title);
    if (!entry.title.empty()) entries->push_back(std::move(entry));
  }
  return !entries->empty();
}

void SaveCache(const std::vector<AppEntry>& entries) {
  std::string out = "v2\n";
  for (const AppEntry& entry : entries) {
    wchar_t color[16];
    swprintf(color, ARRAYSIZE(color), L"%06X", entry.color);
    const std::wstring line = std::wstring(entry.IsUwp() ? L"uwp" : L"app") + L"\t" + color + L"\t" +
                              SanitizeField(entry.title) + L"\t" + SanitizeField(entry.detail) +
                              L"\t" + SanitizeField(entry.iconKey) + L"\t" +
                              SanitizeField(entry.launchTarget) + L"\n";
    out += WideToUtf8(line);
  }
  for (const std::wstring& path : CachePaths()) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) continue;
    DWORD written = 0;
    const BOOL ok = WriteFile(file, out.data(), static_cast<DWORD>(out.size()), &written, nullptr);
    CloseHandle(file);
    if (ok && static_cast<size_t>(written) == out.size()) {
      Log(L"索引缓存已写入: %ls", path.c_str());
      return;
    }
  }
  LogError(L"索引缓存写入失败: %lu", GetLastError());
}

}  // namespace

std::vector<AppEntry> CollectAppEntries() {
  std::vector<AppEntry> cached;
  if (LoadCache(&cached)) {
    Log(L"应用索引: %zu 条（缓存）", cached.size());
    return cached;
  }

  Collector collector;
  CollectStartMenu(&collector);
  CollectAppsFolder(&collector);
  CollectAppPaths(&collector);

  std::sort(collector.entries.begin(), collector.entries.end(),
            [](const AppEntry& a, const AppEntry& b) {
              return ToLower(a.title) < ToLower(b.title);
            });
  if (collector.entries.size() > 500) collector.entries.resize(500);
  SaveCache(collector.entries);
  Log(L"应用索引: %zu 条（重建）", collector.entries.size());
  return collector.entries;
}

std::vector<AppEntry> CollectAllEntries() {
  std::vector<AppEntry> entries = CollectAppEntries();
  const size_t appCount = entries.size();
  std::vector<AppEntry> pages = CollectSettingsPages();
  entries.reserve(appCount + pages.size());
  for (AppEntry& page : pages) entries.push_back(std::move(page));
  Log(L"搜索条目: %zu（应用 %zu + 设置页 %zu）", entries.size(), appCount,
      entries.size() - appCount);
  return entries;
}

std::vector<ResultRow> BuildRowsFromEntries(const std::vector<const AppEntry*>& entries,
                                            bool withPlaceholder) {
  std::vector<ResultRow> rows;
  rows.reserve(entries.size());
  for (const AppEntry* entry : entries) {
    ResultRow row;
    row.title = entry->title;
    row.subtitle = entry->detail;
    row.glyph = entry->glyph;
    row.iconKey = entry->iconKey;
    row.color = entry->color;
    row.target = entry->launchTarget;
    if (entry->IsSettings()) {
      row.action = RowAction::OpenSettings;
    } else {
      row.action = entry->IsUwp() ? RowAction::LaunchUwp : RowAction::LaunchApp;
    }
    rows.push_back(std::move(row));
  }
  if (rows.empty() && withPlaceholder) {
    ResultRow placeholder;
    placeholder.title = L"未找到匹配项";
    placeholder.subtitle = L"换个关键词试试";
    placeholder.glyph = L"?";
    placeholder.color = 0x64748B;
    rows.push_back(std::move(placeholder));
  }
  return rows;
}
