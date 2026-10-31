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
#include <unordered_set>

#include "core/log.h"
#include "search/app_entry.h"

namespace {

constexpr size_t kHardMaxEntries = 2000000;  // 快照/索引的硬上限，防止异常文件撑爆内存
constexpr char kMagic[4] = {'E', 'T', 'F', '3'};
constexpr uint32_t kSnapshotVersion = 3;

std::wstring ToLower(std::wstring text) {
  for (wchar_t& ch : text) ch = static_cast<wchar_t>(std::towlower(static_cast<wint_t>(ch)));
  return text;
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

// 查询含非 ASCII 字母/数字时返回 0，表示不做位图预筛。
uint64_t QueryMask(const std::wstring& needle) {
  for (wchar_t ch : needle) {
    const bool alnum = (ch >= L'a' && ch <= L'z') || (ch >= L'0' && ch <= L'9');
    if (!alnum) return 0;
  }
  return NameMask(needle);
}

// 路径命中只做“目录/文件名边界处的子串”匹配，且大幅降权：
// 典型分数 ~150–200，低于文件名子序列（300+），不会盖过文件名结果。
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
  if (RegQueryValueExW(key, name, nullptr, &type, reinterpret_cast<LPBYTE>(buffer),
                       &bytes) == ERROR_SUCCESS &&
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

void EnumerateRoot(const std::wstring& root, size_t quota, std::vector<FileEntry>* out,
                   char* truncated) {
  if (quota == 0) return;
  std::vector<std::wstring> pending{root};
  while (!pending.empty()) {
    if (out->size() >= quota) {
      *truncated = 1;
      return;
    }
    std::wstring directory = std::move(pending.back());
    pending.pop_back();

    WIN32_FIND_DATAW data{};
    const std::wstring pattern = directory + L"\\*";
    HANDLE find = FindFirstFileExW(pattern.c_str(), FindExInfoBasic, &data, FindExSearchNameMatch,
                                   nullptr, FIND_FIRST_EX_LARGE_FETCH);
    if (find == INVALID_HANDLE_VALUE) continue;
    do {
      if (wcscmp(data.cFileName, L".") == 0 || wcscmp(data.cFileName, L"..") == 0) continue;
      if (data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;
      const bool isDirectory = (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
      if (isDirectory && ShouldSkipDirectory(data.cFileName)) continue;
      if (out->size() >= quota) {
        *truncated = 1;
        break;
      }

      std::wstring path = directory + L"\\" + data.cFileName;
      std::wstring lower = ToLower(path);
      const size_t slash = lower.find_last_of(L'\\');
      FileEntry entry;
      entry.nameOffset = static_cast<uint32_t>(slash == std::wstring::npos ? 0 : slash + 1);
      entry.charMask = NameMask(std::wstring_view(lower).substr(entry.nameOffset));
      entry.lowerPath = std::move(lower);
      entry.path = std::move(path);
      entry.isDirectory = isDirectory;
      out->push_back(std::move(entry));
      if (isDirectory) pending.push_back(out->back().path);
    } while (FindNextFileW(find, &data));
    FindClose(find);
  }
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
void AppendPod(std::string* out, const T& value) {
  out->append(reinterpret_cast<const char*>(&value), sizeof(T));
}

void AppendWide(std::string* out, const std::wstring& value) {
  const uint16_t length = static_cast<uint16_t>(std::min<size_t>(value.size(), 65535));
  AppendPod(out, length);
  out->append(reinterpret_cast<const char*>(value.data()), static_cast<size_t>(length) * sizeof(wchar_t));
}

template <typename T>
bool ReadPod(const std::string& data, size_t* offset, T* value) {
  if (*offset + sizeof(T) > data.size()) return false;
  std::memcpy(value, data.data() + *offset, sizeof(T));
  *offset += sizeof(T);
  return true;
}

bool ReadWide(const std::string& data, size_t* offset, std::wstring* value) {
  uint16_t length = 0;
  if (!ReadPod(data, offset, &length)) return false;
  const size_t bytes = static_cast<size_t>(length) * sizeof(wchar_t);
  if (*offset + bytes > data.size()) return false;
  value->assign(reinterpret_cast<const wchar_t*>(data.data() + *offset), length);
  *offset += bytes;
  return true;
}

}  // namespace

FileIndex::~FileIndex() { Stop(); }

void FileIndex::EnsureRoots() {
  if (roots_.empty()) roots_ = DefaultRoots();
}

void FileIndex::SetRoots(std::vector<std::wstring> roots) {
  roots_.clear();
  for (std::wstring& root : roots) {
    if (root.empty()) continue;
    while (!root.empty() && (root.back() == L'\\' || root.back() == L'/')) root.pop_back();
    if (FileExists(root)) roots_.push_back(std::move(root));
  }
}

void FileIndex::SetMaxEntries(size_t value) {
  maxEntries_ = std::clamp<size_t>(value, 1000, 2000000);
}

bool FileIndex::Start() {
  EnsureRoots();
  if (roots_.empty()) {
    Log(L"文件索引: 没有可用根目录");
    return false;
  }

  // 启动即丢弃旧快照，避免把上次运行期间已删除的文件继续搜出来；后台会全量重建。
  for (const std::wstring& path : SnapshotPaths()) {
    if (DeleteFileW(path.c_str())) {
      Log(L"文件索引: 已删除旧快照 %ls", path.c_str());
    }
  }
  {
    std::lock_guard<std::mutex> lock(entriesMutex_);
    entries_.clear();
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

std::vector<std::wstring> FileIndex::roots() const { return roots_; }

size_t FileIndex::size() const {
  std::lock_guard<std::mutex> lock(entriesMutex_);
  return entries_.size();
}

std::vector<std::wstring> FileIndex::SnapshotPaths() const {
  std::vector<std::wstring> paths;
  const std::wstring base = GetEnv(L"LOCALAPPDATA");
  if (!base.empty()) {
    const std::wstring dir = base + L"\\eTools";
    if (EnsureDirectory(dir)) paths.push_back(dir + L"\\files.idx");
  }
  const std::wstring fallback = ExeDirectory() + L"\\files.idx";
  if (std::find(paths.begin(), paths.end(), fallback) == paths.end()) paths.push_back(fallback);
  return paths;
}

bool FileIndex::LoadSnapshot(std::vector<FileEntry>* entries) const {
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
    entries->clear();
    entries->reserve(count);
    bool valid = true;
    for (uint32_t i = 0; i < count; ++i) {
      std::wstring entryPath;
      uint8_t isDirectory = 0;
      if (!ReadWide(data, &offset, &entryPath) || !ReadPod(data, &offset, &isDirectory)) {
        valid = false;
        break;
      }
      FileEntry entry;
      entry.lowerPath = ToLower(entryPath);
      const size_t slash = entry.lowerPath.find_last_of(L'\\');
      entry.nameOffset = static_cast<uint32_t>(slash == std::wstring::npos ? 0 : slash + 1);
      entry.charMask = NameMask(std::wstring_view(entry.lowerPath).substr(entry.nameOffset));
      entry.path = std::move(entryPath);
      entry.isDirectory = isDirectory != 0;
      entries->push_back(std::move(entry));
    }
    if (valid && !entries->empty()) return true;
    entries->clear();
  }
  return false;
}

void FileIndex::SaveSnapshot(const std::vector<FileEntry>& entries) {
  std::string out;
  out.append(kMagic, sizeof(kMagic));
  AppendPod(&out, kSnapshotVersion);
  AppendPod(&out, static_cast<uint32_t>(roots_.size()));
  for (const std::wstring& root : roots_) AppendWide(&out, root);
  AppendPod(&out, static_cast<uint32_t>(entries.size()));
  for (const FileEntry& entry : entries) {
    AppendWide(&out, entry.path);
    const uint8_t isDirectory = entry.isDirectory ? 1 : 0;
    AppendPod(&out, isDirectory);
  }
  for (const std::wstring& path : SnapshotPaths()) {
    if (WriteWholeFile(path, out)) {
      Log(L"文件索引快照已写入: %ls (%zu 条)", path.c_str(), entries.size());
      return;
    }
  }
  Log(L"文件索引快照写入失败: %lu", GetLastError());
}

void FileIndex::RebuildAndSwap() {
  std::vector<FileEntry> fresh;
  fresh.reserve(20000);
  const size_t rootCount = roots_.size();
  const size_t perRootQuota = rootCount ? std::max<size_t>(1, maxEntries_ / rootCount) : 0;
  Log(L"文件索引: 根目录 %zu 个, 每根上限 %zu 条", rootCount, perRootQuota);

  std::vector<std::thread> threads;
  std::vector<std::vector<FileEntry>> partial(roots_.size());
  std::vector<char> truncated(roots_.size(), 0);
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
  for (auto& part : partial) {
    fresh.insert(fresh.end(), std::make_move_iterator(part.begin()),
                 std::make_move_iterator(part.end()));
  }
  std::sort(fresh.begin(), fresh.end(), [](const FileEntry& a, const FileEntry& b) {
    return a.lowerPath < b.lowerPath;
  });
  {
    std::lock_guard<std::mutex> lock(entriesMutex_);
    entries_ = std::move(fresh);
    version_.fetch_add(1);
  }
  if (persistSnapshot_) SaveSnapshot(entries_);
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
  bool sizeChanged = false;
  while (offset + sizeof(FILE_NOTIFY_INFORMATION) <= bytes) {
    const auto* info = reinterpret_cast<const FILE_NOTIFY_INFORMATION*>(buffer + offset);
    std::wstring relative(info->FileName, info->FileNameLength / sizeof(wchar_t));
    const std::wstring full = root + L"\\" + relative;
    const std::wstring lower = ToLower(full);

    switch (info->Action) {
      case FILE_ACTION_ADDED: {
        const DWORD attributes = GetFileAttributesW(full.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES) break;
        const bool isDirectory = (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        if (isDirectory) {
          rebuildRequested_.store(true);  // 目录内容未知，交给后台重建
        } else {
          std::lock_guard<std::mutex> lock(entriesMutex_);
          AddEntryLocked(full, false);
          version_.fetch_add(1);
          sizeChanged = true;
        }
        break;
      }
      case FILE_ACTION_REMOVED: {
        std::lock_guard<std::mutex> lock(entriesMutex_);
        RemoveEntryLocked(lower);
        version_.fetch_add(1);
        sizeChanged = true;
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
  if (sizeChanged) Log(L"文件索引: 增量更新");
}

void FileIndex::AddEntryLocked(const std::wstring& path, bool isDirectory) {
  if (entries_.size() >= maxEntries_) return;
  FileEntry entry;
  entry.lowerPath = ToLower(path);
  const size_t slash = entry.lowerPath.find_last_of(L'\\');
  entry.nameOffset = static_cast<uint32_t>(slash == std::wstring::npos ? 0 : slash + 1);
  entry.charMask = NameMask(std::wstring_view(entry.lowerPath).substr(entry.nameOffset));
  entry.path = path;
  entry.isDirectory = isDirectory;
  entries_.push_back(std::move(entry));
}

void FileIndex::RemoveEntryLocked(const std::wstring& lowerPath) {
  const std::wstring prefix = lowerPath + L"\\";
  entries_.erase(std::remove_if(entries_.begin(), entries_.end(),
                                [&](const FileEntry& entry) {
                                  return entry.lowerPath == lowerPath ||
                                         entry.lowerPath.rfind(prefix, 0) == 0;
                                }),
                 entries_.end());
}

size_t FileIndex::RebuildSynchronously() {
  EnsureRoots();
  RebuildAndSwap();
  return size();
}

bool FileIndex::LoadCachedSnapshot() {
  EnsureRoots();
  if (roots_.empty()) return false;
  std::vector<FileEntry> cached;
  if (!LoadSnapshot(&cached)) return false;
  std::lock_guard<std::mutex> lock(entriesMutex_);
  entries_ = std::move(cached);
  version_.fetch_add(1);
  return true;
}

std::vector<std::wstring> FileIndex::SamplePaths(size_t limit) const {
  std::vector<std::wstring> paths;
  std::lock_guard<std::mutex> lock(entriesMutex_);
  const size_t count = std::min(limit, entries_.size());
  paths.reserve(count);
  for (size_t i = 0; i < count; ++i) paths.push_back(entries_[i].path);
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
  };
  auto better = [this](const Scored& a, const Scored& b) {
    if (a.score != b.score) return a.score > b.score;
    const FileEntry& ea = entries_[a.index];
    const FileEntry& eb = entries_[b.index];
    const size_t nameA = ea.lowerPath.size() - ea.nameOffset;
    const size_t nameB = eb.lowerPath.size() - eb.nameOffset;
    if (nameA != nameB) return nameA < nameB;
    if (ea.isDirectory != eb.isDirectory) return !ea.isDirectory;  // 同分时文件优先
    if (ea.lowerPath.size() != eb.lowerPath.size()) return ea.lowerPath.size() < eb.lowerPath.size();
    return ea.lowerPath < eb.lowerPath;
  };

  std::vector<Scored> scored;
  scored.reserve(1024);
  const uint64_t required = QueryMask(needle);
  for (size_t i = 0; i < entries_.size(); ++i) {
    const FileEntry& entry = entries_[i];
    const std::wstring_view name(entry.lowerPath.data() + entry.nameOffset,
                                 entry.lowerPath.size() - entry.nameOffset);
    size_t score = 0;
    if (required == 0 || (entry.charMask & required) == required) {
      if (needle.size() < 3) {
        // 1–2 字符只做子串匹配，避免过松的子序列命中。
        if (name.find(needle) != std::wstring_view::npos) {
          score = ScoreLowerText(name, needle);
        }
      } else {
        score = ScoreLowerText(name, needle);
      }
    }
    if (score > 0) {
      score += 40;  // 文件名命中优先
      scored.push_back({score, i, false});
    }
  }

  // 文件名已够填满首屏时不再做昂贵的全路径扫描：路径命中只用于“文件名几乎没结果”的兜底。
  constexpr size_t kNameHitsBeforePath = 6;
  if (scored.size() < kNameHitsBeforePath) {
    std::vector<uint8_t> nameMatched(entries_.size(), 0);
    for (const Scored& item : scored) nameMatched[item.index] = 1;
    constexpr size_t kMaxPathHits = 2000;
    size_t pathHits = 0;
    for (size_t i = 0; i < entries_.size() && pathHits < kMaxPathHits; ++i) {
      if (nameMatched[i]) continue;
      const size_t score = ScorePathOnly(entries_[i].lowerPath, needle);
      if (score > 0) {
        scored.push_back({score, i, true});
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
    results.push_back({entries_[item.index], item.score, item.pathOnly});
  }
  return results;
}
