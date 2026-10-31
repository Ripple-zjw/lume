// eTools P0 渲染探针
//
// 目的（P0 工具链闸门）：
//   1. 验证 MinGW-w64 + D3D11/DXGI + Direct2D/DirectWrite + DirectComposition 能编译链接；
//   2. 验证 DComp 组合的透明窗口（无重定向表面）能显示桌面并绘制自定义内容；
//   3. 验证离屏渲染 PNG 自检管线（与窗口共用同一份 DrawScene 代码）；
//   4. 验证中文字体、圆角、透明度、暗色主题的实际像素效果。
//
// 用法：
//   eTools.exe                      显示 1.5 秒后退出（冒烟测试）
//   eTools.exe --shot=out.png       离屏渲染（假桌面背景）并保存 PNG
//   eTools.exe --capture=out.png    显示窗口 400ms 后截取窗口区域（含桌面合成结果）
//   eTools.exe --glass              低透明度面板，用于观察 DWM 背景是否生效
//   eTools.exe --mica               尝试 DWMWA_SYSTEMBACKDROP_TYPE 并打印 HRESULT
//   eTools.exe --scale=1.5          以 150% 缩放渲染
//   eTools.exe --query=微信          指定输入框内容（离屏/截图均可）
//   eTools.exe --ms=800             自定义自动退出时长

#include <windows.h>

#include <d3d11_1.h>
#include <dxgi1_6.h>
#include <d2d1_3.h>
#include <dwrite_3.h>
#include <wincodec.h>
#include <dcomp.h>
#include <dwmapi.h>
#include <objbase.h>

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <string>
#include <vector>

#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif
#ifndef DWMWA_WINDOW_CORNER_PREFERENCE
#define DWMWA_WINDOW_CORNER_PREFERENCE 33
#endif
#ifndef DWMWA_SYSTEMBACKDROP_TYPE
#define DWMWA_SYSTEMBACKDROP_TYPE 38
#endif
#ifndef DWMSBT_MAINWINDOW
#define DWMSBT_MAINWINDOW 2
#endif
#ifndef DWMWCP_ROUND
#define DWMWCP_ROUND 2
#endif

namespace {

constexpr wchar_t kWindowClass[] = L"eToolsProbeWindow";
constexpr float kPanelWidth = 720.0f;
constexpr float kPanelHeight = 248.0f;
constexpr UINT_PTR kTimerCapture = 1;
constexpr UINT_PTR kTimerExit = 2;

template <typename T>
class ComPtr {
 public:
  ComPtr() = default;
  ~ComPtr() { Reset(); }
  ComPtr(const ComPtr&) = delete;
  ComPtr& operator=(const ComPtr&) = delete;

  T* Get() const { return ptr_; }
  T* operator->() const { return ptr_; }
  explicit operator bool() const { return ptr_ != nullptr; }
  T** Put() {
    Reset();
    return &ptr_;
  }
  void** PutVoid() {
    Reset();
    return reinterpret_cast<void**>(&ptr_);
  }
  void Reset() {
    if (ptr_) {
      ptr_->Release();
      ptr_ = nullptr;
    }
  }

 private:
  T* ptr_ = nullptr;
};

struct Options {
  std::wstring shot;
  std::wstring capture;
  std::wstring query;
  float scale = 1.0f;
  int autoExitMs = 1500;
  bool glass = false;
  bool mica = false;
};

struct AppState {
  HWND hwnd = nullptr;
  ComPtr<ID3D11Device> d3d;
  ComPtr<ID3D11DeviceContext> d3dContext;
  ComPtr<IDXGISwapChain1> swapChain;
  ComPtr<ID3D11Texture2D> offscreenTexture;
  ComPtr<IDCompositionDevice> compDevice;
  ComPtr<IDCompositionTarget> compTarget;
  ComPtr<IDCompositionVisual> compVisual;
  ComPtr<ID2D1Factory1> d2dFactory;
  ComPtr<ID2D1Device> d2dDevice;
  ComPtr<ID2D1DeviceContext> d2dContext;
  ComPtr<ID2D1Bitmap1> d2dTarget;
  ComPtr<IDWriteFactory> dwrite;
  ComPtr<IWICImagingFactory> wic;
  ComPtr<IDWriteTextFormat> queryFormat;
  ComPtr<IDWriteTextFormat> titleFormat;
  ComPtr<IDWriteTextFormat> subtitleFormat;
  Options options;
  float actualScale = 1.0f;
};

AppState g_app;

int Log(const wchar_t* format, ...) {
  wchar_t buffer[2048];
  va_list args;
  va_start(args, format);
  int written = vswprintf(buffer, ARRAYSIZE(buffer), format, args);
  va_end(args);
  if (written < 0) return written;
  char utf8[8192];
  int bytes = WideCharToMultiByte(CP_UTF8, 0, buffer, written, utf8, sizeof(utf8) - 1, nullptr, nullptr);
  if (bytes < 0) bytes = 0;
  utf8[bytes] = '\0';
  fputs(utf8, stdout);
  fputc('\n', stdout);
  fflush(stdout);
  return written;
}

void LogHr(const wchar_t* what, HRESULT hr) {
  Log(L"%-38ls hr=0x%08lX", what, static_cast<unsigned long>(hr));
}

// 免去 -Wcast-function-type：经由 memcpy 转换 FARPROC
template <typename Fn>
Fn LoadProc(const wchar_t* moduleName, const char* procName) {
  static_assert(sizeof(Fn) == sizeof(FARPROC), "unexpected function pointer size");
  FARPROC proc = GetProcAddress(GetModuleHandleW(moduleName), procName);
  Fn fn = nullptr;
  if (proc) std::memcpy(&fn, &proc, sizeof(fn));
  return fn;
}

bool SavePng(const std::wstring& path, UINT width, UINT height, const BYTE* pixels, UINT stride,
             REFGUID pixelFormat) {
  if (!g_app.wic) return false;

  ComPtr<IWICStream> stream;
  HRESULT hr = g_app.wic->CreateStream(stream.Put());
  if (FAILED(hr)) return false;
  hr = stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE);
  if (FAILED(hr)) return false;

  ComPtr<IWICBitmapEncoder> encoder;
  hr = g_app.wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, encoder.Put());
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
    Log(L"WIC 改写了像素格式，为避免花屏已中止保存");
    return false;
  }
  hr = frame->WritePixels(height, stride, stride * height, const_cast<BYTE*>(pixels));
  if (FAILED(hr)) return false;
  hr = frame->Commit();
  if (FAILED(hr)) return false;
  hr = encoder->Commit();
  if (FAILED(hr)) return false;
  return true;
}

bool InitD3D11(UINT width, UINT height, bool forSwapChain) {
  UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
  D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
  D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
  HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, levels,
                                 ARRAYSIZE(levels), D3D11_SDK_VERSION, g_app.d3d.Put(), &level,
                                 g_app.d3dContext.Put());
  if (FAILED(hr)) {
    LogHr(L"D3D11CreateDevice(hardware)", hr);
    hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, flags, levels, ARRAYSIZE(levels),
                           D3D11_SDK_VERSION, g_app.d3d.Put(), &level, g_app.d3dContext.Put());
    if (FAILED(hr)) {
      LogHr(L"D3D11CreateDevice(warp)", hr);
      return false;
    }
    Log(L"D3D11: WARP 软件设备");
  } else {
    Log(L"D3D11: 硬件设备, feature level 0x%X", static_cast<unsigned>(level));
  }

  ComPtr<IDXGIDevice> dxgiDevice;
  hr = g_app.d3d->QueryInterface(__uuidof(IDXGIDevice), dxgiDevice.PutVoid());
  if (FAILED(hr)) return false;

  hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1), nullptr,
                         g_app.d2dFactory.PutVoid());
  if (FAILED(hr)) {
    LogHr(L"D2D1CreateFactory", hr);
    return false;
  }
  hr = g_app.d2dFactory->CreateDevice(dxgiDevice.Get(), g_app.d2dDevice.Put());
  if (FAILED(hr)) {
    LogHr(L"ID2D1Factory1::CreateDevice", hr);
    return false;
  }
  hr = g_app.d2dDevice->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE,
                                            g_app.d2dContext.Put());
  if (FAILED(hr)) {
    LogHr(L"CreateDeviceContext", hr);
    return false;
  }

  if (!forSwapChain) {
    // 离屏目标：D3D11 纹理 + D2D 目标位图
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET;
    hr = g_app.d3d->CreateTexture2D(&desc, nullptr, g_app.offscreenTexture.Put());
    if (FAILED(hr)) {
      LogHr(L"CreateTexture2D(offscreen)", hr);
      return false;
    }
    ComPtr<IDXGISurface> surface;
    hr = g_app.offscreenTexture->QueryInterface(__uuidof(IDXGISurface), surface.PutVoid());
    if (FAILED(hr)) return false;
    D2D1_BITMAP_PROPERTIES1 props = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.0f, 96.0f);
    hr = g_app.d2dContext->CreateBitmapFromDxgiSurface(surface.Get(), &props, g_app.d2dTarget.Put());
    if (FAILED(hr)) {
      LogHr(L"CreateBitmapFromDxgiSurface(offscreen)", hr);
      return false;
    }
    g_app.d2dContext->SetTarget(g_app.d2dTarget.Get());
  }

  return true;
}

bool InitDWriteAndWic() {
  HRESULT hr = DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                   reinterpret_cast<IUnknown**>(g_app.dwrite.Put()));
  if (FAILED(hr)) {
    LogHr(L"DWriteCreateFactory", hr);
    return false;
  }
  hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                        __uuidof(IWICImagingFactory), g_app.wic.PutVoid());
  if (FAILED(hr)) {
    LogHr(L"CoCreateInstance(WIC)", hr);
    return false;
  }

  const wchar_t* family = L"Segoe UI Variable Text";
  struct FormatSpec {
    const wchar_t* name;
    float size;
    DWRITE_FONT_WEIGHT weight;
    ComPtr<IDWriteTextFormat>* out;
  };
  FormatSpec specs[] = {
      {family, 26.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, &g_app.queryFormat},
      {family, 14.0f, DWRITE_FONT_WEIGHT_NORMAL, &g_app.titleFormat},
      {family, 11.5f, DWRITE_FONT_WEIGHT_NORMAL, &g_app.subtitleFormat},
  };
  for (const auto& spec : specs) {
    hr = g_app.dwrite->CreateTextFormat(spec.name, nullptr, spec.weight, DWRITE_FONT_STYLE_NORMAL,
                                        DWRITE_FONT_STRETCH_NORMAL, spec.size, L"zh-CN",
                                        spec.out->Put());
    if (FAILED(hr)) {
      LogHr(L"CreateTextFormat", hr);
      return false;
    }
    (*spec.out)->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
  }
  return true;
}

bool InitComposition(HWND hwnd, UINT width, UINT height) {
  ComPtr<IDXGIDevice> dxgiDevice;
  HRESULT hr = g_app.d3d->QueryInterface(__uuidof(IDXGIDevice), dxgiDevice.PutVoid());
  if (FAILED(hr)) return false;

  ComPtr<IDXGIAdapter> adapter;
  hr = dxgiDevice->GetAdapter(adapter.Put());
  if (FAILED(hr)) return false;
  ComPtr<IDXGIFactory2> factory;
  hr = adapter->GetParent(__uuidof(IDXGIFactory2), factory.PutVoid());
  if (FAILED(hr)) return false;

  DXGI_SWAP_CHAIN_DESC1 desc{};
  desc.Width = width;
  desc.Height = height;
  desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
  desc.SampleDesc.Count = 1;
  desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
  desc.BufferCount = 2;
  desc.Scaling = DXGI_SCALING_STRETCH;
  desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
  desc.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
  hr = factory->CreateSwapChainForComposition(g_app.d3d.Get(), &desc, nullptr,
                                              g_app.swapChain.Put());
  if (FAILED(hr)) {
    LogHr(L"CreateSwapChainForComposition", hr);
    return false;
  }

  hr = DCompositionCreateDevice(dxgiDevice.Get(), __uuidof(IDCompositionDevice),
                                g_app.compDevice.PutVoid());
  if (FAILED(hr)) {
    LogHr(L"DCompositionCreateDevice", hr);
    return false;
  }
  hr = g_app.compDevice->CreateTargetForHwnd(hwnd, TRUE, g_app.compTarget.Put());
  if (FAILED(hr)) {
    LogHr(L"CreateTargetForHwnd", hr);
    return false;
  }
  hr = g_app.compDevice->CreateVisual(g_app.compVisual.Put());
  if (FAILED(hr)) {
    LogHr(L"CreateVisual", hr);
    return false;
  }
  hr = g_app.compVisual->SetContent(g_app.swapChain.Get());
  if (FAILED(hr)) {
    LogHr(L"SetContent", hr);
    return false;
  }
  hr = g_app.compTarget->SetRoot(g_app.compVisual.Get());
  if (FAILED(hr)) return false;
  hr = g_app.compDevice->Commit();
  if (FAILED(hr)) return false;

  ComPtr<IDXGISurface> surface;
  hr = g_app.swapChain->GetBuffer(0, __uuidof(IDXGISurface), surface.PutVoid());
  if (FAILED(hr)) {
    LogHr(L"GetBuffer(0)", hr);
    return false;
  }
  D2D1_BITMAP_PROPERTIES1 props = D2D1::BitmapProperties1(
      D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
      D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.0f, 96.0f);
  hr = g_app.d2dContext->CreateBitmapFromDxgiSurface(surface.Get(), &props, g_app.d2dTarget.Put());
  if (FAILED(hr)) {
    LogHr(L"CreateBitmapFromDxgiSurface(swapchain)", hr);
    return false;
  }
  g_app.d2dContext->SetTarget(g_app.d2dTarget.Get());
  return true;
}

D2D1_COLOR_F Rgba(UINT32 rgb, float alpha) {
  return D2D1::ColorF(static_cast<float>((rgb >> 16) & 0xFF) / 255.0f,
                      static_cast<float>((rgb >> 8) & 0xFF) / 255.0f,
                      static_cast<float>(rgb & 0xFF) / 255.0f, alpha);
}

struct RowSample {
  const wchar_t* title;
  const wchar_t* subtitle;
  UINT32 tileColor;
  const wchar_t* glyph;
};

const RowSample kRows[] = {
    {L"微信", L"C:\\Program Files\\Tencent\\WeChat\\WeChat.exe", 0x14C463, L"微"},
    {L"Visual Studio Code", L"C:\\Users\\13152\\AppData\\Local\\Programs\\VSCode\\Code.exe", 0x2F80ED, L"V"},
    {L"下载", L"C:\\Users\\13152\\Downloads", 0x8B5CF6, L"下"},
};

void DrawScene(ID2D1DeviceContext* dc, float scale, bool fakeWallpaper) {
  const float alphaScale = g_app.options.glass ? 0.35f : 0.88f;
  dc->BeginDraw();
  dc->SetTransform(D2D1::Matrix3x2F::Scale(scale, scale));
  dc->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
  dc->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
  dc->Clear(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f));

  ComPtr<ID2D1SolidColorBrush> brush;
  HRESULT hr = dc->CreateSolidColorBrush(Rgba(0xFFFFFF, 1.0f), brush.Put());
  if (FAILED(hr)) return;

  if (fakeWallpaper) {
    // 模拟桌面壁纸，让离屏图与真实合成结果可比
    D2D1_GRADIENT_STOP stops[3] = {
        {0.0f, Rgba(0x2A3550, 1.0f)},
        {0.55f, Rgba(0x3A2B4E, 1.0f)},
        {1.0f, Rgba(0x1B2438, 1.0f)},
    };
    ComPtr<ID2D1GradientStopCollection> collection;
    dc->CreateGradientStopCollection(stops, 3, collection.Put());
    D2D1_LINEAR_GRADIENT_BRUSH_PROPERTIES props{};
    props.startPoint = D2D1::Point2F(0.0f, 0.0f);
    props.endPoint = D2D1::Point2F(kPanelWidth, kPanelHeight);
    ComPtr<ID2D1LinearGradientBrush> gradient;
    if (collection && SUCCEEDED(dc->CreateLinearGradientBrush(props, collection.Get(), gradient.Put()))) {
      dc->FillRectangle(D2D1::RectF(0, 0, kPanelWidth, kPanelHeight), gradient.Get());
    }
    // 几个柔光圆点，模拟壁纸内容
    ComPtr<ID2D1RadialGradientBrush> glow;
    D2D1_GRADIENT_STOP glowStops[2] = {{0.0f, Rgba(0x7FB2FF, 0.55f)}, {1.0f, Rgba(0x7FB2FF, 0.0f)}};
    ComPtr<ID2D1GradientStopCollection> glowCollection;
    dc->CreateGradientStopCollection(glowStops, 2, glowCollection.Put());
    D2D1_RADIAL_GRADIENT_BRUSH_PROPERTIES glowProps{};
    glowProps.center = D2D1::Point2F(140.0f, 40.0f);
    glowProps.radiusX = 260.0f;
    glowProps.radiusY = 200.0f;
    if (glowCollection && SUCCEEDED(dc->CreateRadialGradientBrush(glowProps, glowCollection.Get(), glow.Put()))) {
      dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(140.0f, 40.0f), 260.0f, 200.0f), glow.Get());
    }
  }

  // 面板本体：圆角 + 半透明底 + 1px 内描边
  const D2D1_ROUNDED_RECT panel{D2D1::RectF(0.5f, 0.5f, kPanelWidth - 0.5f, kPanelHeight - 0.5f),
                                16.0f, 16.0f};
  brush->SetColor(Rgba(0x1B1B1F, alphaScale));
  dc->FillRoundedRectangle(panel, brush.Get());
  brush->SetColor(Rgba(0xFFFFFF, 0.10f));
  dc->DrawRoundedRectangle(panel, brush.Get(), 1.0f);

  // 顶部弱高光：覆盖整个面板并按 96px 淡出，避免出现"内层圆角卡片"的接缝
  D2D1_GRADIENT_STOP topStops[3] = {{0.0f, Rgba(0xFFFFFF, 0.07f)},
                                    {96.0f / kPanelHeight, Rgba(0xFFFFFF, 0.0f)},
                                    {1.0f, Rgba(0xFFFFFF, 0.0f)}};
  ComPtr<ID2D1GradientStopCollection> topCollection;
  dc->CreateGradientStopCollection(topStops, 3, topCollection.Put());
  D2D1_LINEAR_GRADIENT_BRUSH_PROPERTIES topProps{};
  topProps.startPoint = D2D1::Point2F(0.0f, 0.0f);
  topProps.endPoint = D2D1::Point2F(0.0f, kPanelHeight);
  ComPtr<ID2D1LinearGradientBrush> topBrush;
  if (topCollection && SUCCEEDED(dc->CreateLinearGradientBrush(topProps, topCollection.Get(), topBrush.Put()))) {
    dc->FillRoundedRectangle(panel, topBrush.Get());
  }

  // 搜索图标：圆 + 手柄
  brush->SetColor(Rgba(0xFFFFFF, 0.55f));
  dc->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(35.0f, 35.0f), 7.0f, 7.0f), brush.Get(), 1.8f);
  dc->DrawLine(D2D1::Point2F(40.0f, 40.0f), D2D1::Point2F(46.0f, 46.0f), brush.Get(), 1.8f);

  // 输入框文字与光标
  const std::wstring& query = g_app.options.query;
  const bool empty = query.empty();
  const wchar_t* text = empty ? L"搜索应用与文件" : query.c_str();
  ComPtr<IDWriteTextLayout> layout;
  if (SUCCEEDED(g_app.dwrite->CreateTextLayout(text, static_cast<UINT32>(wcslen(text)),
                                               g_app.queryFormat.Get(), 560.0f, 36.0f,
                                               layout.Put()))) {
    brush->SetColor(Rgba(0xFFFFFF, empty ? 0.36f : 0.95f));
    dc->DrawTextLayout(D2D1::Point2F(64.0f, 17.0f), layout.Get(), brush.Get());
    DWRITE_TEXT_METRICS metrics{};
    layout->GetMetrics(&metrics);
    if (!empty) {
      const float caretX = 64.0f + metrics.widthIncludingTrailingWhitespace;
      brush->SetColor(Rgba(0x6EA8FF, 1.0f));
      dc->FillRectangle(D2D1::RectF(caretX + 1.0f, 21.0f, caretX + 2.6f, 49.0f), brush.Get());
    }
  }

  // 分隔线
  brush->SetColor(Rgba(0xFFFFFF, 0.07f));
  dc->FillRectangle(D2D1::RectF(20.0f, 72.0f, kPanelWidth - 20.0f, 73.0f), brush.Get());

  // 结果列表
  const float rowTop = 84.0f;
  const float rowHeight = 44.0f;
  const float rowGap = 4.0f;
  for (int i = 0; i < 3; ++i) {
    const float y = rowTop + i * (rowHeight + rowGap);
    const bool selected = (i == 0);
    D2D1_ROUNDED_RECT rowRect{D2D1::RectF(12.0f, y, kPanelWidth - 12.0f, y + rowHeight), 10.0f, 10.0f};
    if (selected) {
      brush->SetColor(Rgba(0x6EA8FF, g_app.options.glass ? 0.20f : 0.16f));
      dc->FillRoundedRectangle(rowRect, brush.Get());
      brush->SetColor(Rgba(0x6EA8FF, 0.95f));
      dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(20.0f, y + 12.0f, 23.0f, y + 32.0f), 1.5f, 1.5f),
                               brush.Get());
    }

    // 图标底座
    D2D1_ROUNDED_RECT tile{D2D1::RectF(36.0f, y + 8.0f, 64.0f, y + 36.0f), 7.0f, 7.0f};
    brush->SetColor(Rgba(kRows[i].tileColor, 0.95f));
    dc->FillRoundedRectangle(tile, brush.Get());

    ComPtr<IDWriteTextLayout> glyphLayout;
    if (SUCCEEDED(g_app.dwrite->CreateTextLayout(kRows[i].glyph, 1, g_app.titleFormat.Get(), 28.0f, 28.0f,
                                                 glyphLayout.Put()))) {
      glyphLayout->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
      glyphLayout->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
      brush->SetColor(Rgba(0xFFFFFF, 0.96f));
      dc->DrawTextLayout(D2D1::Point2F(36.0f, y + 8.0f), glyphLayout.Get(), brush.Get());
    }

    ComPtr<IDWriteTextLayout> titleLayout;
    if (SUCCEEDED(g_app.dwrite->CreateTextLayout(kRows[i].title, static_cast<UINT32>(wcslen(kRows[i].title)),
                                                 g_app.titleFormat.Get(), 400.0f, 18.0f, titleLayout.Put()))) {
      brush->SetColor(Rgba(0xFFFFFF, 0.95f));
      dc->DrawTextLayout(D2D1::Point2F(76.0f, y + 7.0f), titleLayout.Get(), brush.Get());
    }
    ComPtr<IDWriteTextLayout> subtitleLayout;
    if (SUCCEEDED(g_app.dwrite->CreateTextLayout(kRows[i].subtitle,
                                                 static_cast<UINT32>(wcslen(kRows[i].subtitle)),
                                                 g_app.subtitleFormat.Get(), 600.0f, 16.0f,
                                                 subtitleLayout.Put()))) {
      brush->SetColor(Rgba(0xFFFFFF, 0.50f));
      dc->DrawTextLayout(D2D1::Point2F(76.0f, y + 24.0f), subtitleLayout.Get(), brush.Get());
    }
  }

  dc->SetTransform(D2D1::Matrix3x2F::Identity());
  HRESULT endHr = dc->EndDraw();
  if (FAILED(endHr)) LogHr(L"ID2D1DeviceContext::EndDraw", endHr);
}

bool RenderOffscreenShot(const Options& options) {
  const UINT width = static_cast<UINT>(std::lround(kPanelWidth * options.scale));
  const UINT height = static_cast<UINT>(std::lround(kPanelHeight * options.scale));
  if (!InitD3D11(width, height, false)) {
    Log(L"InitD3D11 失败");
    return false;
  }
  if (!InitDWriteAndWic()) {
    Log(L"InitDWriteAndWic 失败");
    return false;
  }

  g_app.options = options;
  DrawScene(g_app.d2dContext.Get(), options.scale, true);
  g_app.d2dContext->SetTarget(nullptr);
  g_app.d2dContext->Flush();

  // 回读
  if (!g_app.offscreenTexture) {
    Log(L"离屏纹理缺失");
    return false;
  }
  D3D11_TEXTURE2D_DESC desc{};
  g_app.offscreenTexture->GetDesc(&desc);
  desc.BindFlags = 0;
  desc.Usage = D3D11_USAGE_STAGING;
  desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  desc.MiscFlags = 0;
  ComPtr<ID3D11Texture2D> staging;
  HRESULT hr = g_app.d3d->CreateTexture2D(&desc, nullptr, staging.Put());
  if (FAILED(hr)) {
    LogHr(L"CreateTexture2D(staging)", hr);
    return false;
  }
  g_app.d3dContext->CopyResource(staging.Get(), g_app.offscreenTexture.Get());
  D3D11_MAPPED_SUBRESOURCE mapped{};
  hr = g_app.d3dContext->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped);
  if (FAILED(hr)) {
    LogHr(L"Map(staging)", hr);
    return false;
  }
  bool ok = SavePng(options.shot, width, height, static_cast<const BYTE*>(mapped.pData), mapped.RowPitch,
                    GUID_WICPixelFormat32bppBGRA);
  g_app.d3dContext->Unmap(staging.Get(), 0);
  if (ok) Log(L"离屏渲染已保存: %ls (%ux%u)", options.shot.c_str(), width, height);
  return ok;
}

bool CaptureWindowPng(const std::wstring& path) {
  RECT rect{};
  if (!GetWindowRect(g_app.hwnd, &rect)) return false;
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
      // DIB 的 alpha 字节由 GDI 置 0；截图需要不透明
      auto* pixels = static_cast<DWORD*>(bits);
      const size_t count = static_cast<size_t>(width) * static_cast<size_t>(height);
      for (size_t i = 0; i < count; ++i) pixels[i] |= 0xFF000000u;
      ok = SavePng(path, static_cast<UINT>(width), static_cast<UINT>(height), static_cast<const BYTE*>(bits),
                   static_cast<UINT>(width * 4), GUID_WICPixelFormat32bppBGRA);
    }
    SelectObject(memory, old);
  }
  if (bitmap) DeleteObject(bitmap);
  DeleteDC(memory);
  ReleaseDC(nullptr, screen);
  if (ok) Log(L"窗口合成截图已保存: %ls (%dx%d)", path.c_str(), width, height);
  return ok;
}

LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
  switch (message) {
    case WM_PAINT: {
      PAINTSTRUCT ps;
      BeginPaint(hwnd, &ps);
      if (g_app.d2dContext && g_app.d2dTarget) {
        DrawScene(g_app.d2dContext.Get(), g_app.actualScale, false);
        if (g_app.swapChain) {
          HRESULT presentHr = g_app.swapChain->Present(1, 0);
          if (FAILED(presentHr)) LogHr(L"Present", presentHr);
        }
      }
      EndPaint(hwnd, &ps);
      return 0;
    }
    case WM_TIMER:
      Log(L"WM_TIMER %llu", static_cast<unsigned long long>(wParam));
      if (wParam == kTimerCapture) {
        KillTimer(hwnd, kTimerCapture);
        if (!g_app.options.capture.empty()) {
          if (!CaptureWindowPng(g_app.options.capture)) Log(L"截图失败");
        }
        PostMessageW(hwnd, WM_CLOSE, 0, 0);
        return 0;
      }
      if (wParam == kTimerExit) {
        KillTimer(hwnd, kTimerExit);
        PostMessageW(hwnd, WM_CLOSE, 0, 0);
        return 0;
      }
      return 0;
    case WM_CLOSE:
      if (hwnd == g_app.hwnd) Log(L"WM_CLOSE");
      return DefWindowProcW(hwnd, message, wParam, lParam);
    case WM_DESTROY:
      if (hwnd == g_app.hwnd) {
        Log(L"WM_DESTROY");
        PostQuitMessage(0);
      }
      return 0;
    default:
      return DefWindowProcW(hwnd, message, wParam, lParam);
  }
}

bool EnableDpiAwareness() {
  using SetDpiAwarenessFn = BOOL(WINAPI*)(HANDLE);
  auto fn = LoadProc<SetDpiAwarenessFn>(L"user32.dll", "SetProcessDpiAwarenessContext");
  if (fn && fn(reinterpret_cast<HANDLE>(-4))) return true;  // PER_MONITOR_AWARE_V2
  return SetProcessDPIAware() != FALSE;
}

int RunWindow(const Options& options) {
  if (!EnableDpiAwareness()) Log(L"DPI 感知设置失败（继续）");

  WNDCLASSEXW wc{};
  wc.cbSize = sizeof(wc);
  wc.lpfnWndProc = WindowProc;
  wc.hInstance = GetModuleHandleW(nullptr);
  wc.lpszClassName = kWindowClass;
  wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  if (!RegisterClassExW(&wc)) {
    Log(L"RegisterClassExW 失败: %lu", GetLastError());
    return 2;
  }

  g_app.options = options;
  // 用系统 DPI 计算初始像素尺寸（避免创建临时窗口——其 WM_DESTROY 会误投 WM_QUIT）
  using GetDpiForSystemFn = UINT(WINAPI*)();
  auto getDpiForSystem = LoadProc<GetDpiForSystemFn>(L"user32.dll", "GetDpiForSystem");
  const UINT dpi = getDpiForSystem ? getDpiForSystem() : 96;

  g_app.actualScale = (options.scale != 1.0f) ? options.scale : static_cast<float>(dpi) / 96.0f;
  const int widthPx = static_cast<int>(std::lround(kPanelWidth * g_app.actualScale));
  const int heightPx = static_cast<int>(std::lround(kPanelHeight * g_app.actualScale));

  const int screenW = GetSystemMetrics(SM_CXSCREEN);
  const int screenH = GetSystemMetrics(SM_CYSCREEN);
  const int x = (screenW - widthPx) / 2;
  const int y = static_cast<int>(screenH * 0.18);
  Log(L"窗口: %dx%d (scale=%.2f, dpi=%u)", widthPx, heightPx, g_app.actualScale, dpi);

  HWND hwnd = CreateWindowExW(
      WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOREDIRECTIONBITMAP, kWindowClass, L"eTools", WS_POPUP,
      x, y, widthPx, heightPx, nullptr, nullptr, wc.hInstance, nullptr);
  if (!hwnd) {
    Log(L"CreateWindowExW 失败: %lu", GetLastError());
    return 2;
  }
  g_app.hwnd = hwnd;

  BOOL dark = TRUE;
  LogHr(L"DwmSetWindowAttribute(dark)",
        DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark)));
  if (options.mica) {
    int backdrop = DWMSBT_MAINWINDOW;
    LogHr(L"DwmSetWindowAttribute(mica)",
          DwmSetWindowAttribute(hwnd, DWMWA_SYSTEMBACKDROP_TYPE, &backdrop, sizeof(backdrop)));
  }
  const int corner = DWMWCP_ROUND;
  LogHr(L"DwmSetWindowAttribute(corner)",
        DwmSetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &corner, sizeof(corner)));

  if (!InitD3D11(static_cast<UINT>(widthPx), static_cast<UINT>(heightPx), true)) {
    Log(L"InitD3D11 失败");
    return 2;
  }
  if (!InitDWriteAndWic()) {
    Log(L"InitDWriteAndWic 失败");
    return 2;
  }
  if (!InitComposition(hwnd, static_cast<UINT>(widthPx), static_cast<UINT>(heightPx))) {
    Log(L"InitComposition 失败");
    return 2;
  }

  ShowWindow(hwnd, SW_SHOW);
  SetForegroundWindow(hwnd);
  UpdateWindow(hwnd);

  if (!options.capture.empty()) {
    UINT_PTR timerId = SetTimer(hwnd, kTimerCapture, 450, nullptr);
    Log(L"capture timer id=%llu", static_cast<unsigned long long>(timerId));
  }
  if (options.autoExitMs > 0 && options.capture.empty()) {
    SetTimer(hwnd, kTimerExit, static_cast<UINT>(options.autoExitMs), nullptr);
  }

  MSG msg{};
  BOOL getResult = 0;
  while ((getResult = GetMessageW(&msg, nullptr, 0, 0)) > 0) {
    TranslateMessage(&msg);
    DispatchMessageW(&msg);
  }
  Log(L"消息循环结束 get=%d msg=0x%04X err=%lu", getResult, msg.message, GetLastError());
  if (g_app.d2dContext) g_app.d2dContext->SetTarget(nullptr);
  return 0;
}

Options ParseOptions(int argc, wchar_t** argv) {
  Options options;
  for (int i = 1; i < argc; ++i) {
    std::wstring arg = argv[i];
    auto value = [&](const wchar_t* prefix) -> std::wstring {
      const size_t len = wcslen(prefix);
      if (arg.rfind(prefix, 0) == 0) return arg.substr(len);
      return {};
    };
    if (arg.rfind(L"--shot=", 0) == 0) {
      options.shot = value(L"--shot=");
    } else if (arg.rfind(L"--capture=", 0) == 0) {
      options.capture = value(L"--capture=");
    } else if (arg.rfind(L"--query=", 0) == 0) {
      options.query = value(L"--query=");
    } else if (arg.rfind(L"--scale=", 0) == 0) {
      options.scale = static_cast<float>(wcstod(value(L"--scale=").c_str(), nullptr));
      if (options.scale < 0.5f || options.scale > 4.0f) options.scale = 1.0f;
    } else if (arg.rfind(L"--ms=", 0) == 0) {
      options.autoExitMs = static_cast<int>(wcstol(value(L"--ms=").c_str(), nullptr, 10));
    } else if (arg == L"--glass") {
      options.glass = true;
    } else if (arg == L"--mica") {
      options.mica = true;
    }
  }
  return options;
}

// 在 CoUninitialize 之前释放 COM 对象，避免静态析构阶段访问已反初始化的 COM。
void ReleaseAll() {
  g_app.subtitleFormat.Reset();
  g_app.titleFormat.Reset();
  g_app.queryFormat.Reset();
  g_app.dwrite.Reset();
  g_app.wic.Reset();
  g_app.d2dTarget.Reset();
  g_app.d2dContext.Reset();
  g_app.d2dDevice.Reset();
  g_app.d2dFactory.Reset();
  g_app.compVisual.Reset();
  g_app.compTarget.Reset();
  g_app.compDevice.Reset();
  g_app.swapChain.Reset();
  g_app.offscreenTexture.Reset();
  g_app.d3dContext.Reset();
  g_app.d3d.Reset();
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
  SetConsoleOutputCP(CP_UTF8);
  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  Options options = ParseOptions(argc, argv);
  int exitCode = 0;
  if (!options.shot.empty()) {
    if (!RenderOffscreenShot(options)) {
      Log(L"离屏渲染失败");
      exitCode = 2;
    }
  } else {
    exitCode = RunWindow(options);
  }
  ReleaseAll();
  CoUninitialize();
  return exitCode;
}
