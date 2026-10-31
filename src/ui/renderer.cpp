#include "ui/renderer.h"

#include <cstring>

#include "core/log.h"

Renderer::~Renderer() {
  if (ctx_) ctx_->SetTarget(nullptr);
  ctx_.Reset();
  targetBitmap_.Reset();
  offscreenTexture_.Reset();
  compVisual_.Reset();
  compTarget_.Reset();
  compDevice_.Reset();
  swapChain_.Reset();
  d2dDevice_.Reset();
  d2dFactory_.Reset();
  dwrite_.Reset();
  wic_.Reset();
  d3dContext_.Reset();
  d3d_.Reset();
}

bool Renderer::Init() { return CreateDeviceIndependent(); }

bool Renderer::CreateDeviceIndependent() {
  UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
  D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
  D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
  HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, levels,
                                 ARRAYSIZE(levels), D3D11_SDK_VERSION, d3d_.Put(), &level,
                                 d3dContext_.Put());
  if (FAILED(hr)) {
    LogHr(L"D3D11CreateDevice(hardware)", hr);
    hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, flags, levels, ARRAYSIZE(levels),
                           D3D11_SDK_VERSION, d3d_.Put(), &level, d3dContext_.Put());
    if (FAILED(hr)) {
      LogHr(L"D3D11CreateDevice(warp)", hr);
      return false;
    }
    Log(L"图形: WARP 软件渲染");
  } else {
    Log(L"图形: 硬件设备, feature level 0x%X", static_cast<unsigned>(level));
  }

  ComPtr<IDXGIDevice> dxgiDevice;
  hr = d3d_->QueryInterface(__uuidof(IDXGIDevice), dxgiDevice.PutVoid());
  if (FAILED(hr)) {
    LogHr(L"QueryInterface(IDXGIDevice)", hr);
    return false;
  }

  hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1), nullptr,
                         d2dFactory_.PutVoid());
  if (FAILED(hr)) {
    LogHr(L"D2D1CreateFactory", hr);
    return false;
  }
  hr = d2dFactory_->CreateDevice(dxgiDevice.Get(), d2dDevice_.Put());
  if (FAILED(hr)) {
    LogHr(L"ID2D1Factory1::CreateDevice", hr);
    return false;
  }
  hr = d2dDevice_->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, ctx_.Put());
  if (FAILED(hr)) {
    LogHr(L"CreateDeviceContext", hr);
    return false;
  }

  hr = DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                           reinterpret_cast<IUnknown**>(dwrite_.Put()));
  if (FAILED(hr)) {
    LogHr(L"DWriteCreateFactory", hr);
    return false;
  }

  hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                        __uuidof(IWICImagingFactory), wic_.PutVoid());
  if (FAILED(hr)) {
    LogHr(L"CoCreateInstance(WIC)", hr);
    return false;
  }
  return true;
}

bool Renderer::CreateSwapChainAndComposition(HWND hwnd) {
  ComPtr<IDXGIDevice> dxgiDevice;
  HRESULT hr = d3d_->QueryInterface(__uuidof(IDXGIDevice), dxgiDevice.PutVoid());
  if (FAILED(hr)) return false;

  ComPtr<IDXGIAdapter> adapter;
  hr = dxgiDevice->GetAdapter(adapter.Put());
  if (FAILED(hr)) return false;
  ComPtr<IDXGIFactory2> factory;
  hr = adapter->GetParent(__uuidof(IDXGIFactory2), factory.PutVoid());
  if (FAILED(hr)) return false;

  DXGI_SWAP_CHAIN_DESC1 desc{};
  desc.Width = width_;
  desc.Height = height_;
  desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
  desc.SampleDesc.Count = 1;
  desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
  desc.BufferCount = 2;
  desc.Scaling = DXGI_SCALING_STRETCH;
  desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
  desc.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
  hr = factory->CreateSwapChainForComposition(d3d_.Get(), &desc, nullptr, swapChain_.Put());
  if (FAILED(hr)) {
    LogHr(L"CreateSwapChainForComposition", hr);
    return false;
  }

  hr = DCompositionCreateDevice(dxgiDevice.Get(), __uuidof(IDCompositionDevice),
                                compDevice_.PutVoid());
  if (FAILED(hr)) {
    LogHr(L"DCompositionCreateDevice", hr);
    return false;
  }
  hr = compDevice_->CreateTargetForHwnd(hwnd, TRUE, compTarget_.Put());
  if (FAILED(hr)) {
    LogHr(L"CreateTargetForHwnd", hr);
    return false;
  }
  hr = compDevice_->CreateVisual(compVisual_.Put());
  if (FAILED(hr)) {
    LogHr(L"CreateVisual", hr);
    return false;
  }
  hr = compVisual_->SetContent(swapChain_.Get());
  if (FAILED(hr)) return false;
  hr = compTarget_->SetRoot(compVisual_.Get());
  if (FAILED(hr)) return false;
  hr = compDevice_->Commit();
  if (FAILED(hr)) return false;
  return true;
}

bool Renderer::BindSwapChainTarget() {
  if (!swapChain_) return false;
  ComPtr<IDXGISurface> surface;
  HRESULT hr = swapChain_->GetBuffer(0, __uuidof(IDXGISurface), surface.PutVoid());
  if (FAILED(hr)) {
    LogHr(L"GetBuffer(0)", hr);
    return false;
  }
  D2D1_BITMAP_PROPERTIES1 props = D2D1::BitmapProperties1(
      D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
      D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.0f, 96.0f);
  hr = ctx_->CreateBitmapFromDxgiSurface(surface.Get(), &props, targetBitmap_.Put());
  if (FAILED(hr)) {
    LogHr(L"CreateBitmapFromDxgiSurface(swapchain)", hr);
    return false;
  }
  ctx_->SetTarget(targetBitmap_.Get());
  return true;
}

bool Renderer::BindOffscreenTarget() {
  D3D11_TEXTURE2D_DESC desc{};
  desc.Width = width_;
  desc.Height = height_;
  desc.MipLevels = 1;
  desc.ArraySize = 1;
  desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
  desc.SampleDesc.Count = 1;
  desc.Usage = D3D11_USAGE_DEFAULT;
  desc.BindFlags = D3D11_BIND_RENDER_TARGET;
  HRESULT hr = d3d_->CreateTexture2D(&desc, nullptr, offscreenTexture_.Put());
  if (FAILED(hr)) {
    LogHr(L"CreateTexture2D(offscreen)", hr);
    return false;
  }
  ComPtr<IDXGISurface> surface;
  hr = offscreenTexture_->QueryInterface(__uuidof(IDXGISurface), surface.PutVoid());
  if (FAILED(hr)) return false;
  D2D1_BITMAP_PROPERTIES1 props = D2D1::BitmapProperties1(
      D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
      D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.0f, 96.0f);
  hr = ctx_->CreateBitmapFromDxgiSurface(surface.Get(), &props, targetBitmap_.Put());
  if (FAILED(hr)) {
    LogHr(L"CreateBitmapFromDxgiSurface(offscreen)", hr);
    return false;
  }
  ctx_->SetTarget(targetBitmap_.Get());
  return true;
}

bool Renderer::AttachWindow(HWND hwnd, UINT width, UINT height) {
  hwnd_ = hwnd;
  width_ = width;
  height_ = height;
  if (!CreateSwapChainAndComposition(hwnd)) return false;
  kind_ = RenderTargetKind::Window;
  return BindSwapChainTarget();
}

bool Renderer::AttachOffscreen(UINT width, UINT height) {
  width_ = width;
  height_ = height;
  kind_ = RenderTargetKind::Offscreen;
  return BindOffscreenTarget();
}

bool Renderer::Resize(UINT width, UINT height) {
  if (kind_ == RenderTargetKind::None || width == 0 || height == 0) return false;
  width_ = width;
  height_ = height;
  ctx_->SetTarget(nullptr);
  targetBitmap_.Reset();
  if (kind_ == RenderTargetKind::Window) {
    if (!swapChain_) return false;
    HRESULT hr = swapChain_->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, 0);
    if (FAILED(hr)) {
      LogHr(L"ResizeBuffers", hr);
      return false;
    }
    return BindSwapChainTarget();
  }
  offscreenTexture_.Reset();
  return BindOffscreenTarget();
}

bool Renderer::BeginFrame() {
  if (!ctx_ || kind_ == RenderTargetKind::None) return false;
  ctx_->BeginDraw();
  frameOpen_ = true;
  ctx_->Clear(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f));
  return true;
}

bool Renderer::EndFrame() {
  if (!frameOpen_) return false;
  frameOpen_ = false;
  HRESULT hr = ctx_->EndDraw();
  if (FAILED(hr)) LogHr(L"ID2D1DeviceContext::EndDraw", hr);
  if (kind_ == RenderTargetKind::Window && swapChain_) {
    // 启动器是"点开即看"的静态界面：不等垂直同步以避免最多一帧的额外延迟。
    HRESULT presentHr = swapChain_->Present(0, 0);
    if (FAILED(presentHr)) LogHr(L"Present", presentHr);
  }
  return SUCCEEDED(hr);
}

bool Renderer::SavePng(const std::wstring& path, UINT width, UINT height, const BYTE* pixels,
                       UINT stride, REFGUID pixelFormat) {
  if (!wic_ || !pixels) return false;

  ComPtr<IWICStream> stream;
  HRESULT hr = wic_->CreateStream(stream.Put());
  if (FAILED(hr)) return false;
  hr = stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE);
  if (FAILED(hr)) return false;

  ComPtr<IWICBitmapEncoder> encoder;
  hr = wic_->CreateEncoder(GUID_ContainerFormatPng, nullptr, encoder.Put());
  if (FAILED(hr)) return false;
  hr = encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache);
  if (FAILED(hr)) return false;

  ComPtr<IWICBitmapFrameEncode> frame;
  ComPtr<IPropertyBag2> props;
  hr = encoder->CreateNewFrame(frame.Put(), props.Put());
  if (FAILED(hr)) return false;
  hr = frame->Initialize(props.Get());
  if (FAILED(hr)) return false;
  hr = frame->SetSize(width, height);
  if (FAILED(hr)) return false;

  WICPixelFormatGUID format = pixelFormat;
  hr = frame->SetPixelFormat(&format);
  if (FAILED(hr)) return false;
  if (!IsEqualGUID(format, pixelFormat)) {
    Log(L"WIC 改写了像素格式，中止保存");
    return false;
  }
  hr = frame->WritePixels(height, stride, stride * height, const_cast<BYTE*>(pixels));
  if (FAILED(hr)) return false;
  hr = frame->Commit();
  if (FAILED(hr)) return false;
  hr = encoder->Commit();
  return SUCCEEDED(hr);
}

bool Renderer::SaveFrameToPng(const std::wstring& path) {
  if (kind_ != RenderTargetKind::Offscreen || !offscreenTexture_) return false;
  if (frameOpen_) EndFrame();

  D3D11_TEXTURE2D_DESC desc{};
  offscreenTexture_->GetDesc(&desc);
  desc.BindFlags = 0;
  desc.Usage = D3D11_USAGE_STAGING;
  desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  desc.MiscFlags = 0;
  ComPtr<ID3D11Texture2D> staging;
  HRESULT hr = d3d_->CreateTexture2D(&desc, nullptr, staging.Put());
  if (FAILED(hr)) {
    LogHr(L"CreateTexture2D(staging)", hr);
    return false;
  }
  d3dContext_->CopyResource(staging.Get(), offscreenTexture_.Get());
  D3D11_MAPPED_SUBRESOURCE mapped{};
  hr = d3dContext_->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped);
  if (FAILED(hr)) {
    LogHr(L"Map(staging)", hr);
    return false;
  }
  const bool ok =
      SavePng(path, width_, height_, static_cast<const BYTE*>(mapped.pData), mapped.RowPitch,
              GUID_WICPixelFormat32bppBGRA);
  d3dContext_->Unmap(staging.Get(), 0);
  if (ok) Log(L"离屏渲染已保存: %ls (%ux%u)", path.c_str(), width_, height_);
  return ok;
}

bool Renderer::CaptureWindowToPng(const std::wstring& path) {
  RECT rect{};
  if (!hwnd_ || !GetWindowRect(hwnd_, &rect)) return false;
  const int width = rect.right - rect.left;
  const int height = rect.bottom - rect.top;
  if (width <= 0 || height <= 0) return false;

  HDC screen = GetDC(nullptr);
  HDC memory = CreateCompatibleDC(screen);
  BITMAPINFO info{};
  info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  info.bmiHeader.biWidth = width;
  info.bmiHeader.biHeight = -height;  // top-down
  info.bmiHeader.biPlanes = 1;
  info.bmiHeader.biBitCount = 32;
  info.bmiHeader.biCompression = BI_RGB;
  void* bits = nullptr;
  HBITMAP bitmap = CreateDIBSection(memory, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
  bool ok = false;
  if (bitmap && bits) {
    HGDIOBJ old = SelectObject(memory, bitmap);
    if (BitBlt(memory, 0, 0, width, height, screen, rect.left, rect.top, SRCCOPY | CAPTUREBLT)) {
      auto* pixels = static_cast<DWORD*>(bits);
      const size_t count = static_cast<size_t>(width) * static_cast<size_t>(height);
      for (size_t i = 0; i < count; ++i) pixels[i] |= 0xFF000000u;
      ok = SavePng(path, static_cast<UINT>(width), static_cast<UINT>(height),
                   static_cast<const BYTE*>(bits), static_cast<UINT>(width * 4),
                   GUID_WICPixelFormat32bppBGRA);
    }
    SelectObject(memory, old);
  }
  if (bitmap) DeleteObject(bitmap);
  DeleteDC(memory);
  ReleaseDC(nullptr, screen);
  if (ok) Log(L"窗口合成截图已保存: %ls (%dx%d)", path.c_str(), width, height);
  return ok;
}
