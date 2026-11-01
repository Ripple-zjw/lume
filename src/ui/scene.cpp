#include "ui/scene.h"

#include <algorithm>
#include <cmath>

#include "core/log.h"
#include "ui/icon_cache.h"
#include "ui/renderer.h"

namespace {

constexpr float kInputLeft = 64.0f;
constexpr float kInputTop = 17.0f;
constexpr float kInputMaxWidth = 628.0f;
constexpr float kCaretTop = 22.0f;
constexpr float kCaretHeight = 28.0f;
// 行高亮/命中区域左右内缩（与选中态圆角块一致）。
constexpr float kRowHitLeft = 12.0f;

D2D1_COLOR_F Rgba(UINT32 rgb, float alpha) {
  return D2D1::ColorF(static_cast<float>((rgb >> 16) & 0xFF) / 255.0f,
                      static_cast<float>((rgb >> 8) & 0xFF) / 255.0f,
                      static_cast<float>(rgb & 0xFF) / 255.0f, alpha);
}

D2D1_COLOR_F WithAlpha(D2D1_COLOR_F color, float alpha) {
  color.a = alpha;
  return color;
}

D2D1_COLOR_F ScaleAlpha(D2D1_COLOR_F color, float factor) {
  color.a *= factor;
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

bool Scene::Init(Renderer& renderer, const Theme& theme, IconCache* icons) {
  renderer_ = &renderer;
  icons_ = icons;
  theme_ = theme;
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
  if (!CreateFormat(factory, theme.fontFamily, theme.queryFontSize, DWRITE_FONT_WEIGHT_SEMI_BOLD,
                    &placeholderFormat_)) {
    return false;
  }

  if (!CreateFormat(factory, theme.fontFamily, theme.chipFontSize, DWRITE_FONT_WEIGHT_NORMAL,
                    &chipFormat_)) {
    return false;
  }

  HRESULT hr = renderer.context()->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 1), brush_.Put());
  if (FAILED(hr)) {
    LogHr(L"CreateSolidColorBrush", hr);
    return false;
  }

  if (FAILED(factory->CreateTextLayout(L"搜索应用与文件", 7, placeholderFormat_.Get(), 560.0f,
                                       40.0f, placeholderLayout_.Put()))) {
    LogError(L"占位文本布局创建失败");
    return false;
  }
  return true;
}

void Scene::SetTheme(const Theme& theme) {
  theme_ = theme;
  queryKey_.clear();
  chipSignature_.clear();
}

void Scene::EnsureQueryLayout(const SceneState& state) {
  wchar_t tail[64];
  swprintf(tail, ARRAYSIZE(tail), L"|%zu|%zu", state.compositionStart, state.compositionLength);
  const std::wstring key = state.displayText + tail;
  if (key == queryKey_ && queryLayout_) {
    const size_t caret = std::min(state.caretIndex, state.displayText.size());
    if (caret != queryCaret_) UpdateCaretMetrics(state);
    return;
  }
  queryKey_ = key;
  queryLayout_.Reset();
  caretDipX_ = 0.0f;
  scrollOffset_ = 0.0f;
  queryCaret_ = 0;
  if (state.displayText.empty()) return;

  const UINT32 length = static_cast<UINT32>(state.displayText.size());
  if (FAILED(renderer_->dwrite()->CreateTextLayout(state.displayText.c_str(), length,
                                                   queryFormat_.Get(), 2400.0f, 40.0f,
                                                   queryLayout_.Put()))) {
    LogError(L"查询文本布局创建失败");
    return;
  }
  if (state.compositionLength > 0) {
    queryLayout_->SetUnderline(TRUE, DWRITE_TEXT_RANGE{static_cast<UINT32>(state.compositionStart),
                                                      static_cast<UINT32>(state.compositionLength)});
  }
  UpdateCaretMetrics(state);
}

void Scene::UpdateCaretMetrics(const SceneState& state) {
  if (!queryLayout_ || state.displayText.empty()) {
    caretDipX_ = 0.0f;
    scrollOffset_ = 0.0f;
    queryCaret_ = 0;
    return;
  }
  const UINT32 caret = static_cast<UINT32>(std::min(state.caretIndex, state.displayText.size()));
  DWRITE_HIT_TEST_METRICS metrics{};
  FLOAT x = 0.0f;
  FLOAT y = 0.0f;
  if (SUCCEEDED(queryLayout_->HitTestTextPosition(caret, FALSE, &x, &y, &metrics))) {
    caretDipX_ = x;
  }
  const float maxVisible = kInputMaxWidth - 8.0f;
  if (caretDipX_ > scrollOffset_ + maxVisible) {
    scrollOffset_ = caretDipX_ - maxVisible;
  } else if (caretDipX_ < scrollOffset_) {
    scrollOffset_ = caretDipX_;
  }
  if (scrollOffset_ < 0.0f) scrollOffset_ = 0.0f;
  queryCaret_ = caret;
}

void Scene::EnsureRowLayouts(const SceneState& state) {
  std::vector<std::wstring> signature;
  signature.reserve(state.rows.size() * 4);
  for (const ResultRow& row : state.rows) {
    signature.push_back(row.title);
    signature.push_back(row.subtitle);
    signature.push_back(row.glyph);
    signature.push_back(row.iconKey);
  }
  if (signature == rowSignature_ && !titleLayouts_.empty()) return;
  rowSignature_ = std::move(signature);
  titleLayouts_.clear();
  subtitleLayouts_.clear();
  glyphLayouts_.clear();
  IDWriteFactory* factory = renderer_->dwrite();
  for (const ResultRow& row : state.rows) {
    ComPtr<IDWriteTextLayout> title;
    if (SUCCEEDED(factory->CreateTextLayout(row.title.c_str(),
                                            static_cast<UINT32>(row.title.size()),
                                            titleFormat_.Get(), 420.0f, 18.0f, title.Put()))) {
      titleLayouts_.push_back(std::move(title));
    } else {
      titleLayouts_.emplace_back();
    }
    ComPtr<IDWriteTextLayout> subtitle;
    if (SUCCEEDED(factory->CreateTextLayout(row.subtitle.c_str(),
                                            static_cast<UINT32>(row.subtitle.size()),
                                            subtitleFormat_.Get(), 600.0f, 16.0f,
                                            subtitle.Put()))) {
      subtitleLayouts_.push_back(std::move(subtitle));
    } else {
      subtitleLayouts_.emplace_back();
    }
    ComPtr<IDWriteTextLayout> glyph;
    if (SUCCEEDED(factory->CreateTextLayout(row.glyph.c_str(),
                                            static_cast<UINT32>(row.glyph.size()),
                                            glyphFormat_.Get(), 28.0f, 28.0f, glyph.Put()))) {
      glyphLayouts_.push_back(std::move(glyph));
    } else {
      glyphLayouts_.emplace_back();
    }
  }
}

float Scene::CaretX(const SceneState& state) {
  EnsureQueryLayout(state);
  if (state.displayText.empty()) return kInputLeft;
  return kInputLeft + caretDipX_ - scrollOffset_;
}

void Scene::EnsureChipLayouts(const SceneState& state) {
  std::vector<std::wstring> signature;
  signature.reserve(state.chips.size() * 2);
  for (const ChipState& chip : state.chips) {
    signature.push_back(chip.label);
    signature.push_back(chip.active ? L"1" : L"0");
  }
  if (signature == chipSignature_ && chipRects_.size() == state.chips.size()) return;
  chipSignature_ = std::move(signature);
  chipLayouts_.clear();
  chipRects_.clear();
  chipLayouts_.resize(state.chips.size());
  chipRects_.resize(state.chips.size());
  if (state.chips.empty()) return;

  IDWriteFactory* factory = renderer_->dwrite();
  std::vector<float> widths(state.chips.size(), 0.0f);
  std::vector<float> lefts(state.chips.size(), 0.0f);
  for (size_t i = 0; i < state.chips.size(); ++i) {
    const std::wstring& label = state.chips[i].label;
    const UINT32 length = static_cast<UINT32>(label.size());
    float textWidth = 0.0f;
    ComPtr<IDWriteTextLayout> probe;
    if (SUCCEEDED(factory->CreateTextLayout(label.c_str(), length, chipFormat_.Get(), 300.0f,
                                            20.0f, probe.Put()))) {
      DWRITE_TEXT_METRICS metrics{};
      if (SUCCEEDED(probe->GetMetrics(&metrics))) textWidth = metrics.width;
    }
    widths[i] = std::max(28.0f, textWidth + theme_.chipPaddingX * 2.0f);
    // 左对齐绘制：布局宽度按文字实际宽度收紧，再画在 chipPaddingX 偏移处。
    const float layoutWidth = std::max(8.0f, textWidth + 2.0f);
    if (FAILED(factory->CreateTextLayout(label.c_str(), length, chipFormat_.Get(), layoutWidth,
                                         20.0f, chipLayouts_[i].Put()))) {
      chipLayouts_[i].Reset();
    }
  }

  float x = kChipLeft;
  for (size_t i = 0; i < widths.size(); ++i) {
    lefts[i] = x;
    x += widths[i] + theme_.chipGap;
  }

  // 放不下时整体左移，保证当前组完整可见；但不会把当前组推到左边外面。
  float scroll = 0.0f;
  const float availableRight = theme_.panelWidth - kChipLeft;
  const float totalRight = lefts.back() + widths.back();
  if (totalRight > availableRight) {
    size_t active = 0;
    for (size_t i = 0; i < state.chips.size(); ++i) {
      if (state.chips[i].active) {
        active = i;
        break;
      }
    }
    if (lefts[active] + widths[active] > availableRight) {
      scroll = lefts[active] + widths[active] - availableRight;
    }
    scroll = std::min(scroll, std::max(0.0f, lefts[active] - kChipLeft));
  }

  for (size_t i = 0; i < widths.size(); ++i) {
    const float chipLeft = lefts[i] - scroll;
    chipRects_[i] =
        D2D1::RectF(chipLeft, kChipTop, chipLeft + widths[i], kChipTop + theme_.chipHeight);
  }
}

int Scene::ChipAt(const SceneState& state, float xDip, float yDip) {
  if (state.chips.empty()) return -1;
  EnsureChipLayouts(state);
  if (yDip < kChipTop || yDip > kChipTop + theme_.chipHeight) return -1;
  for (size_t i = 0; i < chipRects_.size(); ++i) {
    if (xDip >= chipRects_[i].left && xDip <= chipRects_[i].right) return static_cast<int>(i);
  }
  return -1;
}

int Scene::RowAt(const SceneState& state, float xDip, float yDip) {
  if (state.rows.empty()) return -1;
  const int visibleRows = std::clamp(state.visibleRows, 0, kMaxVisibleRows);
  if (visibleRows <= 0) return -1;
  if (xDip < kRowHitLeft || xDip > theme_.panelWidth - kRowHitLeft) return -1;
  const float listTop = theme_.listTop + (state.chips.empty() ? 0.0f : theme_.filterRow);
  const float stride = theme_.rowHeight + theme_.rowGap;
  const float offset = yDip - listTop;
  if (offset < 0.0f || offset >= static_cast<float>(visibleRows) * stride) return -1;
  const int line = static_cast<int>(offset / stride);
  // 行与行之间的 4 DIP 间隙不算命中，免得点在缝隙上也算点中。
  if (offset - static_cast<float>(line) * stride > theme_.rowHeight) return -1;
  const int index = state.scroll + line;
  if (index < 0 || index >= static_cast<int>(state.rows.size())) return -1;
  // 占位提示行不可点（没有动作），也不算悬停目标。
  if (state.rows[static_cast<size_t>(index)].action == RowAction::None) return -1;
  return index;
}

void Scene::Draw(const Theme& theme, const SceneState& state, bool fakeWallpaper, float scale) {
  ID2D1DeviceContext* ctx = renderer_->context();
  const int visibleRows = std::clamp(state.visibleRows, 0, kMaxVisibleRows);
  const bool withChips = !state.chips.empty();
  const float listTop = theme.listTop + (withChips ? theme.filterRow : 0.0f);
  const float panelHeight = PanelHeight(theme, visibleRows, withChips);
  const float canvasWidth = theme.panelWidth + theme.shadowMargin * 2.0f;
  const float canvasHeight = panelHeight + theme.shadowMargin * 2.0f;
  const D2D1_POINT_2F panel = D2D1::Point2F(theme.shadowMargin, theme.shadowMargin);
  const float contentAlpha = std::clamp(state.contentAlpha, 0.0f, 1.0f);
  const float globalAlpha = std::clamp(state.globalAlpha, 0.0f, 1.0f);
  const float bodyAlpha = contentAlpha * globalAlpha;

  EnsureQueryLayout(state);
  EnsureRowLayouts(state);
  if (withChips) EnsureChipLayouts(state);

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

  const D2D1_ROUNDED_RECT panelRect{
      D2D1::RectF(panel.x, panel.y, panel.x + theme.panelWidth, panel.y + panelHeight),
      theme.corner, theme.corner};

  // 分层柔和阴影。两条约束：
  // 1) 扩散量比 shadowMargin 少 2 DIP，外面留一圈纯透明——窗口自己的圆角裁剪（DWM 的半径比面板小）
  //    就不会切到阴影，四角不会出现「比面板更方的硬弧」；
  // 2) 每层的圆角半径都用 theme.corner（不随扩散变大）：外圈轮廓的圆角与面板一致，
  //    否则同心放大（radius + spread）会让外圈看起来比面板更「方」。
  const float shadowLimit = std::max(0.0f, theme.shadowMargin - 2.0f);
  for (int layer = 6; layer >= 1; --layer) {
    const float spread = std::min(static_cast<float>(layer) * 2.2f, shadowLimit);
    if (spread <= 0.0f) break;
    brush_->SetColor(WithAlpha(theme.shadow, theme.shadow.a * 0.06f * globalAlpha));
    ctx->FillRoundedRectangle(
        D2D1::RoundedRect(
            D2D1::RectF(panelRect.rect.left - spread, panelRect.rect.top - spread,
                        panelRect.rect.right + spread, panelRect.rect.bottom + spread),
            theme.corner, theme.corner),
        brush_.Get());
  }

  D2D1_COLOR_F panelColor = theme.panel;
  if (state.glass) panelColor.a = 0.35f;
  panelColor.a *= globalAlpha;
  brush_->SetColor(panelColor);
  ctx->FillRoundedRectangle(panelRect, brush_.Get());
  brush_->SetColor(ScaleAlpha(theme.border, globalAlpha));
  ctx->DrawRoundedRectangle(panelRect, brush_.Get(), 1.0f);

  // 顶部柔光
  D2D1_COLOR_F glowColor = ScaleAlpha(theme.glow, globalAlpha);
  D2D1_GRADIENT_STOP topStops[3] = {{0.0f, glowColor},
                                    {96.0f / panelHeight, WithAlpha(glowColor, 0.0f)},
                                    {1.0f, WithAlpha(glowColor, 0.0f)}};
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
  brush_->SetColor(ScaleAlpha(WithAlpha(theme.textSecondary, 0.85f), globalAlpha));
  ctx->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(panel.x + 35.0f, panel.y + 35.0f), 7.0f, 7.0f),
                   brush_.Get(), 1.8f);
  ctx->DrawLine(D2D1::Point2F(panel.x + 40.0f, panel.y + 40.0f),
                D2D1::Point2F(panel.x + 46.0f, panel.y + 46.0f), brush_.Get(), 1.8f);

  // 输入文本（组字串已插入显示文本）
  if (state.displayText.empty()) {
    brush_->SetColor(ScaleAlpha(theme.textPlaceholder, globalAlpha));
    ctx->DrawTextLayout(D2D1::Point2F(panel.x + kInputLeft, panel.y + kInputTop),
                        placeholderLayout_.Get(), brush_.Get());
  } else if (queryLayout_) {
    brush_->SetColor(ScaleAlpha(theme.textPrimary, globalAlpha));
    ctx->DrawTextLayout(D2D1::Point2F(panel.x + kInputLeft - scrollOffset_,
                                      panel.y + kInputTop),
                        queryLayout_.Get(), brush_.Get());
  }

  // 光标
  if (state.caretVisible) {
    const float caretX = panel.x + kInputLeft + caretDipX_ - scrollOffset_;
    brush_->SetColor(ScaleAlpha(theme.accent, globalAlpha));
    ctx->FillRectangle(
        D2D1::RectF(caretX + 1.0f, panel.y + kCaretTop, caretX + 2.6f,
                    panel.y + kCaretTop + kCaretHeight),
        brush_.Get());
  }

  // 分隔线：没有推荐/结果、也没有筛选芯片时不画
  if (!state.rows.empty() || withChips) {
    brush_->SetColor(ScaleAlpha(theme.divider, bodyAlpha));
    ctx->FillRectangle(
        D2D1::RectF(panel.x + 20.0f, panel.y + 72.0f, panel.x + theme.panelWidth - 20.0f,
                    panel.y + 73.0f),
        brush_.Get());
  }

  // 筛选组芯片：横向一排，超出面板宽度时被裁剪（当前组始终滚动到可见）。
  if (withChips) {
    ctx->PushAxisAlignedClip(
        D2D1::RectF(panel.x + 12.0f, panel.y + kChipTop - 6.0f,
                    panel.x + theme.panelWidth - 12.0f,
                    panel.y + kChipTop + theme.chipHeight + 6.0f),
        D2D1_ANTIALIAS_MODE_ALIASED);
    for (size_t i = 0; i < state.chips.size() && i < chipRects_.size(); ++i) {
      const bool active = state.chips[i].active;
      const bool hover = static_cast<int>(i) == state.hoverChip;
      const D2D1_RECT_F local = chipRects_[i];
      const D2D1_ROUNDED_RECT chipRect{
          D2D1::RectF(panel.x + local.left, panel.y + local.top, panel.x + local.right,
                      panel.y + local.bottom),
          theme.chipHeight * 0.5f, theme.chipHeight * 0.5f};
      if (active) {
        brush_->SetColor(WithAlpha(theme.accent, 0.18f * globalAlpha));
        ctx->FillRoundedRectangle(chipRect, brush_.Get());
        brush_->SetColor(ScaleAlpha(theme.accent, globalAlpha));
        ctx->DrawRoundedRectangle(chipRect, brush_.Get(), 1.0f);
      } else {
        brush_->SetColor(ScaleAlpha(hover ? theme.hoverFill : WithAlpha(theme.textPrimary, 0.05f),
                                    globalAlpha));
        ctx->FillRoundedRectangle(chipRect, brush_.Get());
      }
      if (i < chipLayouts_.size() && chipLayouts_[i]) {
        brush_->SetColor(
            ScaleAlpha(active ? theme.textPrimary : theme.textSecondary, globalAlpha));
        ctx->DrawTextLayout(D2D1::Point2F(chipRect.rect.left + theme.chipPaddingX,
                                          chipRect.rect.top + (theme.chipHeight - 20.0f) * 0.5f),
                            chipLayouts_[i].Get(), brush_.Get());
      }
    }
    ctx->PopAxisAlignedClip();
  }

  // 鼠标悬停行：选中行不重复画（选中态更亮），只在可见窗口内画。
  const int hoverLine = state.hoverRow - state.scroll;
  if (state.hoverRow >= 0 && state.hoverRow != state.selected && hoverLine >= 0 &&
      hoverLine < visibleRows && state.hoverRow < static_cast<int>(state.rows.size())) {
    const float hoverY =
        panel.y + listTop + static_cast<float>(hoverLine) * (theme.rowHeight + theme.rowGap);
    brush_->SetColor(ScaleAlpha(theme.hoverFill, globalAlpha));
    ctx->FillRoundedRectangle(
        D2D1::RoundedRect(D2D1::RectF(panel.x + kRowHitLeft, hoverY,
                                      panel.x + theme.panelWidth - kRowHitLeft,
                                      hoverY + theme.rowHeight),
                          theme.rowCorner, theme.rowCorner),
        brush_.Get());
  }

  // 选中态（位置可动画）
  const float selectionOffset =
      (state.selectionRow - static_cast<float>(state.scroll)) * (theme.rowHeight + theme.rowGap);
  const bool selectionVisible = state.selectionRow >= -0.5f &&
                                state.selectionRow <= static_cast<float>(state.scroll + visibleRows) -
                                                           0.5f;
  if (selectionVisible && !state.rows.empty()) {
    const float y = panel.y + listTop + selectionOffset;
    brush_->SetColor(ScaleAlpha(theme.selectedFill, globalAlpha));
    ctx->FillRoundedRectangle(D2D1::RoundedRect(
                                  D2D1::RectF(panel.x + kRowHitLeft, y,
                                              panel.x + theme.panelWidth - kRowHitLeft,
                                              y + theme.rowHeight),
                                  theme.rowCorner, theme.rowCorner),
                              brush_.Get());
    brush_->SetColor(ScaleAlpha(theme.accent, globalAlpha));
    ctx->FillRoundedRectangle(
        D2D1::RoundedRect(D2D1::RectF(panel.x + 20.0f, y + 12.0f, panel.x + 23.0f, y + 32.0f),
                          1.5f, 1.5f),
        brush_.Get());
  }

  // 虚拟化行内容：只绘制可见窗口
  for (int v = 0; v < visibleRows; ++v) {
    const size_t index = static_cast<size_t>(state.scroll + v);
    if (index >= state.rows.size()) break;
    const ResultRow& row = state.rows[index];
    const float y = panel.y + listTop +
                    static_cast<float>(v) * (theme.rowHeight + theme.rowGap);

    ID2D1Bitmap1* icon = (icons_ && !row.iconKey.empty()) ? icons_->Get(row.iconKey) : nullptr;
    if (icon) {
      ctx->DrawBitmap(icon,
                      D2D1::RectF(panel.x + 36.0f, y + 8.0f, panel.x + 64.0f, y + 36.0f),
                      contentAlpha * globalAlpha, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
    } else {
      brush_->SetColor(ScaleAlpha(Rgba(row.color, 0.95f), bodyAlpha));
      ctx->FillRoundedRectangle(
          D2D1::RoundedRect(D2D1::RectF(panel.x + 36.0f, y + 8.0f, panel.x + 64.0f, y + 36.0f),
                            theme.tileCorner, theme.tileCorner),
          brush_.Get());
    }

    if (!icon && index < glyphLayouts_.size() && glyphLayouts_[index]) {
      brush_->SetColor(ScaleAlpha(theme.tileText, bodyAlpha));
      ctx->DrawTextLayout(D2D1::Point2F(panel.x + 36.0f, y + 8.0f), glyphLayouts_[index].Get(),
                          brush_.Get());
    }
    if (index < titleLayouts_.size() && titleLayouts_[index]) {
      brush_->SetColor(ScaleAlpha(theme.textPrimary, bodyAlpha));
      ctx->DrawTextLayout(D2D1::Point2F(panel.x + 76.0f, y + 7.0f), titleLayouts_[index].Get(),
                          brush_.Get());
    }
    if (index < subtitleLayouts_.size() && subtitleLayouts_[index]) {
      brush_->SetColor(ScaleAlpha(theme.textSecondary, bodyAlpha));
      ctx->DrawTextLayout(D2D1::Point2F(panel.x + 76.0f, y + 24.0f),
                          subtitleLayouts_[index].Get(), brush_.Get());
    }
  }

  ctx->SetTransform(D2D1::Matrix3x2F::Identity());
}
