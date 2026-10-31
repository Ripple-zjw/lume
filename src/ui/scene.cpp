#include "ui/scene.h"

#include "core/log.h"
#include "ui/renderer.h"

namespace {

struct RowSample {
  const wchar_t* title;
  const wchar_t* subtitle;
  UINT32 tileColor;
  const wchar_t* glyph;
};

const RowSample kRows[] = {
    {L"微信", L"C:\\Program Files\\Tencent\\WeChat\\WeChat.exe", 0x14C463, L"微"},
    {L"Visual Studio Code", L"C:\\Users\\13152\\AppData\\Local\\Programs\\VSCode\\Code.exe", 0x2F80ED,
     L"V"},
    {L"下载", L"C:\\Users\\13152\\Downloads", 0x8B5CF6, L"下"},
};
constexpr int kRowCount = ARRAYSIZE(kRows);

D2D1_COLOR_F Rgba(UINT32 rgb, float alpha) {
  return D2D1::ColorF(static_cast<float>((rgb >> 16) & 0xFF) / 255.0f,
                      static_cast<float>((rgb >> 8) & 0xFF) / 255.0f,
                      static_cast<float>(rgb & 0xFF) / 255.0f, alpha);
}

D2D1_COLOR_F WithAlpha(D2D1_COLOR_F color, float alpha) {
  color.a = alpha;
  return color;
}

bool CreateFormat(IDWriteFactory* factory, const wchar_t* family, float size,
                  DWRITE_FONT_WEIGHT weight, ComPtr<IDWriteTextFormat>* out) {
  HRESULT hr = factory->CreateTextFormat(family, nullptr, weight, DWRITE_FONT_STYLE_NORMAL,
                                         DWRITE_FONT_STRETCH_NORMAL, size, L"zh-CN", out->Put());
  if (FAILED(hr)) {
    LogHr(L"CreateTextFormat", hr);
    return false;
  }
  (*out)->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
  return true;
}

}  // namespace

bool Scene::Init(Renderer& renderer, const Theme& theme) {
  renderer_ = &renderer;
  IDWriteFactory* factory = renderer.dwrite();
  if (!CreateFormat(factory, theme.fontFamily, theme.queryFontSize, DWRITE_FONT_WEIGHT_SEMI_BOLD,
                    &queryFormat_)) {
    return false;
  }
  if (!CreateFormat(factory, theme.fontFamily, theme.titleFontSize, DWRITE_FONT_WEIGHT_NORMAL,
                    &titleFormat_)) {
    return false;
  }
  if (!CreateFormat(factory, theme.fontFamily, theme.subtitleFontSize, DWRITE_FONT_WEIGHT_NORMAL,
                    &subtitleFormat_)) {
    return false;
  }
  if (!CreateFormat(factory, theme.fontFamily, theme.glyphFontSize, DWRITE_FONT_WEIGHT_SEMI_BOLD,
                    &glyphFormat_)) {
    return false;
  }
  glyphFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
  glyphFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);

  HRESULT hr = renderer.context()->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 1), brush_.Put());
  if (FAILED(hr)) {
    LogHr(L"CreateSolidColorBrush", hr);
    return false;
  }
  return true;
}

void Scene::DrawRows(ID2D1DeviceContext* ctx, const Theme& theme, const SceneState& state,
                     D2D1_POINT_2F panel, float panelHeight) {
  const int rows = state.rows < 1 ? 1 : (state.rows > kRowCount ? kRowCount : state.rows);
  for (int i = 0; i < rows; ++i) {
    const float y = panel.y + theme.listTop + i * (theme.rowHeight + theme.rowGap);
    const bool selected = (i == state.selected);
    const float left = panel.x + 12.0f;
    const float right = panel.x + theme.panelWidth - 12.0f;

    if (selected) {
      brush_->SetColor(theme.selectedFill);
      ctx->FillRoundedRectangle(
          D2D1::RoundedRect(D2D1::RectF(left, y, right, y + theme.rowHeight), theme.rowCorner,
                            theme.rowCorner),
          brush_.Get());
      brush_->SetColor(theme.accent);
      ctx->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(panel.x + 20.0f, y + 12.0f,
                                                             panel.x + 23.0f, y + 32.0f),
                                                  1.5f, 1.5f),
                                brush_.Get());
    }

    // 图标底座（P3 换成真实 shell 图标）
    brush_->SetColor(Rgba(kRows[i].tileColor, 0.95f));
    ctx->FillRoundedRectangle(
        D2D1::RoundedRect(D2D1::RectF(panel.x + 36.0f, y + 8.0f, panel.x + 64.0f, y + 36.0f),
                          theme.tileCorner, theme.tileCorner),
        brush_.Get());

    ComPtr<IDWriteTextLayout> glyph;
    if (SUCCEEDED(renderer_->dwrite()->CreateTextLayout(
            kRows[i].glyph, 1, glyphFormat_.Get(), 28.0f, 28.0f, glyph.Put()))) {
      brush_->SetColor(theme.tileText);
      ctx->DrawTextLayout(D2D1::Point2F(panel.x + 36.0f, y + 8.0f), glyph.Get(), brush_.Get());
    }

    ComPtr<IDWriteTextLayout> title;
    if (SUCCEEDED(renderer_->dwrite()->CreateTextLayout(
            kRows[i].title, static_cast<UINT32>(wcslen(kRows[i].title)), titleFormat_.Get(), 420.0f,
            18.0f, title.Put()))) {
      brush_->SetColor(theme.textPrimary);
      ctx->DrawTextLayout(D2D1::Point2F(panel.x + 76.0f, y + 7.0f), title.Get(), brush_.Get());
    }
    ComPtr<IDWriteTextLayout> subtitle;
    if (SUCCEEDED(renderer_->dwrite()->CreateTextLayout(
            kRows[i].subtitle, static_cast<UINT32>(wcslen(kRows[i].subtitle)),
            subtitleFormat_.Get(), 600.0f, 16.0f, subtitle.Put()))) {
      brush_->SetColor(theme.textSecondary);
      ctx->DrawTextLayout(D2D1::Point2F(panel.x + 76.0f, y + 24.0f), subtitle.Get(), brush_.Get());
    }
  }
  (void)panelHeight;
}

void Scene::Draw(const Theme& theme, const SceneState& state, bool fakeWallpaper, float scale) {
  ID2D1DeviceContext* ctx = renderer_->context();
  const float panelHeight = PanelHeight(theme, 3);
  const float canvasWidth = theme.panelWidth + theme.shadowMargin * 2.0f;
  const float canvasHeight = panelHeight + theme.shadowMargin * 2.0f;
  const D2D1_POINT_2F panel = D2D1::Point2F(theme.shadowMargin, theme.shadowMargin);

  ctx->SetTransform(D2D1::Matrix3x2F::Scale(scale, scale));
  ctx->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
  ctx->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);

  if (fakeWallpaper) {
    D2D1_GRADIENT_STOP stops[3] = {
        {0.0f, Rgba(0x2A3550, 1.0f)},
        {0.55f, Rgba(0x3A2B4E, 1.0f)},
        {1.0f, Rgba(0x1B2438, 1.0f)},
    };
    ComPtr<ID2D1GradientStopCollection> collection;
    if (SUCCEEDED(ctx->CreateGradientStopCollection(stops, 3, collection.Put()))) {
      D2D1_LINEAR_GRADIENT_BRUSH_PROPERTIES props{};
      props.startPoint = D2D1::Point2F(0.0f, 0.0f);
      props.endPoint = D2D1::Point2F(canvasWidth, canvasHeight);
      ComPtr<ID2D1LinearGradientBrush> gradient;
      if (SUCCEEDED(ctx->CreateLinearGradientBrush(props, collection.Get(), gradient.Put()))) {
        ctx->FillRectangle(D2D1::RectF(0, 0, canvasWidth, canvasHeight), gradient.Get());
      }
    }
    D2D1_GRADIENT_STOP glowStops[2] = {{0.0f, Rgba(0x7FB2FF, 0.45f)},
                                       {1.0f, Rgba(0x7FB2FF, 0.0f)}};
    ComPtr<ID2D1GradientStopCollection> glowCollection;
    if (SUCCEEDED(ctx->CreateGradientStopCollection(glowStops, 2, glowCollection.Put()))) {
      D2D1_RADIAL_GRADIENT_BRUSH_PROPERTIES glowProps{};
      glowProps.center = D2D1::Point2F(panel.x + 120.0f, panel.y + 20.0f);
      glowProps.radiusX = 260.0f;
      glowProps.radiusY = 200.0f;
      ComPtr<ID2D1RadialGradientBrush> glow;
      if (SUCCEEDED(ctx->CreateRadialGradientBrush(glowProps, glowCollection.Get(), glow.Put()))) {
        ctx->FillEllipse(D2D1::Ellipse(glowProps.center, 260.0f, 200.0f), glow.Get());
      }
    }
  }

  // 分层柔和阴影（P2 视效果决定是否换 GaussianBlur 效果）
  const D2D1_ROUNDED_RECT panelRect{
      D2D1::RectF(panel.x, panel.y, panel.x + theme.panelWidth, panel.y + panelHeight),
      theme.corner, theme.corner};
  for (int layer = 6; layer >= 1; --layer) {
    const float spread = static_cast<float>(layer) * 2.2f;
    brush_->SetColor(WithAlpha(theme.shadow, theme.shadow.a * 0.06f));
    ctx->FillRoundedRectangle(
        D2D1::RoundedRect(
            D2D1::RectF(panelRect.rect.left - spread, panelRect.rect.top - spread,
                        panelRect.rect.right + spread, panelRect.rect.bottom + spread + 2.0f),
            theme.corner + spread, theme.corner + spread),
        brush_.Get());
  }

  // 面板本体（--glass 时降低不透明度，用于真实桌面合成检查）
  D2D1_COLOR_F panelColor = theme.panel;
  if (state.glass) panelColor.a = 0.35f;
  brush_->SetColor(panelColor);
  ctx->FillRoundedRectangle(panelRect, brush_.Get());
  brush_->SetColor(theme.border);
  ctx->DrawRoundedRectangle(panelRect, brush_.Get(), 1.0f);

  // 顶部柔光：覆盖整个面板并按 96px 淡出
  D2D1_GRADIENT_STOP topStops[3] = {{0.0f, theme.glow},
                                    {96.0f / panelHeight, WithAlpha(theme.glow, 0.0f)},
                                    {1.0f, WithAlpha(theme.glow, 0.0f)}};
  ComPtr<ID2D1GradientStopCollection> topCollection;
  if (SUCCEEDED(ctx->CreateGradientStopCollection(topStops, 3, topCollection.Put()))) {
    D2D1_LINEAR_GRADIENT_BRUSH_PROPERTIES topProps{};
    topProps.startPoint = D2D1::Point2F(0.0f, panel.y);
    topProps.endPoint = D2D1::Point2F(0.0f, panel.y + panelHeight);
    ComPtr<ID2D1LinearGradientBrush> topBrush;
    if (SUCCEEDED(ctx->CreateLinearGradientBrush(topProps, topCollection.Get(), topBrush.Put()))) {
      ctx->FillRoundedRectangle(panelRect, topBrush.Get());
    }
  }

  // 搜索图标
  brush_->SetColor(WithAlpha(theme.textSecondary, 0.85f));
  ctx->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(panel.x + 35.0f, panel.y + 35.0f), 7.0f, 7.0f),
                   brush_.Get(), 1.8f);
  ctx->DrawLine(D2D1::Point2F(panel.x + 40.0f, panel.y + 40.0f),
                D2D1::Point2F(panel.x + 46.0f, panel.y + 46.0f), brush_.Get(), 1.8f);

  // 输入文字与光标
  const bool empty = state.query.empty();
  const wchar_t* text = empty ? L"搜索应用与文件" : state.query.c_str();
  ComPtr<IDWriteTextLayout> queryLayout;
  if (SUCCEEDED(renderer_->dwrite()->CreateTextLayout(
          text, static_cast<UINT32>(wcslen(text)), queryFormat_.Get(), 560.0f, 36.0f,
          queryLayout.Put()))) {
    brush_->SetColor(empty ? theme.textPlaceholder : theme.textPrimary);
    ctx->DrawTextLayout(D2D1::Point2F(panel.x + 64.0f, panel.y + 17.0f), queryLayout.Get(),
                        brush_.Get());
    DWRITE_TEXT_METRICS metrics{};
    queryLayout->GetMetrics(&metrics);
    if (!empty) {
      const float caretX = panel.x + 64.0f + metrics.widthIncludingTrailingWhitespace;
      brush_->SetColor(theme.accent);
      ctx->FillRectangle(
          D2D1::RectF(caretX + 1.0f, panel.y + 21.0f, caretX + 2.6f, panel.y + 49.0f),
          brush_.Get());
    }
  }

  // 分隔线
  brush_->SetColor(theme.divider);
  ctx->FillRectangle(
      D2D1::RectF(panel.x + 20.0f, panel.y + 72.0f, panel.x + theme.panelWidth - 20.0f,
                  panel.y + 73.0f),
      brush_.Get());

  DrawRows(ctx, theme, state, panel, panelHeight);

  ctx->SetTransform(D2D1::Matrix3x2F::Identity());
  (void)canvasHeight;
}
