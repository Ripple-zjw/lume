#pragma once

#include <d2d1_3.h>
#include <dwrite_3.h>

#include <string>
#include <vector>

#include "core/com_ptr.h"
#include "search/result.h"
#include "ui/theme.h"

class Renderer;
class IconCache;

struct ChipState {
  std::wstring label;
  bool active = false;
};

struct SceneState {
  std::wstring displayText;  // 已插入组字串
  size_t caretIndex = 0;
  size_t compositionStart = 0;
  size_t compositionLength = 0;
  bool caretVisible = true;
  std::vector<ResultRow> rows;
  std::vector<ChipState> chips;  // 搜索框下方的筛选组（空 = 不显示这一行）
  int hoverChip = -1;
  int hoverRow = -1;  // 鼠标悬停的结果行（绝对值，-1 = 没有）
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
  // 芯片行几何（面板相对 DIP）：与 Scene::ChipAt 共用同一份常量。
  static constexpr float kChipTop = 76.0f;
  static constexpr float kChipLeft = 20.0f;

  bool Init(Renderer& renderer, const Theme& theme, IconCache* icons = nullptr);
  void SetTheme(const Theme& theme);
  void Draw(const Theme& theme, const SceneState& state, bool fakeWallpaper, float scale);
  // 光标相对面板左上角的 X（DIP），用于 IME 候选窗定位。
  float CaretX(const SceneState& state);
  // 面板相对 DIP 坐标落在哪颗筛选芯片上；-1 = 不在芯片上（窗口用来处理点击/悬停）。
  int ChipAt(const SceneState& state, float xDip, float yDip);
  // 面板相对 DIP 坐标落在哪一行结果上（绝对值，含滚动偏移）；-1 = 不在行上。
  int RowAt(const SceneState& state, float xDip, float yDip);

 private:
  void EnsureQueryLayout(const SceneState& state);
  void UpdateCaretMetrics(const SceneState& state);
  void EnsureRowLayouts(const SceneState& state);
  void EnsureChipLayouts(const SceneState& state);

  Renderer* renderer_ = nullptr;
  IconCache* icons_ = nullptr;
  Theme theme_;
  ComPtr<ID2D1SolidColorBrush> brush_;
  ComPtr<IDWriteTextFormat> queryFormat_;
  ComPtr<IDWriteTextFormat> titleFormat_;
  ComPtr<IDWriteTextFormat> subtitleFormat_;
  ComPtr<IDWriteTextFormat> glyphFormat_;
  ComPtr<IDWriteTextFormat> placeholderFormat_;
  ComPtr<IDWriteTextFormat> chipFormat_;

  std::wstring queryKey_;
  ComPtr<IDWriteTextLayout> queryLayout_;
  ComPtr<IDWriteTextLayout> placeholderLayout_;
  float caretDipX_ = 0.0f;
  float scrollOffset_ = 0.0f;
  size_t queryCaret_ = 0;

  std::vector<std::wstring> rowSignature_;
  std::vector<ComPtr<IDWriteTextLayout>> titleLayouts_;
  std::vector<ComPtr<IDWriteTextLayout>> subtitleLayouts_;
  std::vector<ComPtr<IDWriteTextLayout>> glyphLayouts_;

  std::vector<std::wstring> chipSignature_;
  std::vector<ComPtr<IDWriteTextLayout>> chipLayouts_;
  std::vector<D2D1_RECT_F> chipRects_;  // 已含横向滚动偏移
};
