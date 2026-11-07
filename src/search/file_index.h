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

#include "search/file_hit.h"
#include "search/filter.h"

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

  // 仅清除：删除快照文件并清空内存索引，不自动重建（设置窗「清除索引」）。
  void ClearIndex();
  // 手动重建：请求后台重建并写回快照，不受 60s 冷却限制（设置窗「重建索引」）。
  void RebuildIndex();

  // 测试/CLI 指定根目录（需在 Start 前调用；空表示使用默认桌面/文档/下载）。
  void SetRoots(std::vector<std::wstring> roots);

  // 快照目录（需在 Start 前调用；空表示默认 %LOCALAPPDATA%\Lume）。
  void SetIndexDirectory(std::wstring directory);
  // 解析后的快照目录（绝对路径；默认目录不可用时回退 exe 目录）。
  static std::wstring DefaultIndexDirectory();
  std::wstring indexDirectory() const;
  // 当前快照文件路径（<indexDirectory>\files.idx）。
  std::wstring snapshotPath() const;
  // 现有快照的字节数（取候选里 mtime 最新的那份；不存在 = 0）。
  uint64_t snapshotBytes() const;
  // 快照目录是否可用（能创建/已存在）。
  bool snapshotWritable() const;

  // 索引快照允许的最大字节数（默认 512MB，最大 256GB）。
  void SetMaxIndexBytes(size_t value);
  // 是否把重建结果写回 files.idx（常驻模式启动即重建，不需要落盘）。
  void SetPersistSnapshot(bool value) { persistSnapshot_ = value; }

  std::vector<std::wstring> roots() const;
  // filter 非空时只返回属于该筛选组的命中；过滤发生在候选打分之后、截断之前，
  // 所以不会出现「前 80 条都是别的类型、真正的命中被截掉」。
  std::vector<FileHit> Search(const std::wstring& query, size_t limit,
                              const FilterGroup* filter = nullptr) const;
  size_t size() const;
  uint64_t version() const { return version_.load(); }
  // 是否正在后台重建（设置窗状态用）。
  bool rebuilding() const { return rebuilding_.load(); }
  // 上次重建耗时（毫秒；0 = 尚未完成过一次重建）。
  uint64_t lastRebuildDurationMs() const { return lastRebuildDurationMs_.load(); }

  // 加载快照但不启动后台线程（截图/诊断用）。
  bool LoadCachedSnapshot();

  // 基准/自检专用：在内存里造 count 条假条目（不碰磁盘、不启动任何线程）。
  // 目录表、名字 arena、charMask/pathHash 的填充方式与真实枚举一致，
  // 所以 Search() 走的是和线上完全相同的「预筛 → 打分 → 过滤 → 排序」代码。
  void LoadSyntheticForBenchmark(size_t count, uint32_t seed);

  // 单元测试专用：用给定路径（以 \ 结尾表示目录）填充内存索引，不碰磁盘。
  // 与真实索引共用同一套目录表/名字 arena 结构，Search() 的排序口径因此可测。
  void LoadPathsForTest(const std::vector<std::wstring>& paths);

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
  // 快照/日志/图标本身在索引根内时，避免写文件触发 watcher 再触发重建的死循环。
  bool ShouldIgnoreWatchPath(const std::wstring& fullPath) const;

  mutable std::mutex entriesMutex_;
  Storage storage_;
  std::vector<std::wstring> roots_;
  std::wstring indexDir_;  // 空 = 默认目录
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
  std::atomic<bool> rebuilding_{false};    // RebuildAndSwap 执行中
  std::atomic<bool> discardBuild_{false};  // 清除索引时丢弃正在跑的重建结果
  std::atomic<uint64_t> version_{0};
  std::atomic<uint64_t> lastRebuildMs_{0};
  std::atomic<uint64_t> lastRebuildDurationMs_{0};
  std::atomic<bool> saveSnapshotAfterRebuild_{true};
};
