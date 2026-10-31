#include "ui/icon_cache.h"

#include <shobjidl.h>
#include <wincodec.h>

#include <utility>

#include "core/log.h"
#include "ui/renderer.h"

IconCache::~IconCache() { Shutdown(); }

bool IconCache::Init(Renderer* renderer) {
  renderer_ = renderer;
  if (!renderer_) return false;
  stop_.store(false);
  worker_ = std::thread([this]() { WorkerLoop(); });
  return true;
}

void IconCache::Shutdown() {
  if (stop_.exchange(true)) return;
  cv_.notify_all();
  if (worker_.joinable()) worker_.join();
  {
    std::lock_guard<std::mutex> lock(mutex_);
    for (Completed& item : completed_) {
      if (item.bitmap) DeleteObject(item.bitmap);
    }
    completed_.clear();
    queue_.clear();
    queued_.clear();
  }
}

void IconCache::WorkerLoop() {
  CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  while (!stop_.load()) {
    std::wstring key;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      cv_.wait(lock, [this]() { return stop_.load() || !queue_.empty(); });
      if (stop_.load()) break;
      key = std::move(queue_.front());
      queue_.pop_front();
    }
    HBITMAP bitmap = nullptr;
    if (!LoadBitmap(key, &bitmap)) bitmap = nullptr;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      completed_.push_back({std::move(key), bitmap});
    }
    version_.fetch_add(1);
  }
  CoUninitialize();
}

bool IconCache::LoadBitmap(const std::wstring& key, HBITMAP* bitmap) {
  ComPtr<IShellItemImageFactory> factory;
  HRESULT hr = SHCreateItemFromParsingName(key.c_str(), nullptr,
                                           __uuidof(IShellItemImageFactory), factory.PutVoid());
  if (FAILED(hr) || !factory) return false;
  SIZE requested{32, 32};
  hr = factory->GetImage(requested, SIIGBF_ICONONLY | SIIGBF_RESIZETOFIT, bitmap);
  return SUCCEEDED(hr) && *bitmap != nullptr;
}

ID2D1Bitmap1* IconCache::CreateD2DBitmap(HBITMAP bitmap) {
  ComPtr<IWICBitmap> wicBitmap;
  if (FAILED(renderer_->wic()->CreateBitmapFromHBITMAP(bitmap, nullptr, WICBitmapUseAlpha,
                                                       wicBitmap.Put()))) {
    return nullptr;
  }
  ComPtr<IWICFormatConverter> converter;
  if (FAILED(renderer_->wic()->CreateFormatConverter(converter.Put())) ||
      FAILED(converter->Initialize(wicBitmap.Get(), GUID_WICPixelFormat32bppPBGRA,
                                   WICBitmapDitherTypeNone, nullptr, 0.0,
                                   WICBitmapPaletteTypeCustom))) {
    return nullptr;
  }
  ComPtr<ID2D1Bitmap1> result;
  if (FAILED(renderer_->context()->CreateBitmapFromWicBitmap(converter.Get(), nullptr,
                                                              result.Put()))) {
    return nullptr;
  }
  return result.Detach();
}

void IconCache::DrainCompleted() {
  std::vector<Completed> completed;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    completed.swap(completed_);
  }
  for (Completed& item : completed) {
    ComPtr<ID2D1Bitmap1> bitmap;
    if (item.bitmap) {
      ID2D1Bitmap1* raw = CreateD2DBitmap(item.bitmap);
      DeleteObject(item.bitmap);
      if (raw) bitmap.Attach(raw);
    }
    cache_[item.key] = std::move(bitmap);
  }
}

ID2D1Bitmap1* IconCache::Get(const std::wstring& parsingName, UINT size) {
  (void)size;
  if (parsingName.empty() || !renderer_) return nullptr;
  auto cached = cache_.find(parsingName);
  if (cached != cache_.end()) return cached->second.Get();
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (queued_.insert(parsingName).second) queue_.push_back(parsingName);
  }
  cv_.notify_one();
  return nullptr;
}

bool IconCache::Warm(const std::wstring& parsingName, UINT size) {
  (void)size;
  if (parsingName.empty() || !renderer_) return false;
  auto cached = cache_.find(parsingName);
  if (cached != cache_.end()) return cached->second.Get() != nullptr;
  HBITMAP bitmap = nullptr;
  if (!LoadBitmap(parsingName, &bitmap)) {
    cache_[parsingName] = ComPtr<ID2D1Bitmap1>();
    return false;
  }
  ID2D1Bitmap1* d2d = CreateD2DBitmap(bitmap);
  DeleteObject(bitmap);
  ComPtr<ID2D1Bitmap1> holder;
  if (d2d) holder.Attach(d2d);
  cache_[parsingName] = std::move(holder);
  return d2d != nullptr;
}
