#pragma once

#include <d2d1_3.h>
#include <dwrite_3.h>

#include <string>
#include <vector>

#include "core/com_ptr.h"
#include "search/result.h"
#include "ui/theme.h"

class Renderer;

struct SceneState {
  std::wstring displayText;  // 已插入组字串
  size_t caretIndex = 0;
  size_t compositionStart = 0;
  size_t compositionLength = 0;
  bool caretVisible = true;
  std::vector<ResultRow> rows;
  int selected = 0;
  int scroll = 0;
  int visibleRows = 6;
  float selectionRow = 0.0f;   // 动画中的选中行（可为小数）
  float contentAlpha = 1.0f;   // 结果区淡入
  float globalAlpha = 1.0f;    // 整窗淡入（唤出动画）
  bool glass = false;
};

// 面板绘制：窗口/离屏共用；文本布局与行资源做缓存。
class Scene {
 public:
  static constexpr int kMaxVisibleRows = 6;

  bool Init(Renderer& renderer, const Theme& theme);
  void SetTheme(const Theme& theme);
  void Draw(const Theme& theme, const SceneState& state, bool fakeWallpaper, float scale);
  // 光标相对面板左上角的 X（DIP），用于 IME 候选窗定位。
  float CaretX(const SceneState& state);

 private:
  void EnsureQueryLayout(const SceneState& state);
  void EnsureRowLayouts(const SceneState& state);

  Renderer* renderer_ = nullptr;
  ComPtr<ID2D1SolidColorBrush> brush_;
  ComPtr<IDWriteTextFormat> queryFormat_;
  ComPtr<IDWriteTextFormat> titleFormat_;
  ComPtr<IDWriteTextFormat> subtitleFormat_;
  ComPtr<IDWriteTextFormat> glyphFormat_;
  ComPtr<IDWriteTextFormat> placeholderFormat_;

  std::wstring queryKey_;
  ComPtr<IDWriteTextLayout> queryLayout_;
  ComPtr<IDWriteTextLayout> placeholderLayout_;
  float caretDipX_ = 0.0f;
  float scrollOffset_ = 0.0f;

  std::vector<std::wstring> rowSignature_;
  std::vector<ComPtr<IDWriteTextLayout>> titleLayouts_;
  std::vector<ComPtr<IDWriteTextLayout>> subtitleLayouts_;
  std::vector<ComPtr<IDWriteTextLayout>> glyphLayouts_;
};
