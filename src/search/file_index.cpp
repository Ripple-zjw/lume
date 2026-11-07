#include "search/file_index.h"

#include <psapi.h>
#include <shlobj.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <cwctype>
#include <functional>
#include <iterator>
#include <string_view>
#include <tuple>

#include "core/log.h"
#include "search/app_entry.h"
#include "search/file_score.h"

namespace {

constexpr size_t kHardMaxEntries = 50000000;
constexpr char kMagic[4] = {'E', 'T', 'F', '4'};
constexpr uint32_t kSnapshotVersion = 4;

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

bool EndsWith(const std::wstring& text, const wchar_t* suffix) {
  const size_t length = wcslen(suffix);
  return text.size() >= length && text.compare(text.size() - length, length, suffix) == 0;
}

// 快照/日志/图标本身在索引根内时，避免写文件触发 watcher 再触发重建的死循环。
bool ShouldIgnoreSnapshotName(const std::wstring& fullPath) {
  const std::wstring lower = ToLower(fullPath);
  if (EndsWith(lower, L"\\files.idx") || EndsWith(lower, L"\\files.idx.tmp") ||
      EndsWith(lower, L"\\apps.idx") || EndsWith(lower, L"\\usage.conf") ||
      EndsWith(lower, L"\\settings.conf")) {
    return true;
  }
  // 日志默认落在 exe 同目录（常驻进程每写一行都会改 mtime），轮转出的 .1 也一并忽略。
  if (EndsWith(lower, L"\\lume.log") || EndsWith(lower, L"\\lume.log.1") ||
      EndsWith(lower, L"\\etools.log")) {
    return true;
  }
  if (lower.find(L"\\lume\\logs\\") != std::wstring::npos ||
      lower.find(L"\\lume\\icons\\") != std::wstring::npos ||
      lower.find(L"\\etools\\logs\\") != std::wstring::npos ||
      lower.find(L"\\etools\\icons\\") != std::wstring::npos ||
      lower.find(L"\\build\\icons\\") != std::wstring::npos) {
    return true;
  }
  return false;
}

// path 是否位于 directory 之下（大小写不敏感，要求分隔符边界）。
bool IsUnderDirectory(const std::wstring& path, const std::wstring& directory) {
  if (path.empty() || directory.empty() || path.size() <= directory.size()) return false;
  if (_wcsnicmp(path.c_str(), directory.c_str(), directory.size()) != 0) return false;
  const wchar_t next = path[directory.size()];
  return next == L'\\' || next == L'/';
}

// 去掉首尾空白与末尾路径分隔符，方便比较/拼接。
std::wstring TrimDirectory(std::wstring value) {
  const size_t first = value.find_first_not_of(L" \t\r\n");
  if (first == std::wstring::npos) return {};
  const size_t last = value.find_last_not_of(L" \t\r\n");
  value = value.substr(first, last - first + 1);
  while (value.size() > 1 && (value.back() == L'\\' || value.back() == L'/')) value.pop_back();
  return value;
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

uint32_t AppendChars(std::vector<wchar_t>* arena, std::wstring_view text) {
  const uint32_t offset = static_cast<uint32_t>(arena->size());
  arena->insert(arena->end(), text.begin(), text.end());
  return offset;
}

// 与快照序列化一致：4 字节长度 + UTF-16 路径 + 1 字节 isDirectory。
uint64_t SerializedEntryBytes(const std::wstring& path) {
  return 4 + static_cast<uint64_t>(path.size()) * sizeof(wchar_t) + 1;
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

void FileIndex::SetMaxIndexBytes(size_t value) {
  maxIndexBytes_ = std::clamp<size_t>(value, 64ull * 1024 * 1024,
                                      256ull * 1024 * 1024 * 1024);
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
  const std::wstring_view name = NameOf(entry);
  if (storage_.directories.empty()) return std::wstring(name);
  return storage_.directories[entry.directory].path + L"\\" + std::wstring(name);
}

std::wstring_view FileIndex::NameOf(const Entry& entry) const {
  if (entry.nameLength == 0) return {};
  return std::wstring_view(storage_.names.data() + entry.nameOffset, entry.nameLength);
}

std::wstring_view FileIndex::LowerNameOf(const Entry& entry) const {
  if (entry.lowerLength == 0) return {};
  return std::wstring_view(storage_.names.data() + entry.lowerOffset, entry.lowerLength);
}

uint32_t FileIndex::LookupDirectory(
    const std::unordered_multimap<uint64_t, uint32_t>& index,
    const std::vector<Directory>& directories, const std::wstring& lowerPath) {
  const uint64_t hash = Fnv1a(lowerPath);
  auto range = index.equal_range(hash);
  for (auto it = range.first; it != range.second; ++it) {
    if (directories[it->second].lowerPath == lowerPath) return it->second;
  }
  return 0xFFFFFFFFu;
}

uint32_t FileIndex::EnsureDirectoryLocked(const std::wstring& path) {
  const std::wstring lower = ToLower(path);
  const uint32_t existing =
      LookupDirectory(storage_.directoryIndex, storage_.directories, lower);
  if (existing != 0xFFFFFFFFu) return existing;

  const size_t slash = path.find_last_of(L'\\');
  if (slash != std::wstring::npos && slash > 0) {
    EnsureDirectoryLocked(path.substr(0, slash));
  }
  Directory directory;
  directory.path = path;
  directory.lowerPath = lower;
  const uint32_t index = static_cast<uint32_t>(storage_.directories.size());
  storage_.directories.push_back(std::move(directory));
  storage_.directoryIndex.emplace(Fnv1a(lower), index);
  return index;
}

void FileIndex::EnumerateRoot(const std::wstring& root, uint64_t byteQuota, RootData* data,
                              char* truncated) {
  if (byteQuota == 0) return;
  const std::wstring rootLower = ToLower(root);
  Directory rootDirectory;
  rootDirectory.path = root;
  rootDirectory.lowerPath = rootLower;
  const uint32_t rootIndex = static_cast<uint32_t>(data->directories.size());
  data->directories.push_back(std::move(rootDirectory));
  data->directoryIndex.emplace(Fnv1a(rootLower), rootIndex);

  std::vector<uint32_t> pending{rootIndex};
  while (!pending.empty()) {
    if (stop_.load()) return;
    if (data->serializedBytes >= byteQuota) {
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
      if (stop_.load()) {
        FindClose(find);
        return;
      }
      if (wcscmp(findData.cFileName, L".") == 0 || wcscmp(findData.cFileName, L"..") == 0) {
        continue;
      }
      if (findData.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;
      const bool isDirectory = (findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
      if (isDirectory && ShouldSkipDirectory(findData.cFileName)) continue;

      const std::wstring name = findData.cFileName;
      const std::wstring lowerName = ToLower(name);
      const std::wstring fullPath = directoryPath + L"\\" + name;
      const std::wstring fullLower = directoryLower + L"\\" + lowerName;
      const uint64_t entryBytes = SerializedEntryBytes(fullPath);
      if (data->serializedBytes + entryBytes > byteQuota) {
        *truncated = 1;
        break;
      }

      Entry entry;
      entry.directory = directoryIndex;
      entry.nameOffset = AppendChars(&data->names, name);
      entry.nameLength = static_cast<uint16_t>(name.size());
      if (name == lowerName) {
        entry.lowerOffset = entry.nameOffset;
        entry.lowerLength = entry.nameLength;
      } else {
        entry.lowerOffset = AppendChars(&data->names, lowerName);
        entry.lowerLength = static_cast<uint16_t>(lowerName.size());
      }
      entry.charMask = NameMask(lowerName);
      entry.pathHash = static_cast<uint32_t>(Fnv1a(fullLower));
      entry.isDirectory = isDirectory;
      data->entries.push_back(std::move(entry));
      data->serializedBytes += entryBytes;

      if (isDirectory) {
        const uint32_t existing =
            LookupDirectory(data->directoryIndex, data->directories, fullLower);
        if (existing == 0xFFFFFFFFu) {
          Directory child;
          child.path = fullPath;
          child.lowerPath = fullLower;
          const uint32_t childIndex = static_cast<uint32_t>(data->directories.size());
          data->directories.push_back(std::move(child));
          data->directoryIndex.emplace(Fnv1a(fullLower), childIndex);
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
    LogWarn(L"文件索引: 没有可用根目录");
    return false;
  }

  const ULONGLONG loadStart = GetTickCount64();
  const bool loadedSnapshot = LoadSnapshot();
  saveSnapshotAfterRebuild_.store(!loadedSnapshot);
  if (loadedSnapshot) {
    Log(L"文件索引: 快照加载完成 (%llu ms)", GetTickCount64() - loadStart);
  }

  stop_.store(false);
  rebuildRequested_.store(true);
  for (const std::wstring& root : roots_) {
    Log(L"文件索引根: %ls", root.c_str());
  }
  worker_ = std::thread([this]() { WorkerLoop(); });
  for (const std::wstring& root : roots_) {
    watchers_.emplace_back([this, root]() { WatchLoop(root); });
  }
  Log(L"文件索引: 根目录 %zu 个, 监听已启动（后台重建中）", roots_.size());
  return true;
}

void FileIndex::SetIndexDirectory(std::wstring directory) {
  directory = TrimDirectory(std::move(directory));
  indexDir_ = directory.empty() ? std::wstring() : ExpandEnv(directory);
}

std::wstring FileIndex::DefaultIndexDirectory() {
  const std::wstring base = GetEnv(L"LOCALAPPDATA");
  if (!base.empty()) {
    const std::wstring directory = base + L"\\Lume";
    if (EnsureDirectory(directory)) return directory;
  }
  return ExeDirectory();
}

std::wstring FileIndex::indexDirectory() const {
  return indexDir_.empty() ? DefaultIndexDirectory() : indexDir_;
}

std::wstring FileIndex::snapshotPath() const { return indexDirectory() + L"\\files.idx"; }

uint64_t FileIndex::snapshotBytes() const {
  uint64_t bytes = 0;
  ULARGE_INTEGER newest{};
  for (const std::wstring& path : SnapshotPaths()) {
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data)) continue;
    ULARGE_INTEGER modified{};
    modified.LowPart = data.ftLastWriteTime.dwLowDateTime;
    modified.HighPart = data.ftLastWriteTime.dwHighDateTime;
    if (modified.QuadPart < newest.QuadPart) continue;
    newest = modified;
    bytes = (static_cast<uint64_t>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
  }
  return bytes;
}

bool FileIndex::snapshotWritable() const { return !SnapshotPaths().empty(); }

void FileIndex::ClearIndex() {
  size_t removed = 0;
  for (const std::wstring& path : SnapshotPaths()) {
    if (DeleteFileW(path.c_str())) {
      ++removed;
      Log(L"文件索引: 已删除快照 %ls", path.c_str());
    }
  }
  {
    std::lock_guard<std::mutex> lock(entriesMutex_);
    storage_ = Storage{};
    version_.fetch_add(1);
  }
  saveSnapshotAfterRebuild_.store(true);
  // 清除时正好有重建在跑：丢弃那次结果，否则索引会“自己回来”。
  if (rebuilding_.load()) discardBuild_.store(true);
  Log(L"文件索引: 已清除（未重建），删除快照 %zu 个", removed);
}

void FileIndex::RebuildIndex() {
  discardBuild_.store(false);
  // 手动重建不受 60s 冷却限制（lastRebuildMs_ 同时是冷却时间戳）。
  lastRebuildMs_.store(0);
  saveSnapshotAfterRebuild_.store(true);
  rebuildRequested_.store(true);
  workerCv_.notify_all();
  Log(L"文件索引: 手动重建已请求");
}

void FileIndex::Stop() {
  if (stop_.exchange(true)) return;
  Log(L"文件索引: 停止中");
  {
    std::lock_guard<std::mutex> lock(watchMutex_);
    for (HANDLE handle : watchHandles_) {
      if (handle != INVALID_HANDLE_VALUE) CancelIoEx(handle, nullptr);
    }
  }
  workerCv_.notify_all();
  Log(L"文件索引: 等待 worker");
  if (worker_.joinable()) worker_.join();
  Log(L"文件索引: 等待 watcher");
  for (std::thread& thread : watchers_) {
    if (thread.joinable()) thread.join();
  }
  watchers_.clear();
  {
    std::lock_guard<std::mutex> lock(watchMutex_);
    watchHandles_.clear();
  }
  Log(L"文件索引: 已停止");
}

void FileIndex::RebuildAndSwap() {
  const size_t rootCount = roots_.size();
  const uint64_t perRootBytes = rootCount ? maxIndexBytes_ / rootCount : 0;
  Log(L"文件索引: 根目录 %zu 个, 每根预算 %.0f MB", rootCount,
      static_cast<double>(perRootBytes) / (1024.0 * 1024.0));

  std::vector<RootData> partial(roots_.size());
  std::vector<char> truncated(roots_.size(), 0);
  std::vector<std::thread> threads;
  for (size_t i = 0; i < roots_.size(); ++i) {
    threads.emplace_back([&, i]() {
      EnumerateRoot(roots_[i], perRootBytes, &partial[i], &truncated[i]);
    });
  }
  for (std::thread& thread : threads) thread.join();
  if (stop_.load()) {
    LogWarn(L"文件索引: 重建已取消");
    return;
  }
  for (size_t i = 0; i < roots_.size(); ++i) {
    if (truncated[i]) {
      LogWarn(L"文件索引: %ls 达到预算 %.0f MB，已截断", roots_[i].c_str(),
              static_cast<double>(perRootBytes) / (1024.0 * 1024.0));
    }
  }

  Storage fresh;
  for (RootData& data : partial) {
    const uint32_t base = static_cast<uint32_t>(fresh.directories.size());
    for (Directory& directory : data.directories) {
      fresh.directories.push_back(std::move(directory));
    }
    const uint32_t nameBase = static_cast<uint32_t>(fresh.names.size());
    if (!data.names.empty()) {
      fresh.names.insert(fresh.names.end(), data.names.begin(), data.names.end());
    }
    for (Entry& entry : data.entries) {
      entry.directory += base;
      entry.nameOffset += nameBase;
      entry.lowerOffset += nameBase;
      fresh.entries.push_back(std::move(entry));
    }
    fresh.serializedBytes += data.serializedBytes;
    // 逐个根释放枚举缓冲，降低合并阶段峰值。
    std::vector<Directory>().swap(data.directories);
    std::vector<Entry>().swap(data.entries);
    std::vector<wchar_t>().swap(data.names);
    data.directoryIndex.clear();
    data.directoryIndex.rehash(0);
  }
  fresh.directoryIndex.reserve(fresh.directories.size());
  for (size_t i = 0; i < fresh.directories.size(); ++i) {
    fresh.directoryIndex.emplace(Fnv1a(fresh.directories[i].lowerPath),
                                 static_cast<uint32_t>(i));
  }
  // 清除索引时点下去的重建：结果丢弃，索引保持空，等用户显式重建。
  if (discardBuild_.exchange(false)) {
    LogWarn(L"文件索引: 重建结果已丢弃（清除索引）");
    return;
  }
  {
    std::lock_guard<std::mutex> lock(entriesMutex_);
    storage_ = std::move(fresh);
    version_.fetch_add(1);
  }
  // 释放并行枚举留下的 RootData 容量，再统计常驻内存。
  partial.clear();
  partial.shrink_to_fit();
  if (persistSnapshot_ && saveSnapshotAfterRebuild_.exchange(false)) SaveSnapshot();

  PROCESS_MEMORY_COUNTERS memory{};
  if (K32GetProcessMemoryInfo(GetCurrentProcess(), &memory, sizeof(memory))) {
    LogDebug(L"文件索引内存: 当前 %.0f MB, 峰值 %.0f MB",
             static_cast<double>(memory.WorkingSetSize) / (1024.0 * 1024.0),
             static_cast<double>(memory.PeakWorkingSetSize) / (1024.0 * 1024.0));
  }
  {
    size_t nameChars = 0;
    size_t lowerChars = 0;
    size_t sameNames = 0;
    size_t dirChars = 0;
    size_t dirLowerChars = 0;
    for (const Entry& entry : storage_.entries) {
      nameChars += entry.nameLength;
      lowerChars += entry.lowerLength;
      if (entry.nameOffset == entry.lowerOffset) ++sameNames;
    }
    for (const Directory& directory : storage_.directories) {
      dirChars += directory.path.size();
      dirLowerChars += directory.lowerPath.size();
    }
    LogDebug(L"索引统计: 条目 %zu, 目录 %zu, 名称字符 %zu, 小写名 %zu, 同形 %zu, "
             L"目录路径 %zu, 小写目录 %zu, 预算累计(按完整路径保守估算) %.1f MB, "
             L"sizeof(Entry)=%zu, sizeof(Dir)=%zu",
             storage_.entries.size(), storage_.directories.size(), nameChars, lowerChars, sameNames,
             dirChars, dirLowerChars,
             static_cast<double>(storage_.serializedBytes) / (1024.0 * 1024.0), sizeof(Entry),
             sizeof(Directory));
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
      const uint64_t last = lastRebuildMs_.load();
      const uint64_t now = GetTickCount64();
      // 大根目录（尤其 C:）系统噪声会不断触发目录重建，做 60s 冷却避免 CPU/磁盘抖动。
      if (last != 0 && now - last < 60000) {
        rebuildRequested_.store(true);
        std::unique_lock<std::mutex> lock(workerMutex_);
        workerCv_.wait_for(lock, std::chrono::milliseconds(60000 - (now - last)),
                           [this]() { return stop_.load(); });
        continue;
      }
      const ULONGLONG start = GetTickCount64();
      rebuilding_.store(true);
      RebuildAndSwap();
      rebuilding_.store(false);
      lastRebuildMs_.store(GetTickCount64());
      lastRebuildDurationMs_.store(GetTickCount64() - start);
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
    LogError(L"文件监听打开失败: %ls (%lu)", root.c_str(), GetLastError());
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
    if (ShouldIgnoreWatchPath(full)) {
      if (info->NextEntryOffset == 0) break;
      offset += info->NextEntryOffset;
      continue;
    }

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
  if (changed) LogDebug(L"文件索引: 增量更新");
}

void FileIndex::AddEntryLocked(const std::wstring& path, bool isDirectory) {
  const uint64_t entryBytes = SerializedEntryBytes(path);
  if (storage_.serializedBytes + entryBytes > maxIndexBytes_) return;
  const size_t slash = path.find_last_of(L'\\');
  if (slash == std::wstring::npos || slash + 1 >= path.size()) return;
  const std::wstring parentPath = path.substr(0, slash);
  const std::wstring name = path.substr(slash + 1);
  const std::wstring lowerName = ToLower(name);
  const std::wstring lowerFull = ToLower(parentPath) + L"\\" + lowerName;
  const uint32_t hash = static_cast<uint32_t>(Fnv1a(lowerFull));
  for (const Entry& existing : storage_.entries) {
    if (existing.pathHash == hash && FullPathOf(existing) == path) return;
  }
  const uint32_t directory = EnsureDirectoryLocked(parentPath);
  Entry entry;
  entry.directory = directory;
  entry.nameOffset = AppendChars(&storage_.names, name);
  entry.nameLength = static_cast<uint16_t>(name.size());
  if (name == lowerName) {
    entry.lowerOffset = entry.nameOffset;
    entry.lowerLength = entry.nameLength;
  } else {
    entry.lowerOffset = AppendChars(&storage_.names, lowerName);
    entry.lowerLength = static_cast<uint16_t>(lowerName.size());
  }
  entry.charMask = NameMask(lowerName);
  entry.pathHash = hash;
  entry.isDirectory = isDirectory;
  storage_.entries.push_back(std::move(entry));
  storage_.serializedBytes += entryBytes;
}

void FileIndex::RemoveEntryLocked(const std::wstring& path) {
  const std::wstring lowerFull = ToLower(path);
  const uint32_t hash = static_cast<uint32_t>(Fnv1a(lowerFull));
  for (size_t i = 0; i < storage_.entries.size(); ++i) {
    const Entry& entry = storage_.entries[i];
    if (entry.pathHash != hash) continue;
    if (_wcsicmp(FullPathOf(entry).c_str(), path.c_str()) != 0) continue;
    if (entry.isDirectory) {
      // 目录内容未知，交给后台重建。
      rebuildRequested_.store(true);
      return;
    }
    const uint64_t bytes = SerializedEntryBytes(path);
    storage_.entries.erase(storage_.entries.begin() + static_cast<std::ptrdiff_t>(i));
    storage_.serializedBytes =
        storage_.serializedBytes > bytes ? storage_.serializedBytes - bytes : 0;
    return;
  }
}

std::vector<std::wstring> FileIndex::SnapshotPaths() const {
  std::vector<std::wstring> paths;
  // 用户指定了快照目录时只认它：不静默回退到默认目录，避免读到旧快照造成误解。
  if (!indexDir_.empty()) {
    if (EnsureDirectory(indexDir_)) {
      paths.push_back(indexDir_ + L"\\files.idx");
    } else {
      LogError(L"文件索引: 索引目录不可用 %ls", indexDir_.c_str());
    }
    return paths;
  }
  const std::wstring base = GetEnv(L"LOCALAPPDATA");
  if (!base.empty()) {
    const std::wstring directory = base + L"\\Lume";
    if (EnsureDirectory(directory)) paths.push_back(directory + L"\\files.idx");
  }
  const std::wstring fallbackDir = ExeDirectory();
  if (EnsureDirectory(fallbackDir)) {
    const std::wstring fallback = fallbackDir + L"\\files.idx";
    if (std::find(paths.begin(), paths.end(), fallback) == paths.end()) {
      paths.push_back(fallback);
    }
  }
  // 旧 eTools 快照只用于首次迁移读取；写入仍优先 Lume。
  if (!base.empty()) paths.push_back(base + L"\\eTools\\files.idx");
  return paths;
}

bool FileIndex::ShouldIgnoreWatchPath(const std::wstring& fullPath) const {
  if (ShouldIgnoreSnapshotName(fullPath)) return true;
  // 自定义索引目录常见于监听根内部：该目录下的写入同样不能触发重建。
  return IsUnderDirectory(fullPath, indexDirectory());
}

bool FileIndex::LoadSnapshot() {
  std::vector<std::wstring> candidates = SnapshotPaths();
  auto modifiedTime = [](const std::wstring& path) {
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data)) return ULARGE_INTEGER{};
    ULARGE_INTEGER value{};
    value.LowPart = data.ftLastWriteTime.dwLowDateTime;
    value.HighPart = data.ftLastWriteTime.dwHighDateTime;
    return value;
  };
  std::sort(candidates.begin(), candidates.end(),
            [&](const std::wstring& a, const std::wstring& b) {
              return modifiedTime(a).QuadPart > modifiedTime(b).QuadPart;
            });

  for (const std::wstring& path : candidates) {
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

    uint32_t dirCount = 0;
    if (!ReadPod(data, &offset, &dirCount) || dirCount > 5000000) continue;
    struct DirRecord {
      uint32_t parent = 0xFFFFFFFFu;
      std::wstring component;
    };
    std::vector<DirRecord> dirRecords(dirCount);
    bool valid = true;
    for (uint32_t i = 0; i < dirCount; ++i) {
      if (!ReadPod(data, &offset, &dirRecords[i].parent) ||
          !ReadWide(data, &offset, &dirRecords[i].component)) {
        valid = false;
        break;
      }
    }
    if (!valid) continue;

    uint32_t nameCount = 0;
    if (!ReadPod(data, &offset, &nameCount) || nameCount > 200000000) continue;
    const size_t nameBytes = static_cast<size_t>(nameCount) * sizeof(wchar_t);
    if (offset + nameBytes > data.size()) continue;
    std::vector<wchar_t> names(nameCount);
    if (nameBytes > 0) std::memcpy(names.data(), data.data() + offset, nameBytes);
    offset += nameBytes;

    uint32_t count = 0;
    if (!ReadPod(data, &offset, &count) || count > kHardMaxEntries) continue;
    Storage fresh;
    fresh.names = std::move(names);
    for (uint32_t i = 0; i < count; ++i) {
      Entry entry;
      uint8_t isDirectory = 0;
      if (!ReadPod(data, &offset, &entry.directory) ||
          !ReadPod(data, &offset, &entry.nameOffset) ||
          !ReadPod(data, &offset, &entry.lowerOffset) ||
          !ReadPod(data, &offset, &entry.nameLength) ||
          !ReadPod(data, &offset, &entry.lowerLength) ||
          !ReadPod(data, &offset, &entry.charMask) ||
          !ReadPod(data, &offset, &entry.pathHash) ||
          !ReadPod(data, &offset, &isDirectory)) {
        valid = false;
        break;
      }
      if (entry.directory >= dirCount ||
          static_cast<size_t>(entry.nameOffset) + entry.nameLength > fresh.names.size() ||
          static_cast<size_t>(entry.lowerOffset) + entry.lowerLength > fresh.names.size()) {
        valid = false;
        break;
      }
      entry.isDirectory = isDirectory != 0;
      fresh.entries.push_back(std::move(entry));
    }
    if (!valid || fresh.entries.empty()) continue;

    std::vector<Directory> directories(dirCount);
    std::vector<uint8_t> state(dirCount, 0);
    std::function<bool(uint32_t)> build = [&](uint32_t id) -> bool {
      if (id >= dirCount) return false;
      if (state[id] == 2) return true;
      if (state[id] == 1) return false;
      state[id] = 1;
      const DirRecord& record = dirRecords[id];
      if (record.parent == 0xFFFFFFFFu || record.parent >= dirCount) {
        directories[id].path = record.component;
      } else {
        if (!build(record.parent)) return false;
        directories[id].path = directories[record.parent].path + L"\\" + record.component;
      }
      directories[id].lowerPath = ToLower(directories[id].path);
      state[id] = 2;
      return true;
    };
    for (uint32_t i = 0; i < dirCount; ++i) {
      if (!build(i)) {
        valid = false;
        break;
      }
    }
    if (!valid) continue;

    fresh.directories = std::move(directories);
    fresh.directoryIndex.reserve(fresh.directories.size());
    for (size_t i = 0; i < fresh.directories.size(); ++i) {
      fresh.directoryIndex.emplace(Fnv1a(fresh.directories[i].lowerPath),
                                   static_cast<uint32_t>(i));
    }
    for (const Entry& entry : fresh.entries) {
      const std::wstring fullPath =
          fresh.directories[entry.directory].path + L"\\" +
          std::wstring(fresh.names.data() + entry.nameOffset, entry.nameLength);
      fresh.serializedBytes += SerializedEntryBytes(fullPath);
    }

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
  for (const std::wstring& path : SnapshotPaths()) {
    const std::wstring temp = path + L".tmp";
    HANDLE file = CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) continue;

    std::string buffer;
    buffer.reserve(1 << 20);
    size_t writtenTotal = 0;
    bool ok = true;
    auto flush = [&]() {
      if (buffer.empty()) return true;
      const DWORD size = static_cast<DWORD>(buffer.size());
      DWORD written = 0;
      const BOOL result = WriteFile(file, buffer.data(), size, &written, nullptr);
      buffer.clear();
      writtenTotal += written;
      return result != 0 && written == size;
    };
    auto pod = [&](auto value) {
      buffer.append(reinterpret_cast<const char*>(&value), sizeof(value));
      if (buffer.size() >= (1 << 20)) ok = ok && flush();
    };
    auto wide = [&](const std::wstring& value) {
      pod(static_cast<uint32_t>(value.size()));
      buffer.append(reinterpret_cast<const char*>(value.data()), value.size() * sizeof(wchar_t));
      if (buffer.size() >= (1 << 20)) ok = ok && flush();
    };
    auto raw = [&](const void* data, size_t bytes) {
      const char* cursor = static_cast<const char*>(data);
      size_t remaining = bytes;
      while (remaining > 0) {
        const size_t chunk = std::min<size_t>(remaining, 1 << 20);
        buffer.append(cursor, chunk);
        cursor += chunk;
        remaining -= chunk;
        if (buffer.size() >= (1 << 20)) ok = ok && flush();
      }
    };

    buffer.append(kMagic, sizeof(kMagic));
    pod(kSnapshotVersion);
    pod(static_cast<uint32_t>(roots_.size()));
    for (const std::wstring& root : roots_) wide(root);

    pod(static_cast<uint32_t>(storage_.directories.size()));
    for (const Directory& directory : storage_.directories) {
      bool isRoot = false;
      for (const std::wstring& root : roots_) {
        if (_wcsicmp(directory.path.c_str(), root.c_str()) == 0) {
          isRoot = true;
          break;
        }
      }
      uint32_t parent = 0xFFFFFFFFu;
      std::wstring component = directory.path;
      if (!isRoot) {
        const size_t slash = directory.path.find_last_of(L'\\');
        if (slash != std::wstring::npos && slash > 0) {
          const std::wstring parentPath = directory.path.substr(0, slash);
          parent = LookupDirectory(storage_.directoryIndex, storage_.directories,
                                   ToLower(parentPath));
          if (parent != 0xFFFFFFFFu) component = directory.path.substr(slash + 1);
        }
      }
      pod(parent);
      wide(component);
      if (!ok) break;
    }
    if (!ok) {
      CloseHandle(file);
      DeleteFileW(temp.c_str());
      return false;
    }

    pod(static_cast<uint32_t>(storage_.names.size()));
    raw(storage_.names.data(), storage_.names.size() * sizeof(wchar_t));
    pod(static_cast<uint32_t>(storage_.entries.size()));
    size_t index = 0;
    for (const Entry& entry : storage_.entries) {
      if ((++index & 4095) == 0 && stop_.load()) {
        ok = false;
        break;
      }
      pod(entry.directory);
      pod(entry.nameOffset);
      pod(entry.lowerOffset);
      pod(entry.nameLength);
      pod(entry.lowerLength);
      pod(entry.charMask);
      pod(entry.pathHash);
      pod(static_cast<uint8_t>(entry.isDirectory ? 1 : 0));
    }
    ok = ok && flush();
    CloseHandle(file);
    if (!ok) {
      DeleteFileW(temp.c_str());
      LogError(L"文件索引快照写入中断: %ls", temp.c_str());
      return false;
    }
    if (!MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) {
      DeleteFileW(temp.c_str());
      LogError(L"文件索引快照替换失败: %lu", GetLastError());
      continue;
    }
    Log(L"文件索引快照已写入: %ls (%zu 条, %.1f MB)", path.c_str(), storage_.entries.size(),
        static_cast<double>(writtenTotal) / (1024.0 * 1024.0));
    return true;
  }
  LogError(L"文件索引快照写入失败: %lu", GetLastError());
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
    paths.push_back(storage_.directories[entry.directory].path + L"\\" +
                    std::wstring(NameOf(entry)));
  }
  return paths;
}

std::vector<FileHit> FileIndex::Search(const std::wstring& query, size_t limit,
                                       const FilterGroup* filter) const {
  std::vector<FileHit> results;
  if (query.empty()) return results;
  const std::wstring needle = ToLower(query);
  const std::vector<std::wstring> words = SplitQueryWords(needle);
  const bool multiWord = words.size() > 1;
  std::vector<uint64_t> wordMasks;
  if (multiWord) {
    wordMasks.reserve(words.size());
    for (const std::wstring& word : words) wordMasks.push_back(QueryMask(word));
  }
  std::lock_guard<std::mutex> lock(entriesMutex_);

  struct Scored {
    size_t score;
    size_t index;
    bool pathOnly;
    uint8_t quality;
  };
  auto pathOf = [this](const Entry& entry) {
    return storage_.directories[entry.directory].path + L"\\" + std::wstring(NameOf(entry));
  };
  auto better = [this](const Scored& a, const Scored& b) {
    if (a.quality != b.quality) return a.quality > b.quality;
    if (a.score != b.score) return a.score > b.score;
    const Entry& ea = storage_.entries[a.index];
    const Entry& eb = storage_.entries[b.index];
    if (ea.lowerLength != eb.lowerLength) return ea.lowerLength < eb.lowerLength;
    if (ea.isDirectory != eb.isDirectory) return !ea.isDirectory;
    const size_t pathA = storage_.directories[ea.directory].lowerPath.size() + 1 +
                         ea.lowerLength;
    const size_t pathB = storage_.directories[eb.directory].lowerPath.size() + 1 +
                         eb.lowerLength;
    if (pathA != pathB) return pathA < pathB;
    const std::wstring_view nameA = LowerNameOf(ea);
    const std::wstring_view nameB = LowerNameOf(eb);
    return std::tie(storage_.directories[ea.directory].lowerPath, nameA) <
           std::tie(storage_.directories[eb.directory].lowerPath, nameB);
  };

  std::vector<Scored> scored;
  scored.reserve(1024);
  const uint64_t required = multiWord ? 0 : QueryMask(needle);
  for (size_t i = 0; i < storage_.entries.size(); ++i) {
    const Entry& entry = storage_.entries[i];
    if (multiWord) {
      // 多词：每个词的关键字符都必须出现在文件名里（顺序不限），作为廉价预筛。
      bool maskOk = true;
      for (const uint64_t mask : wordMasks) {
        if (mask != 0 && (entry.charMask & mask) != mask) {
          maskOk = false;
          break;
        }
      }
      if (!maskOk) continue;
    } else if (required != 0 && (entry.charMask & required) != required) {
      continue;
    }
    const std::wstring_view name = LowerNameOf(entry);
    size_t nameScoreValue = 0;
    uint8_t nameQuality = 0;
    if (multiWord) {
      const FileNameScore nameScore = ScoreNameForQuery(name, needle);
      nameScoreValue = nameScore.score;
      nameQuality = nameScore.quality;
    } else if (needle.size() < 3) {
      // 短查询只做子串，内联 find 保持热路径速度；单字符额外要求前缀或词首。
      const size_t found = name.find(needle);
      if (found == std::wstring_view::npos) continue;
      if (needle.size() == 1 && found != 0 && !IsPathBoundary(name[found - 1])) continue;
      nameScoreValue = ScoreLowerText(name, needle);
      nameQuality = NameQuality(name, needle);
    } else {
      // 单词查询是热路径：不分配分词数组，直接打分 + 判定质量。
      nameScoreValue = ScoreLowerText(name, needle);
      if (nameScoreValue > 0) nameQuality = NameQuality(name, needle);
    }
    if (nameScoreValue == 0) continue;
    // 筛选只在命中后判断：不影响全表扫描的字符位图预筛与打分开销。
    if (filter && !FilterMatchesPath(*filter, name, entry.isDirectory)) continue;
    scored.push_back({nameScoreValue + 40, i, false, nameQuality});
  }

  // 文件名命中很少时补路径命中（优先级最低）。路径扫描开销明显，只在候选不足时做，
  // 这样常见的文件名查询不会多付一次全表路径扫描。
  constexpr size_t kPathFallbackBelow = 5;
  constexpr size_t kMaxPathHits = 500;
  if (needle.size() >= 3 && scored.size() < kPathFallbackBelow) {
    std::vector<uint8_t> nameMatched;
    if (!scored.empty()) {
      nameMatched.assign(storage_.entries.size(), 0);
      for (const Scored& item : scored) nameMatched[item.index] = 1;
    }
    size_t pathHits = 0;
    for (size_t i = 0; i < storage_.entries.size() && pathHits < kMaxPathHits; ++i) {
      if (!nameMatched.empty() && nameMatched[i]) continue;
      const Entry& entry = storage_.entries[i];
      const size_t score = ScorePathOnly(storage_.directories[entry.directory].lowerPath, needle);
      if (score == 0) continue;
      if (filter && !FilterMatchesPath(*filter, LowerNameOf(entry), entry.isDirectory)) continue;
      scored.push_back({score, i, true, 0});
      ++pathHits;
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

void FileIndex::LoadPathsForTest(const std::vector<std::wstring>& paths) {
  std::lock_guard<std::mutex> lock(entriesMutex_);
  storage_ = Storage{};
  for (const std::wstring& raw : paths) {
    if (raw.empty()) continue;
    const bool isDirectory = raw.back() == L'\\' || raw.back() == L'/';
    std::wstring path = raw;
    while (!path.empty() && (path.back() == L'\\' || path.back() == L'/')) path.pop_back();
    const size_t slash = path.find_last_of(L'\\');
    if (slash == std::wstring::npos || slash + 1 >= path.size()) continue;
    AddEntryLocked(path, isDirectory);
  }
  version_.fetch_add(1);
}

size_t FileIndex::RebuildSynchronously() {
  EnsureRoots();
  if (roots_.empty()) return 0;
  RebuildAndSwap();
  return size();
}

void FileIndex::LoadSyntheticForBenchmark(size_t count, uint32_t seed) {
  // 名字按真实文件名的分布造：1–3 个词 + 可选数字、约 6% 无扩展名、少量中文，
  // 扩展名分布让默认筛选组（应用程序/图片/文档/音频/视频/压缩包）都既有命中也有落空。
  static const wchar_t* kWords[] = {
      L"report",  L"photo",    L"setup",   L"backup",  L"project", L"notes",   L"invoice",
      L"draft",   L"final",    L"config",  L"install", L"driver",  L"update",  L"sample",
      L"index",   L"cache",    L"avatar",  L"banner",  L"screen",  L"clip",    L"record",
      L"export",  L"import",   L"result",  L"summary", L"manual",  L"guide",   L"readme",
      L"license", L"changelog", L"patch", L"debug",   L"trace",   L"data",    L"asset",
      L"theme",   L"icon",     L"video",   L"music",   L"podcast", L"ticket",  L"todo",
      L"报告",    L"照片",      L"文档",    L"设置",    L"备份",    L"项目",
  };
  static const wchar_t* kExtensions[] = {
      L"exe",  L"lnk",  L"bat",  L"cmd",  L"msi",  L"appref-ms", L"ps1",  L"png",  L"jpg",
      L"jpeg", L"gif",  L"bmp",  L"webp", L"svg",  L"ico",       L"tif",  L"tiff", L"heic",
      L"pdf",  L"docx", L"xlsx", L"pptx", L"txt",  L"md",        L"rtf",  L"csv",  L"mp3",
      L"flac", L"wav",  L"m4a",  L"mp4",  L"mkv",  L"avi",       L"mov",  L"webm", L"zip",
      L"7z",   L"rar",  L"tar",  L"iso",  L"log",  L"json",      L"dll",  L"sys",  L"ttf",
  };
  constexpr size_t kWordCount = ARRAYSIZE(kWords);
  constexpr size_t kExtensionCount = ARRAYSIZE(kExtensions);

  std::lock_guard<std::mutex> lock(entriesMutex_);
  storage_ = Storage{};
  if (count == 0) return;

  uint32_t state = seed ? seed : 1u;
  auto next = [&state]() {
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    return state;
  };
  auto pick = [&next](size_t range) { return static_cast<size_t>(next() % range); };

  // 目录表：根 + 一层（每个目录放 ~24 个条目，接近真实目录的平均文件数）。
  std::vector<uint32_t> directories;
  const size_t dirCount = std::clamp<size_t>(count / 24 + 1, 1, 40000);
  directories.reserve(dirCount);
  for (size_t i = 0; i < dirCount; ++i) {
    wchar_t path[96] = {};
    swprintf(path, ARRAYSIZE(path), L"D:\\bench\\area%02zu\\project%05zu", i % 32, i);
    Directory directory;
    directory.path = path;
    directory.lowerPath = ToLower(directory.path);
    const uint32_t index = static_cast<uint32_t>(storage_.directories.size());
    storage_.directoryIndex.emplace(Fnv1a(directory.lowerPath), index);
    storage_.directories.push_back(std::move(directory));
    directories.push_back(index);
  }

  storage_.entries.reserve(count);
  // 名字 arena 先按平均 ~30 字符预留，避免反复扩容影响生成耗时（不计入搜索计时）。
  storage_.names.reserve(count * 34);
  uint64_t serializedBytes = 0;
  for (size_t i = 0; i < count; ++i) {
    std::wstring name;
    const size_t shape = pick(100);
    const wchar_t* first = kWords[pick(kWordCount)];
    name += first;
    if (shape < 28) {
      name += L'_';
      name += kWords[pick(kWordCount)];
    } else if (shape < 50) {
      name += L'-';
      name += kWords[pick(kWordCount)];
    } else if (shape < 68) {
      wchar_t digits[16] = {};
      swprintf(digits, ARRAYSIZE(digits), L"%u", static_cast<unsigned>(next() % 10000));
      name += digits;
    } else if (shape < 76) {
      wchar_t digits[16] = {};
      swprintf(digits, ARRAYSIZE(digits), L"%u", static_cast<unsigned>(2020 + (next() % 7)));
      name += L'_';
      name += digits;
    }
    const bool isDirectory = pick(100) < 4;
    if (!isDirectory && pick(100) >= 6) {
      name += L'.';
      name += kExtensions[pick(kExtensionCount)];
    }
    if (name.size() > 240) name.resize(240);

    const std::wstring lower = ToLower(name);
    Entry entry;
    entry.directory = directories[pick(dirCount)];
    entry.nameOffset = AppendChars(&storage_.names, name);
    entry.nameLength = static_cast<uint16_t>(name.size());
    if (name == lower) {
      entry.lowerOffset = entry.nameOffset;  // 全小写时与原名共用 arena 内容
      entry.lowerLength = entry.nameLength;
    } else {
      entry.lowerOffset = AppendChars(&storage_.names, lower);
      entry.lowerLength = static_cast<uint16_t>(lower.size());
    }
    entry.charMask = NameMask(lower);
    entry.pathHash = static_cast<uint32_t>(Fnv1a(lower));
    entry.isDirectory = isDirectory;
    storage_.entries.push_back(entry);

    serializedBytes +=
        SerializedEntryBytes(storage_.directories[entry.directory].path + L"\\" + name);
  }
  storage_.serializedBytes = serializedBytes;
}
