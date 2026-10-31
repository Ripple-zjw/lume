#pragma once

#include <d2d1_3.h>
#include <dwrite_3.h>

#include <string>

#include "core/com_ptr.h"
#include "ui/theme.h"

class Renderer;

struct SceneState {
  std::wstring query;
  int selected = 0;
  bool glass = false;
  int rows = 3;  // P1 用固定样例数据，P3 换成真实结果
};

// 面板绘制：与窗口/离屏目标共用同一份代码。
class Scene {
 public:
  bool Init(Renderer& renderer, const Theme& theme);
  void Draw(const Theme& theme, const SceneState& state, bool fakeWallpaper, float scale);

 private:
  void DrawRows(ID2D1DeviceContext* ctx, const Theme& theme, const SceneState& state,
                D2D1_POINT_2F panel, float panelHeight);

  Renderer* renderer_ = nullptr;
  ComPtr<ID2D1SolidColorBrush> brush_;
  ComPtr<IDWriteTextFormat> queryFormat_;
  ComPtr<IDWriteTextFormat> titleFormat_;
  ComPtr<IDWriteTextFormat> subtitleFormat_;
  ComPtr<IDWriteTextFormat> glyphFormat_;
};
