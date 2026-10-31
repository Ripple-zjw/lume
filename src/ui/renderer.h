#pragma once

#include <d2d1_3.h>
#include <d3d11_1.h>
#include <dcomp.h>
#include <dwrite_3.h>
#include <dxgi1_6.h>
#include <wincodec.h>
#include <windows.h>

#include <string>
#include <vector>

#include "core/com_ptr.h"

enum class RenderTargetKind { None, Window, Offscreen };

// 负责 D3D11/DXGI/DirectComposition/Direct2D/DirectWrite/WIC 的创建、目标绑定与出图。
class Renderer {
 public:
  Renderer() = default;
  ~Renderer();
  Renderer(const Renderer&) = delete;
  Renderer& operator=(const Renderer&) = delete;

  bool Init();
  bool AttachWindow(HWND hwnd, UINT width, UINT height);
  bool AttachOffscreen(UINT width, UINT height);
  bool Resize(UINT width, UINT height);

  ID2D1DeviceContext* context() const { return ctx_.Get(); }
  IDWriteFactory* dwrite() const { return dwrite_.Get(); }
  ID2D1Factory1* d2dFactory() const { return d2dFactory_.Get(); }
  UINT width() const { return width_; }
  UINT height() const { return height_; }
  RenderTargetKind kind() const { return kind_; }

  bool BeginFrame();
  bool EndFrame();
  bool SaveFrameToPng(const std::wstring& path);
  bool CaptureWindowToPng(const std::wstring& path);

 private:
  bool CreateDeviceIndependent();
  bool CreateSwapChainAndComposition(HWND hwnd);
  bool BindSwapChainTarget();
  bool BindOffscreenTarget();
  bool SavePng(const std::wstring& path, UINT width, UINT height, const BYTE* pixels, UINT stride,
               REFGUID pixelFormat);

  ComPtr<ID3D11Device> d3d_;
  ComPtr<ID3D11DeviceContext> d3dContext_;
  ComPtr<IDXGISwapChain1> swapChain_;
  ComPtr<IDCompositionDevice> compDevice_;
  ComPtr<IDCompositionTarget> compTarget_;
  ComPtr<IDCompositionVisual> compVisual_;
  ComPtr<ID2D1Factory1> d2dFactory_;
  ComPtr<ID2D1Device> d2dDevice_;
  ComPtr<ID2D1DeviceContext> ctx_;
  ComPtr<ID2D1Bitmap1> targetBitmap_;
  ComPtr<ID3D11Texture2D> offscreenTexture_;
  ComPtr<IDWriteFactory> dwrite_;
  ComPtr<IWICImagingFactory> wic_;

  HWND hwnd_ = nullptr;
  UINT width_ = 0;
  UINT height_ = 0;
  RenderTargetKind kind_ = RenderTargetKind::None;
  bool frameOpen_ = false;
};
