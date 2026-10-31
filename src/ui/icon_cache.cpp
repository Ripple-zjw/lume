#include "ui/icon_cache.h"

#include <shobjidl.h>

#include <algorithm>
#include <cstdio>
#include <utility>

#include "core/log.h"
#include "ui/renderer.h"

namespace {

constexpr UINT kIconSize = 32;

uint64_t Fnv1a(const std::wstring& text) {
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

std::wstring ExeDirectory() {
  wchar_t path[MAX_PATH] = {};
  if (GetModuleFileNameW(nullptr, path, ARRAYSIZE(path)) == 0) return L".";
  std::wstring dir(path);
  const size_t slash = dir.find_last_of(L"\\/");
  if (slash != std::wstring::npos) dir.resize(slash);
  return dir;
}

}  // namespace

IconCache::~IconCache() { Shutdown(); }

std::vector<std::wstring> IconCache::CachePaths(const std::wstring& key) const {
  wchar_t name[32];
  swprintf(name, ARRAYSIZE(name), L"%016llx.png", static_cast<unsigned long long>(Fnv1a(key)));
  std::vector<std::wstring> paths;
  const std::wstring base = GetEnv(L"LOCALAPPDATA");
  if (!base.empty()) {
    const std::wstring dir = base + L"\\Lume\\icons";
    if (EnsureDirectory(dir)) paths.push_back(dir + L"\\" + name);
  }
  const std::wstring fallbackDir = ExeDirectory() + L"\\icons";
  if (EnsureDirectory(fallbackDir)) {
    const std::wstring fallback = fallbackDir + L"\\" + name;
    if (std::find(paths.begin(), paths.end(), fallback) == paths.end()) paths.push_back(fallback);
  }
  return paths;
}

bool IconCache::LoadFromDisk(IWICImagingFactory* wic, const std::wstring& path,
                             Pixels* pixels) {
  ComPtr<IWICBitmapDecoder> decoder;
  if (FAILED(wic->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
                                            WICDecodeMetadataCacheOnDemand, decoder.Put()))) {
    return false;
  }
  ComPtr<IWICBitmapFrameDecode> frame;
  if (FAILED(decoder->GetFrame(0, frame.Put()))) return false;
  ComPtr<IWICFormatConverter> converter;
  if (FAILED(wic->CreateFormatConverter(converter.Put()))) return false;
  if (FAILED(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppPBGRA,
                                   WICBitmapDitherTypeNone, nullptr, 0.0,
                                   WICBitmapPaletteTypeCustom))) {
    return false;
  }
  UINT width = 0;
  UINT height = 0;
  if (FAILED(converter->GetSize(&width, &height)) || width == 0 || height == 0) return false;
  pixels->width = width;
  pixels->height = height;
  pixels->stride = width * 4;
  pixels->data.resize(static_cast<size_t>(pixels->stride) * height);
  return SUCCEEDED(converter->CopyPixels(nullptr, pixels->stride,
                                         static_cast<UINT>(pixels->data.size()),
                                         pixels->data.data()));
}

bool IconCache::LoadFromShell(IWICImagingFactory* wic, const std::wstring& key, UINT size,
                              Pixels* pixels) {
  ComPtr<IShellItemImageFactory> factory;
  HRESULT hr = SHCreateItemFromParsingName(key.c_str(), nullptr,
                                           __uuidof(IShellItemImageFactory), factory.PutVoid());
  if (FAILED(hr) || !factory) return false;
  HBITMAP bitmap = nullptr;
  SIZE requested{static_cast<LONG>(size), static_cast<LONG>(size)};
  if (FAILED(factory->GetImage(requested, SIIGBF_ICONONLY | SIIGBF_RESIZETOFIT, &bitmap)) ||
      !bitmap) {
    return false;
  }
  ComPtr<IWICBitmap> wicBitmap;
  bool ok = false;
  if (SUCCEEDED(wic->CreateBitmapFromHBITMAP(bitmap, nullptr, WICBitmapUseAlpha,
                                             wicBitmap.Put()))) {
    ComPtr<IWICFormatConverter> converter;
    if (SUCCEEDED(wic->CreateFormatConverter(converter.Put())) &&
        SUCCEEDED(converter->Initialize(wicBitmap.Get(), GUID_WICPixelFormat32bppPBGRA,
                                        WICBitmapDitherTypeNone, nullptr, 0.0,
                                        WICBitmapPaletteTypeCustom))) {
      UINT width = 0;
      UINT height = 0;
      if (SUCCEEDED(converter->GetSize(&width, &height)) && width > 0 && height > 0) {
        pixels->width = width;
        pixels->height = height;
        pixels->stride = width * 4;
        pixels->data.resize(static_cast<size_t>(pixels->stride) * height);
        ok = SUCCEEDED(converter->CopyPixels(nullptr, pixels->stride,
                                             static_cast<UINT>(pixels->data.size()),
                                             pixels->data.data()));
      }
    }
  }
  DeleteObject(bitmap);
  return ok;
}

void IconCache::SaveToDisk(IWICImagingFactory* wic, const std::wstring& key,
                           const Pixels& pixels) {
  const std::vector<std::wstring> paths = CachePaths(key);
  if (paths.empty()) return;
  const std::wstring& path = paths.front();
  ComPtr<IWICStream> stream;
  if (FAILED(wic->CreateStream(stream.Put()))) return;
  if (FAILED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE))) return;
  ComPtr<IWICBitmapEncoder> encoder;
  if (FAILED(wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, encoder.Put()))) return;
  if (FAILED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache))) return;
  ComPtr<IWICBitmapFrameEncode> frame;
  ComPtr<IPropertyBag2> properties;
  if (FAILED(encoder->CreateNewFrame(frame.Put(), properties.Put()))) return;
  if (FAILED(frame->Initialize(properties.Get()))) return;
  if (FAILED(frame->SetSize(pixels.width, pixels.height))) return;
  WICPixelFormatGUID format = GUID_WICPixelFormat32bppPBGRA;
  if (FAILED(frame->SetPixelFormat(&format))) return;
  if (FAILED(frame->WritePixels(pixels.height, pixels.stride,
                                static_cast<UINT>(pixels.data.size()),
                                const_cast<BYTE*>(pixels.data.data())))) {
    return;
  }
  if (FAILED(frame->Commit())) return;
  encoder->Commit();
}

bool IconCache::LoadPixels(IWICImagingFactory* wic, const std::wstring& key, UINT size,
                           Pixels* pixels) {
  for (const std::wstring& path : CachePaths(key)) {
    if (LoadFromDisk(wic, path, pixels)) {
      diskHits_.fetch_add(1);
      return true;
    }
  }
  if (!LoadFromShell(wic, key, size, pixels)) return false;
  shellLoads_.fetch_add(1);
  SaveToDisk(wic, key, *pixels);
  return true;
}

ID2D1Bitmap1* IconCache::CreateD2DBitmap(const Pixels& pixels) {
  if (pixels.data.empty() || !renderer_) return nullptr;
  D2D1_BITMAP_PROPERTIES1 properties = D2D1::BitmapProperties1(
      D2D1_BITMAP_OPTIONS_NONE,
      D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
  ComPtr<ID2D1Bitmap1> bitmap;
  if (FAILED(renderer_->context()->CreateBitmap(
          D2D1::SizeU(pixels.width, pixels.height), pixels.data.data(), pixels.stride, &properties,
          bitmap.Put()))) {
    return nullptr;
  }
  return bitmap.Detach();
}

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
  std::lock_guard<std::mutex> lock(mutex_);
  Log(L"图标缓存: 磁盘命中 %d, shell 取图 %d", diskHits_.load(), shellLoads_.load());
  completed_.clear();
  queue_.clear();
  queued_.clear();
}

void IconCache::WorkerLoop() {
  CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  ComPtr<IWICImagingFactory> wic;
  CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                   __uuidof(IWICImagingFactory), wic.PutVoid());

  while (!stop_.load()) {
    std::wstring key;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      cv_.wait(lock, [this]() { return stop_.load() || !queue_.empty(); });
      if (stop_.load()) break;
      key = std::move(queue_.front());
      queue_.pop_front();
    }
    Completed completed;
    completed.key = key;
    if (wic) {
      completed.valid = LoadPixels(wic.Get(), key, kIconSize, &completed.pixels);
    }
    {
      std::lock_guard<std::mutex> lock(mutex_);
      completed_.push_back(std::move(completed));
    }
    version_.fetch_add(1);
  }
  CoUninitialize();
}

void IconCache::DrainCompleted() {
  std::vector<Completed> completed;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    completed.swap(completed_);
  }
  for (Completed& item : completed) {
    ComPtr<ID2D1Bitmap1> bitmap;
    if (item.valid) {
      ID2D1Bitmap1* raw = CreateD2DBitmap(item.pixels);
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
  if (parsingName.empty() || !renderer_) return false;
  auto cached = cache_.find(parsingName);
  if (cached != cache_.end()) return cached->second.Get() != nullptr;
  Pixels pixels;
  if (!LoadPixels(renderer_->wic(), parsingName, size, &pixels)) {
    cache_[parsingName] = ComPtr<ID2D1Bitmap1>();
    return false;
  }
  ID2D1Bitmap1* raw = CreateD2DBitmap(pixels);
  ComPtr<ID2D1Bitmap1> bitmap;
  if (raw) bitmap.Attach(raw);
  cache_[parsingName] = std::move(bitmap);
  return raw != nullptr;
}
