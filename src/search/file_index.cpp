#include "search/file_index.h"

#include <psapi.h>
#include <shlobj.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <cwctype>
#include <iterator>
#include <string_view>
#include <tuple>

#include "core/log.h"
#include "search/app_entry.h"

namespace {

constexpr size_t kHardMaxEntries = 2000000;
constexpr char kMagic[4] = {'E', 'T', 'F', '3'};
constexpr uint32_t kSnapshotVersion = 3;

std::wstring ToLower(std::wstring text) {
  for (wchar_t& ch : text) ch = static_cast<wchar_t>(std::towlower(static_cast<wint_t>(ch)));
  return text;
}

uint64_t Fnv1a(std::wstring_view text) {
  uint64_t hash = 1469598103934665603ull;
  for (wchar_t ch : text) {
    hash ^= static_cast<uint64_t>(ch);
    hash *= 1099511628211ull;
  }
  return hash;
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

bool FileExists(const std::wstring& path) {
  return !path.empty() && GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

std::wstring ExeDirectory() {
  wchar_t path[MAX_PATH] = {};
  if (GetModuleFileNameW(nullptr, path, ARRAYSIZE(path)) == 0) return L".";
  std::wstring dir(path);
  const size_t slash = dir.find_last_of(L"\\/");
  if (slash != std::wstring::npos) dir.resize(slash);
  return dir;
}

bool ShouldSkipDirectory(const std::wstring& name) {
  static const wchar_t* kSkip[] = {L"$RECYCLE.BIN", L"System Volume Information", L".git",
                                   L".svn",        L".hg",                      L"node_modules"};
  for (const wchar_t* skip : kSkip) {
    if (_wcsicmp(name.c_str(), skip) == 0) return true;
  }
  return false;
}

bool IsPathBoundary(wchar_t ch) {
  return ch == L'\\' || ch == L'/' || ch == L' ' || ch == L'-' || ch == L'_' || ch == L'.';
}

uint64_t NameMask(std::wstring_view name) {
  uint64_t mask = 0;
  for (wchar_t ch : name) {
    if (ch >= L'a' && ch <= L'z') {
      mask |= 1ull << (ch - L'a');
    } else if (ch >= L'0' && ch <= L'9') {
      mask |= 1ull << (26 + (ch - L'0'));
    }
  }
  return mask;
}

// 只用查询里的 ASCII 字母/数字做位图预筛；标点、中文被忽略，仍是必要条件。
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

// 路径命中只做“目录边界处的子串”匹配，且大幅降权。
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

std::wstring KnownFolder(int csidl) {
  wchar_t path[MAX_PATH] = {};
  if (SUCCEEDED(SHGetFolderPathW(nullptr, csidl, nullptr, SHGFP_TYPE_CURRENT, path))) {
    return path;
  }
  return std::wstring();
}

std::wstring DownloadsFolder() {
  HKEY key = nullptr;
  if (RegOpenKeyExW(HKEY_CURRENT_USER,
                    L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\User Shell Folders",
                    0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS) {
    return std::wstring();
  }
  const wchar_t* name = L"{374DE290-123F-4565-9164-39C4925E467B}";
  wchar_t buffer[MAX_PATH * 2] = {};
  DWORD bytes = sizeof(buffer);
  DWORD type = 0;
  std::wstring result;
  if (RegQueryValueExW(key, name, nullptr, &type, reinterpret_cast<LPBYTE>(buffer), &bytes) ==
          ERROR_SUCCESS &&
      type == REG_EXPAND_SZ) {
    result = ExpandEnv(buffer);
  }
  RegCloseKey(key);
  return result;
}

std::vector<std::wstring> DefaultRoots() {
  std::vector<std::wstring> roots;
  auto add = [&roots](std::wstring path) {
    if (path.empty()) return;
    while (!path.empty() && (path.back() == L'\\' || path.back() == L'/')) path.pop_back();
    if (!FileExists(path)) return;
    const std::wstring lower = ToLower(path);
    for (const std::wstring& existing : roots) {
      if (ToLower(existing) == lower) return;
    }
    roots.push_back(std::move(path));
  };
  add(KnownFolder(CSIDL_DESKTOPDIRECTORY));
  add(KnownFolder(CSIDL_PERSONAL));
  add(DownloadsFolder());
  return roots;
}

bool ReadWholeFile(const std::wstring& path, std::string* data) {
  HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                            FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) return false;
  LARGE_INTEGER size{};
  const bool haveSize = GetFileSizeEx(file, &size) != 0;
  if (!haveSize || size.QuadPart <= 0 || size.QuadPart > 512ll * 1024 * 1024) {
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

template <typename T>
bool ReadPod(const std::string& data, size_t* offset, T* value) {
  if (*offset + sizeof(T) > data.size()) return false;
  std::memcpy(value, data.data() + *offset, sizeof(T));
  *offset += sizeof(T);
  return true;
}

bool ReadWide(const std::string& data, size_t* offset, std::wstring* value) {
  uint32_t length = 0;
  if (!ReadPod(data, offset, &length) || length > 32768) return false;
  if (*offset + static_cast<size_t>(length) * sizeof(wchar_t) > data.size()) return false;
  value->assign(reinterpret_cast<const wchar_t*>(data.data() + *offset), length);
  *offset += static_cast<size_t>(length) * sizeof(wchar_t);
  return true;
}

template <typename T>
void AppendPod(std::string* data, T value) {
  data->append(reinterpret_cast<const char*>(&value), sizeof(T));
}

void AppendWide(std::string* data, const std::wstring& value) {
  AppendPod(data, static_cast<uint32_t>(value.size()));
  data->append(reinterpret_cast<const char*>(value.data()), value.size() * sizeof(wchar_t));
}

}  // namespace

FileIndex::~FileIndex() { Stop(); }

void FileIndex::SetRoots(std::vector<std::wstring> roots) {
  roots_.clear();
  for (std::wstring& root : roots) {
    if (root.empty()) continue;
    while (!root.empty() && (root.back() == L'\\' || root.back() == L'/')) root.pop_back();
    if (FileExists(root)) roots_.push_back(std::move(root));
  }
}

void FileIndex::SetMaxEntries(size_t value) {
  maxEntries_ = std::clamp<size_t>(value, 20000, kHardMaxEntries);
}

std::vector<std::wstring> FileIndex::roots() const { return roots_; }

size_t FileIndex::size() const {
  std::lock_guard<std::mutex> lock(entriesMutex_);
  return storage_.entries.size();
}

void FileIndex::EnsureRoots() {
  if (roots_.empty()) roots_ = DefaultRoots();
}

std::wstring FileIndex::FullPathOf(const Entry& entry) const {
  if (storage_.directories.empty()) return entry.name;
  return storage_.directories[entry.directory].path + L"\\" + entry.name;
}

uint32_t FileIndex::EnsureDirectoryLocked(const std::wstring& path) {
  const std::wstring lower = ToLower(path);
  auto found = storage_.directoryIndex.find(lower);
  if (found != storage_.directoryIndex.end()) return found->second;

  const size_t slash = path.find_last_of(L'\\');
  if (slash != std::wstring::npos && slash > 0) {
    EnsureDirectoryLocked(path.substr(0, slash));
  }
  Directory directory;
  directory.path = path;
  directory.lowerPath = lower;
  const uint32_t index = static_cast<uint32_t>(storage_.directories.size());
  storage_.directories.push_back(std::move(directory));
  storage_.directoryIndex.emplace(lower, index);
  return index;
}

void FileIndex::EnumerateRoot(const std::wstring& root, size_t quota, RootData* data,
                              char* truncated) {
  if (quota == 0) return;
  const std::wstring rootLower = ToLower(root);
  Directory rootDirectory;
  rootDirectory.path = root;
  rootDirectory.lowerPath = rootLower;
  const uint32_t rootIndex = static_cast<uint32_t>(data->directories.size());
  data->directories.push_back(std::move(rootDirectory));
  data->directoryIndex.emplace(rootLower, rootIndex);

  std::vector<uint32_t> pending{rootIndex};
  while (!pending.empty()) {
    if (data->entries.size() >= quota) {
      *truncated = 1;
      return;
    }
    const uint32_t directoryIndex = pending.back();
    pending.pop_back();
    // 目录表在枚举过程中会增长，先复制当前目录路径，避免引用失效。
    const std::wstring directoryPath = data->directories[directoryIndex].path;
    const std::wstring directoryLower = data->directories[directoryIndex].lowerPath;

    WIN32_FIND_DATAW findData{};
    const std::wstring pattern = directoryPath + L"\\*";
    HANDLE find = FindFirstFileExW(pattern.c_str(), FindExInfoBasic, &findData,
                                   FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
    if (find == INVALID_HANDLE_VALUE) continue;
    do {
      if (wcscmp(findData.cFileName, L".") == 0 || wcscmp(findData.cFileName, L"..") == 0) {
        continue;
      }
      if (findData.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;
      const bool isDirectory = (findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
      if (isDirectory && ShouldSkipDirectory(findData.cFileName)) continue;
      if (data->entries.size() >= quota) {
        *truncated = 1;
        break;
      }

      const std::wstring name = findData.cFileName;
      const std::wstring lowerName = ToLower(name);
      const std::wstring fullPath = directoryPath + L"\\" + name;
      const std::wstring fullLower = directoryLower + L"\\" + lowerName;

      Entry entry;
      entry.directory = directoryIndex;
      entry.name = name;
      entry.lowerName = lowerName;
      entry.charMask = NameMask(lowerName);
      entry.pathHash = Fnv1a(fullLower);
      entry.isDirectory = isDirectory;
      data->entries.push_back(std::move(entry));

      if (isDirectory) {
        auto existing = data->directoryIndex.find(fullLower);
        if (existing == data->directoryIndex.end()) {
          Directory child;
          child.path = fullPath;
          child.lowerPath = fullLower;
          const uint32_t childIndex = static_cast<uint32_t>(data->directories.size());
          data->directories.push_back(std::move(child));
          data->directoryIndex.emplace(fullLower, childIndex);
          pending.push_back(childIndex);
        }
      }
    } while (FindNextFileW(find, &findData));
    FindClose(find);
  }
}

bool FileIndex::Start() {
  EnsureRoots();
  if (roots_.empty()) {
    Log(L"文件索引: 没有可用根目录");
    return false;
  }

  for (const std::wstring& path : SnapshotPaths()) {
    if (DeleteFileW(path.c_str())) {
      Log(L"文件索引: 已删除旧快照 %ls", path.c_str());
    }
  }
  {
    std::lock_guard<std::mutex> lock(entriesMutex_);
    storage_ = Storage{};
    version_.fetch_add(1);
  }

  stop_.store(false);
  rebuildRequested_.store(true);
  worker_ = std::thread([this]() { WorkerLoop(); });
  for (const std::wstring& root : roots_) {
    watchers_.emplace_back([this, root]() { WatchLoop(root); });
  }
  Log(L"文件索引: 根目录 %zu 个, 监听已启动（启动重建中）", roots_.size());
  return true;
}

void FileIndex::Stop() {
  if (stop_.exchange(true)) return;
  {
    std::lock_guard<std::mutex> lock(watchMutex_);
    for (HANDLE handle : watchHandles_) {
      if (handle != INVALID_HANDLE_VALUE) CancelIoEx(handle, nullptr);
    }
  }
  workerCv_.notify_all();
  if (worker_.joinable()) worker_.join();
  for (std::thread& thread : watchers_) {
    if (thread.joinable()) thread.join();
  }
  watchers_.clear();
  {
    std::lock_guard<std::mutex> lock(watchMutex_);
    watchHandles_.clear();
  }
}

void FileIndex::RebuildAndSwap() {
  const size_t rootCount = roots_.size();
  const size_t perRootQuota =
      rootCount ? std::max<size_t>(1, maxEntries_ / rootCount) : 0;
  Log(L"文件索引: 根目录 %zu 个, 每根上限 %zu 条", rootCount, perRootQuota);

  std::vector<RootData> partial(roots_.size());
  std::vector<char> truncated(roots_.size(), 0);
  std::vector<std::thread> threads;
  for (size_t i = 0; i < roots_.size(); ++i) {
    threads.emplace_back([&, i]() {
      EnumerateRoot(roots_[i], perRootQuota, &partial[i], &truncated[i]);
    });
  }
  for (std::thread& thread : threads) thread.join();
  for (size_t i = 0; i < roots_.size(); ++i) {
    if (truncated[i]) {
      Log(L"文件索引: %ls 达到上限 %zu 条，已截断", roots_[i].c_str(), perRootQuota);
    }
  }

  Storage fresh;
  for (RootData& data : partial) {
    const uint32_t base = static_cast<uint32_t>(fresh.directories.size());
    for (Directory& directory : data.directories) {
      fresh.directoryIndex.emplace(directory.lowerPath, base + static_cast<uint32_t>(
                                                            fresh.directories.size()));
      fresh.directories.push_back(std::move(directory));
    }
    for (Entry& entry : data.entries) {
      entry.directory += base;
      fresh.entries.push_back(std::move(entry));
    }
  }
  {
    std::lock_guard<std::mutex> lock(entriesMutex_);
    storage_ = std::move(fresh);
    version_.fetch_add(1);
  }
  // 释放并行枚举留下的 RootData 容量，再统计常驻内存。
  partial.clear();
  partial.shrink_to_fit();
  if (persistSnapshot_) SaveSnapshot();

  PROCESS_MEMORY_COUNTERS memory{};
  if (K32GetProcessMemoryInfo(GetCurrentProcess(), &memory, sizeof(memory))) {
    Log(L"文件索引内存: 当前 %.0f MB, 峰值 %.0f MB",
        static_cast<double>(memory.WorkingSetSize) / (1024.0 * 1024.0),
        static_cast<double>(memory.PeakWorkingSetSize) / (1024.0 * 1024.0));
  }
  Log(L"文件索引: 重建完成 %zu 条", size());
}

void FileIndex::WorkerLoop() {
  bool first = true;
  while (!stop_.load()) {
    bool shouldBuild = false;
    if (first) {
      first = false;
      rebuildRequested_.store(false);
      shouldBuild = true;
    } else if (rebuildRequested_.exchange(false)) {
      shouldBuild = true;
    }
    if (shouldBuild) {
      const ULONGLONG start = GetTickCount64();
      RebuildAndSwap();
      Log(L"文件索引重建耗时: %llu ms", GetTickCount64() - start);
    }
    std::unique_lock<std::mutex> lock(workerMutex_);
    workerCv_.wait_for(lock, std::chrono::milliseconds(600), [this]() {
      return stop_.load() || rebuildRequested_.load();
    });
  }
}

void FileIndex::WatchLoop(std::wstring root) {
  HANDLE directory = CreateFileW(root.c_str(), FILE_LIST_DIRECTORY,
                                 FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                 OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
  if (directory == INVALID_HANDLE_VALUE) {
    Log(L"文件监听打开失败: %ls (%lu)", root.c_str(), GetLastError());
    return;
  }
  {
    std::lock_guard<std::mutex> lock(watchMutex_);
    watchHandles_.push_back(directory);
  }
  if (stop_.load()) {
    CloseHandle(directory);
    return;
  }

  char buffer[64 * 1024];
  while (!stop_.load()) {
    DWORD bytes = 0;
    const BOOL ok = ReadDirectoryChangesW(
        directory, buffer, sizeof(buffer), TRUE,
        FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME | FILE_NOTIFY_CHANGE_SIZE |
            FILE_NOTIFY_CHANGE_LAST_WRITE,
        &bytes, nullptr, nullptr);
    if (!ok || stop_.load()) break;
    if (bytes == 0) {
      rebuildRequested_.store(true);
    } else {
      HandleNotifications(root, buffer, bytes);
    }
    workerCv_.notify_all();
  }
  CloseHandle(directory);
}

void FileIndex::HandleNotifications(const std::wstring& root, const char* buffer, DWORD bytes) {
  size_t offset = 0;
  bool changed = false;
  while (offset + sizeof(FILE_NOTIFY_INFORMATION) <= bytes) {
    const auto* info = reinterpret_cast<const FILE_NOTIFY_INFORMATION*>(buffer + offset);
    const std::wstring relative(info->FileName, info->FileNameLength / sizeof(wchar_t));
    const std::wstring full = root + L"\\" + relative;

    switch (info->Action) {
      case FILE_ACTION_ADDED: {
        const DWORD attributes = GetFileAttributesW(full.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES) break;
        const bool isDirectory = (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        {
          std::lock_guard<std::mutex> lock(entriesMutex_);
          AddEntryLocked(full, isDirectory);
          version_.fetch_add(1);
        }
        if (isDirectory) rebuildRequested_.store(true);
        changed = true;
        break;
      }
      case FILE_ACTION_REMOVED: {
        {
          std::lock_guard<std::mutex> lock(entriesMutex_);
          RemoveEntryLocked(full);
          version_.fetch_add(1);
        }
        changed = true;
        break;
      }
      case FILE_ACTION_RENAMED_OLD_NAME:
      case FILE_ACTION_RENAMED_NEW_NAME:
        rebuildRequested_.store(true);
        break;
      default:
        break;
    }
    if (info->NextEntryOffset == 0) break;
    offset += info->NextEntryOffset;
  }
  if (changed) Log(L"文件索引: 增量更新");
}

void FileIndex::AddEntryLocked(const std::wstring& path, bool isDirectory) {
  if (storage_.entries.size() >= maxEntries_) return;
  const size_t slash = path.find_last_of(L'\\');
  if (slash == std::wstring::npos || slash + 1 >= path.size()) return;
  const std::wstring parentPath = path.substr(0, slash);
  const std::wstring name = path.substr(slash + 1);
  const std::wstring lowerName = ToLower(name);
  const std::wstring lowerFull = ToLower(parentPath) + L"\\" + lowerName;
  const uint64_t hash = Fnv1a(lowerFull);
  for (const Entry& existing : storage_.entries) {
    if (existing.pathHash == hash && FullPathOf(existing) == path) return;
  }
  const uint32_t directory = EnsureDirectoryLocked(parentPath);
  Entry entry;
  entry.directory = directory;
  entry.name = name;
  entry.lowerName = lowerName;
  entry.charMask = NameMask(lowerName);
  entry.pathHash = hash;
  entry.isDirectory = isDirectory;
  storage_.entries.push_back(std::move(entry));
}

void FileIndex::RemoveEntryLocked(const std::wstring& path) {
  const std::wstring lowerFull = ToLower(path);
  const uint64_t hash = Fnv1a(lowerFull);
  for (size_t i = 0; i < storage_.entries.size(); ++i) {
    const Entry& entry = storage_.entries[i];
    if (entry.pathHash != hash) continue;
    if (_wcsicmp(FullPathOf(entry).c_str(), path.c_str()) != 0) continue;
    if (entry.isDirectory) {
      // 目录内容未知，交给后台重建。
      rebuildRequested_.store(true);
      return;
    }
    storage_.entries.erase(storage_.entries.begin() + static_cast<std::ptrdiff_t>(i));
    return;
  }
}

std::vector<std::wstring> FileIndex::SnapshotPaths() const {
  std::vector<std::wstring> paths;
  const std::wstring base = GetEnv(L"LOCALAPPDATA");
  if (!base.empty()) {
    const std::wstring directory = base + L"\\eTools";
    if (EnsureDirectory(directory)) paths.push_back(directory + L"\\files.idx");
  }
  const std::wstring fallbackDir = ExeDirectory();
  if (EnsureDirectory(fallbackDir)) {
    const std::wstring fallback = fallbackDir + L"\\files.idx";
    if (std::find(paths.begin(), paths.end(), fallback) == paths.end()) {
      paths.push_back(fallback);
    }
  }
  return paths;
}

bool FileIndex::LoadSnapshot() {
  for (const std::wstring& path : SnapshotPaths()) {
    std::string data;
    if (!ReadWholeFile(path, &data)) continue;
    if (data.size() < sizeof(kMagic) || std::memcmp(data.data(), kMagic, sizeof(kMagic)) != 0) {
      continue;
    }
    size_t offset = sizeof(kMagic);
    uint32_t version = 0;
    if (!ReadPod(data, &offset, &version) || version != kSnapshotVersion) continue;
    uint32_t rootCount = 0;
    if (!ReadPod(data, &offset, &rootCount) || rootCount != roots_.size()) continue;

    bool rootsMatch = true;
    for (uint32_t i = 0; i < rootCount; ++i) {
      std::wstring snapshotRoot;
      if (!ReadWide(data, &offset, &snapshotRoot)) {
        rootsMatch = false;
        break;
      }
      if (ToLower(snapshotRoot) != ToLower(roots_[i])) rootsMatch = false;
    }
    if (!rootsMatch) continue;

    uint32_t count = 0;
    if (!ReadPod(data, &offset, &count) || count > kHardMaxEntries) continue;

    Storage fresh;
    bool valid = true;
    for (uint32_t i = 0; i < count; ++i) {
      std::wstring fullPath;
      uint8_t isDirectory = 0;
      if (!ReadWide(data, &offset, &fullPath) || !ReadPod(data, &offset, &isDirectory)) {
        valid = false;
        break;
      }
      const size_t slash = fullPath.find_last_of(L'\\');
      if (slash == std::wstring::npos || slash + 1 >= fullPath.size()) continue;
      const std::wstring parentPath = fullPath.substr(0, slash);
      const std::wstring parentLower = ToLower(parentPath);
      auto found = fresh.directoryIndex.find(parentLower);
      uint32_t directory = 0;
      if (found == fresh.directoryIndex.end()) {
        Directory dir;
        dir.path = parentPath;
        dir.lowerPath = parentLower;
        directory = static_cast<uint32_t>(fresh.directories.size());
        fresh.directories.push_back(std::move(dir));
        fresh.directoryIndex.emplace(parentLower, directory);
      } else {
        directory = found->second;
      }
      const std::wstring name = fullPath.substr(slash + 1);
      const std::wstring lowerName = ToLower(name);
      Entry entry;
      entry.directory = directory;
      entry.name = name;
      entry.lowerName = lowerName;
      entry.charMask = NameMask(lowerName);
      entry.pathHash = Fnv1a(parentLower + L"\\" + lowerName);
      entry.isDirectory = isDirectory != 0;
      fresh.entries.push_back(std::move(entry));
    }
    if (!valid || fresh.entries.empty()) continue;
    std::lock_guard<std::mutex> lock(entriesMutex_);
    storage_ = std::move(fresh);
    version_.fetch_add(1);
    Log(L"文件索引: 快照加载 %zu 条", storage_.entries.size());
    return true;
  }
  return false;
}

bool FileIndex::SaveSnapshot() const {
  std::lock_guard<std::mutex> lock(entriesMutex_);
  std::string out;
  out.append(kMagic, sizeof(kMagic));
  AppendPod(&out, kSnapshotVersion);
  AppendPod(&out, static_cast<uint32_t>(roots_.size()));
  for (const std::wstring& root : roots_) AppendWide(&out, root);

  uint32_t count = static_cast<uint32_t>(storage_.entries.size());
  AppendPod(&out, count);
  for (const Entry& entry : storage_.entries) {
    const std::wstring path = storage_.directories[entry.directory].path + L"\\" + entry.name;
    AppendWide(&out, path);
    const uint8_t isDirectory = entry.isDirectory ? 1 : 0;
    AppendPod(&out, isDirectory);
  }
  for (const std::wstring& path : SnapshotPaths()) {
    if (WriteWholeFile(path, out)) {
      Log(L"文件索引快照已写入: %ls (%zu 条)", path.c_str(), storage_.entries.size());
      return true;
    }
  }
  Log(L"文件索引快照写入失败: %lu", GetLastError());
  return false;
}

bool FileIndex::LoadCachedSnapshot() {
  EnsureRoots();
  if (roots_.empty()) return false;
  return LoadSnapshot();
}

std::vector<std::wstring> FileIndex::SamplePaths(size_t limit) const {
  std::vector<std::wstring> paths;
  std::lock_guard<std::mutex> lock(entriesMutex_);
  for (const Entry& entry : storage_.entries) {
    if (paths.size() >= limit) break;
    paths.push_back(storage_.directories[entry.directory].path + L"\\" + entry.name);
  }
  return paths;
}

std::vector<FileHit> FileIndex::Search(const std::wstring& query, size_t limit) const {
  std::vector<FileHit> results;
  if (query.empty()) return results;
  const std::wstring needle = ToLower(query);
  std::lock_guard<std::mutex> lock(entriesMutex_);

  struct Scored {
    size_t score;
    size_t index;
    bool pathOnly;
    uint8_t quality;
  };
  auto pathOf = [this](const Entry& entry) {
    return storage_.directories[entry.directory].path + L"\\" + entry.name;
  };
  auto better = [this](const Scored& a, const Scored& b) {
    if (a.quality != b.quality) return a.quality > b.quality;
    if (a.score != b.score) return a.score > b.score;
    const Entry& ea = storage_.entries[a.index];
    const Entry& eb = storage_.entries[b.index];
    if (ea.lowerName.size() != eb.lowerName.size()) {
      return ea.lowerName.size() < eb.lowerName.size();
    }
    if (ea.isDirectory != eb.isDirectory) return !ea.isDirectory;
    const size_t pathA = storage_.directories[ea.directory].lowerPath.size() + 1 +
                         ea.lowerName.size();
    const size_t pathB = storage_.directories[eb.directory].lowerPath.size() + 1 +
                         eb.lowerName.size();
    if (pathA != pathB) return pathA < pathB;
    return std::tie(storage_.directories[ea.directory].lowerPath, ea.lowerName) <
           std::tie(storage_.directories[eb.directory].lowerPath, eb.lowerName);
  };

  std::vector<Scored> scored;
  scored.reserve(1024);
  const uint64_t required = QueryMask(needle);
  for (size_t i = 0; i < storage_.entries.size(); ++i) {
    const Entry& entry = storage_.entries[i];
    const std::wstring_view name = entry.lowerName;
    size_t score = 0;
    if (required == 0 || (entry.charMask & required) == required) {
      if (needle.size() < 3) {
        if (name.find(needle) != std::wstring_view::npos) {
          score = ScoreLowerText(name, needle);
        }
      } else {
        score = ScoreLowerText(name, needle);
      }
    }
    if (score > 0) {
      score += 40;
      scored.push_back({score, i, false, NameQuality(name, needle)});
    }
  }

  // 只有文件名完全没有结果时才用路径兜底：路径命中优先级最低，也避免精确查询被全盘路径扫描拖慢。
  if (scored.empty()) {
    std::vector<uint8_t> nameMatched(storage_.entries.size(), 0);
    for (const Scored& item : scored) nameMatched[item.index] = 1;
    constexpr size_t kMaxPathHits = 2000;
    size_t pathHits = 0;
    for (size_t i = 0; i < storage_.entries.size() && pathHits < kMaxPathHits; ++i) {
      if (nameMatched[i]) continue;
      const Entry& entry = storage_.entries[i];
      const size_t score = ScorePathOnly(storage_.directories[entry.directory].lowerPath, needle);
      if (score > 0) {
        scored.push_back({score, i, true, 0});
        ++pathHits;
      }
    }
  }

  if (scored.size() > limit) {
    std::partial_sort(scored.begin(), scored.begin() + limit, scored.end(), better);
    scored.resize(limit);
  } else {
    std::sort(scored.begin(), scored.end(), better);
  }
  results.reserve(scored.size());
  for (const Scored& item : scored) {
    const Entry& entry = storage_.entries[item.index];
    FileHit hit;
    hit.path = pathOf(entry);
    hit.isDirectory = entry.isDirectory;
    hit.score = item.score;
    hit.pathOnly = item.pathOnly;
    hit.quality = item.quality;
    results.push_back(std::move(hit));
  }
  return results;
}

size_t FileIndex::RebuildSynchronously() {
  EnsureRoots();
  if (roots_.empty()) return 0;
  RebuildAndSwap();
  return size();
}
