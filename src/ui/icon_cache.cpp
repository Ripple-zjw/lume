#include "ui/icon_cache.h"

#include <shobjidl.h>
#include <wincodec.h>

#include "core/log.h"
#include "ui/renderer.h"

bool IconCache::Init(Renderer* renderer) {
  renderer_ = renderer;
  return renderer_ != nullptr;
}

ID2D1Bitmap1* IconCache::Get(const std::wstring& parsingName, UINT size) {
  if (parsingName.empty() || !renderer_) return nullptr;
  auto cached = cache_.find(parsingName);
  if (cached != cache_.end()) return cached->second.Get();

  ComPtr<IShellItemImageFactory> factory;
  HRESULT hr = SHCreateItemFromParsingName(parsingName.c_str(), nullptr,
                                           __uuidof(IShellItemImageFactory), factory.PutVoid());
  HBITMAP bitmap = nullptr;
  if (SUCCEEDED(hr) && factory) {
    SIZE requested{static_cast<LONG>(size), static_cast<LONG>(size)};
    hr = factory->GetImage(requested, SIIGBF_ICONONLY | SIIGBF_RESIZETOFIT, &bitmap);
  }

  ComPtr<ID2D1Bitmap1> result;
  if (bitmap) {
    ComPtr<IWICBitmap> wicBitmap;
    if (SUCCEEDED(renderer_->wic()->CreateBitmapFromHBITMAP(bitmap, nullptr, WICBitmapUseAlpha,
                                                            wicBitmap.Put()))) {
      // WIC 从 HBITMAP 得到的格式（32bppBGR）不被 D2D 直接接受，先转预乘 BGRA。
      ComPtr<IWICFormatConverter> converter;
      if (SUCCEEDED(renderer_->wic()->CreateFormatConverter(converter.Put())) &&
          SUCCEEDED(converter->Initialize(wicBitmap.Get(), GUID_WICPixelFormat32bppPBGRA,
                                          WICBitmapDitherTypeNone, nullptr, 0.0,
                                          WICBitmapPaletteTypeCustom))) {
        const HRESULT createHr = renderer_->context()->CreateBitmapFromWicBitmap(
            converter.Get(), nullptr, result.Put());
        if (FAILED(createHr)) LogHr(L"CreateBitmapFromWicBitmap 失败", createHr);
      }
    }
    DeleteObject(bitmap);
  } else {
    LogHr(L"IconCache 取图失败", hr);
  }

  cache_[parsingName] = std::move(result);
  return cache_[parsingName].Get();
}
