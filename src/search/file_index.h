#pragma once

#include <windows.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct FileEntry {
  std::wstring path;       // 完整路径
  std::wstring lowerPath;  // 小写路径，供匹配
  uint32_t nameOffset = 0; // 文件名在 lowerPath 中的起点
  bool isDirectory = false;
};

struct FileHit {
  FileEntry entry;
  size_t score = 0;
};

// 文件/文件夹索引：枚举配置根目录、快照缓存、目录 watcher 增量更新、查询。
class FileIndex {
 public:
  FileIndex() = default;
  ~FileIndex();
  FileIndex(const FileIndex&) = delete;
  FileIndex& operator=(const FileIndex&) = delete;

  // 加载快照并启动后台重建 + watcher。返回 false 表示没有可用根目录。
  bool Start();
  void Stop();

  // 测试/CLI 指定根目录（需在 Start 前调用；空表示使用默认桌面/文档/下载）。
  void SetRoots(std::vector<std::wstring> roots);

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
  void EnsureRoots();
  void WorkerLoop();
  void RebuildAndSwap();
  void WatchLoop(std::wstring root);
  void HandleNotifications(const std::wstring& root, const char* buffer, DWORD bytes);
  void AddEntryLocked(const std::wstring& path, bool isDirectory);
  void RemoveEntryLocked(const std::wstring& path);

  bool LoadSnapshot(std::vector<FileEntry>* entries) const;
  void SaveSnapshot(const std::vector<FileEntry>& entries);
  std::vector<std::wstring> SnapshotPaths() const;

  mutable std::mutex entriesMutex_;
  std::vector<FileEntry> entries_;
  std::vector<std::wstring> roots_;

  std::vector<std::thread> watchers_;
  std::vector<HANDLE> watchHandles_;
  std::mutex watchMutex_;
  std::thread worker_;
  std::mutex workerMutex_;
  std::condition_variable workerCv_;
  std::atomic<bool> stop_{false};
  std::atomic<bool> rebuildRequested_{false};
  std::atomic<uint64_t> version_{0};
};
