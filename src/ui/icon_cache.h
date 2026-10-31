#pragma once

#include <d2d1_3.h>
#include <windows.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "core/com_ptr.h"

class Renderer;

// shell 图标缓存：
//  - Warm() 同步加载（启动预热用，不在按键路径上）
//  - Get() 非阻塞：未命中时入队并返回空，由 worker 取图，UI 线程 DrainCompleted() 转成 D2D 位图
class IconCache {
 public:
  ~IconCache();
  bool Init(Renderer* renderer);
  void Shutdown();

  ID2D1Bitmap1* Get(const std::wstring& parsingName, UINT size = 32);
  bool Warm(const std::wstring& parsingName, UINT size = 32);
  // 在 UI 线程调用：把 worker 完成的 HBITMAP 转成 D2D 位图。
  void DrainCompleted();
  uint64_t version() const { return version_.load(); }

 private:
  struct Completed {
    std::wstring key;
    HBITMAP bitmap = nullptr;
  };
  void WorkerLoop();
  bool LoadBitmap(const std::wstring& key, HBITMAP* bitmap);
  ID2D1Bitmap1* CreateD2DBitmap(HBITMAP bitmap);

  Renderer* renderer_ = nullptr;
  std::unordered_map<std::wstring, ComPtr<ID2D1Bitmap1>> cache_;
  std::unordered_set<std::wstring> queued_;
  std::vector<Completed> completed_;
  std::atomic<uint64_t> version_{0};

  std::thread worker_;
  std::mutex mutex_;
  std::condition_variable cv_;
  std::deque<std::wstring> queue_;
  std::atomic<bool> stop_{false};
};
