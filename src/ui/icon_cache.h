#pragma once

#include <d2d1_3.h>
#include <windows.h>
#include <wincodec.h>

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
//  - 磁盘 PNG 缓存（LLOCALAPPDATAL\eTools\icons），二次启动直接解码
//  - Get() 非阻塞：未命中入队，worker 取图/解码后由 UI 线程转 D2D
//  - Warm() 同步加载（启动预热首屏）
class IconCache {
 public:
  ~IconCache();
  bool Init(Renderer* renderer);
  void Shutdown();

  ID2D1Bitmap1* Get(const std::wstring& parsingName, UINT size = 32);
  bool Warm(const std::wstring& parsingName, UINT size = 32);
  void DrainCompleted();
  uint64_t version() const { return version_.load(); }

 private:
  struct Pixels {
    UINT width = 0;
    UINT height = 0;
    UINT stride = 0;
    std::vector<BYTE> data;
  };
  struct Completed {
    std::wstring key;
    Pixels pixels;
    bool valid = false;
  };

  void WorkerLoop();
  bool LoadPixels(IWICImagingFactory* wic, const std::wstring& key, UINT size, Pixels* pixels);
  bool LoadFromDisk(IWICImagingFactory* wic, const std::wstring& path, Pixels* pixels);
  bool LoadFromShell(IWICImagingFactory* wic, const std::wstring& key, UINT size, Pixels* pixels);
  void SaveToDisk(IWICImagingFactory* wic, const std::wstring& key, const Pixels& pixels);
  ID2D1Bitmap1* CreateD2DBitmap(const Pixels& pixels);
  std::vector<std::wstring> CachePaths(const std::wstring& key) const;

  Renderer* renderer_ = nullptr;
  std::unordered_map<std::wstring, ComPtr<ID2D1Bitmap1>> cache_;
  std::unordered_set<std::wstring> queued_;
  std::vector<Completed> completed_;
  std::atomic<uint64_t> version_{0};
  std::atomic<int> diskHits_{0};
  std::atomic<int> shellLoads_{0};

  std::thread worker_;
  std::mutex mutex_;
  std::condition_variable cv_;
  std::deque<std::wstring> queue_;
  std::atomic<bool> stop_{false};
};
