#pragma once

#include <windows.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

struct FileHit {
  std::wstring path;
  bool isDirectory = false;
  size_t score = 0;
  bool pathOnly = false;  // true = 仅路径命中（文件名没命中）
  // 0 = 模糊/仅路径，1 = 子串，2 = 前缀，3 = 忽略扩展名一致，4 = 字面完全一致
  uint8_t quality = 0;
};

// 文件/文件夹索引：枚举配置根目录、快照缓存、目录 watcher 增量更新、查询。
// 内存结构按“目录表 + 文件名”压缩：完整路径只按目录存一份，条目只存文件名。
class FileIndex {
 public:
  FileIndex() = default;
  ~FileIndex();
  FileIndex(const FileIndex&) = delete;
  FileIndex& operator=(const FileIndex&) = delete;

  // 删除旧快照、读快照（若根目录匹配）并启动后台重建 + watcher。
  bool Start();
  void Stop();

  // 手动清除内存索引与快照，并立即触发后台重建（设置窗按钮）。
  void ClearAndRebuild();

  // 测试/CLI 指定根目录（需在 Start 前调用；空表示使用默认桌面/文档/下载）。
  void SetRoots(std::vector<std::wstring> roots);

  // 索引快照允许的最大字节数（默认 512MB，最大 256GB）。
  void SetMaxIndexBytes(size_t value);
  // 是否把重建结果写回 files.idx（常驻模式启动即重建，不需要落盘）。
  void SetPersistSnapshot(bool value) { persistSnapshot_ = value; }

  std::vector<std::wstring> roots() const;
  std::vector<FileHit> Search(const std::wstring& query, size_t limit) const;
  size_t size() const;
  uint64_t version() const { return version_.load(); }

  // 加载快照但不启动后台线程（截图/诊断用）。
  bool LoadCachedSnapshot();

  // 诊断用：返回前 limit 条路径。
  std::vector<std::wstring> SamplePaths(size_t limit) const;

  // 测试/CLI 用：同步重建（耗时），返回条目数。
  size_t RebuildSynchronously();

 private:
  struct Directory {
    std::wstring path;
    std::wstring lowerPath;
  };
  struct Entry {
    uint32_t directory = 0;  // 父目录在 directories_ 中的下标
    uint32_t nameOffset = 0;
    uint32_t lowerOffset = 0;  // 全小写名称本来就和原名相同时复用 nameOffset
    uint16_t nameLength = 0;
    uint16_t lowerLength = 0;
    uint64_t charMask = 0;
    uint32_t pathHash = 0;  // 小写完整路径 FNV1a 低 32 位，供删除/去重快速筛选
    bool isDirectory = false;
  };
  struct Storage {
    std::vector<Directory> directories;
    std::vector<Entry> entries;
    std::vector<wchar_t> names;  // 原名/小写名共用的连续字符 arena
    std::unordered_multimap<uint64_t, uint32_t> directoryIndex;  // FNV(lower dir) -> index
    uint64_t serializedBytes = 0;
  };
  struct RootData {
    std::vector<Directory> directories;
    std::vector<Entry> entries;
    std::vector<wchar_t> names;
    std::unordered_multimap<uint64_t, uint32_t> directoryIndex;
    uint64_t serializedBytes = 0;
  };

  void EnsureRoots();
  void EnumerateRoot(const std::wstring& root, uint64_t byteQuota, RootData* data,
                     char* truncated);
  void WorkerLoop();
  void RebuildAndSwap();
  void WatchLoop(std::wstring root);
  void HandleNotifications(const std::wstring& root, const char* buffer, DWORD bytes);
  void AddEntryLocked(const std::wstring& path, bool isDirectory);
  void RemoveEntryLocked(const std::wstring& path);
  uint32_t EnsureDirectoryLocked(const std::wstring& path);
  std::wstring FullPathOf(const Entry& entry) const;
  std::wstring_view NameOf(const Entry& entry) const;
  std::wstring_view LowerNameOf(const Entry& entry) const;
  static uint32_t LookupDirectory(
      const std::unordered_multimap<uint64_t, uint32_t>& index,
      const std::vector<Directory>& directories, const std::wstring& lowerPath);

  bool LoadSnapshot();
  bool SaveSnapshot() const;
  std::vector<std::wstring> SnapshotPaths() const;

  mutable std::mutex entriesMutex_;
  Storage storage_;
  std::vector<std::wstring> roots_;
  size_t maxIndexBytes_ = 512ull * 1024 * 1024;
  bool persistSnapshot_ = true;

  std::vector<std::thread> watchers_;
  std::vector<HANDLE> watchHandles_;
  std::mutex watchMutex_;
  std::thread worker_;
  std::mutex workerMutex_;
  std::condition_variable workerCv_;
  std::atomic<bool> stop_{false};
  std::atomic<bool> rebuildRequested_{false};
  std::atomic<uint64_t> version_{0};
  std::atomic<uint64_t> lastRebuildMs_{0};
  std::atomic<bool> saveSnapshotAfterRebuild_{true};
};
