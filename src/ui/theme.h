#pragma once

#include <d2d1.h>
#include <windows.h>

#include <string>

enum class ThemeMode { Dark, Light, System };

struct Theme {
  bool dark = true;
  const wchar_t* fontFamily = L"Segoe UI Variable Text";

  // 尺寸统一以 DIP 表示；绘制时统一乘 DPI 缩放。
  float panelWidth = 720.0f;
  float searchArea = 72.0f;
  float listTop = 84.0f;
  float rowHeight = 44.0f;
  float rowGap = 4.0f;
  float bottomPadding = 16.0f;
  // 面板四周留给自绘阴影的透明边距（窗口 = 面板 + 2×shadowMargin，按 DPI 缩放）。
  // 24 时面板外会留一圈 24–30px 的透明带、看着像多套了一个框；8 是「有呼吸感但不抢眼」的取值。
  float shadowMargin = 8.0f;
  float corner = 16.0f;
  float rowCorner = 10.0f;
  float tileCorner = 7.0f;
  // 搜索框下方的筛选组芯片行（关闭筛选时整行不占高度）。
  float filterRow = 40.0f;
  float chipHeight = 26.0f;
  float chipGap = 8.0f;
  float chipPaddingX = 12.0f;

  float queryFontSize = 26.0f;
  float titleFontSize = 14.0f;
  float subtitleFontSize = 11.5f;
  float glyphFontSize = 13.0f;
  float chipFontSize = 12.5f;

  D2D1_COLOR_F panel{};
  D2D1_COLOR_F border{};
  D2D1_COLOR_F textPrimary{};
  D2D1_COLOR_F textSecondary{};
  D2D1_COLOR_F textPlaceholder{};
  D2D1_COLOR_F accent{};
  D2D1_COLOR_F accentOn{};
  D2D1_COLOR_F selectedFill{};
  D2D1_COLOR_F hoverFill{};
  D2D1_COLOR_F divider{};
  D2D1_COLOR_F glow{};
  D2D1_COLOR_F shadow{};
  D2D1_COLOR_F tileText{};
};

// 默认强调色（设计基线）：不跟随系统，避免系统红色强调色把选中行染成酒红。
// 需要原生观感时用 --accent=system 跟随系统个性化色。
constexpr DWORD kDesignAccent = 0x6EA8FF;

Theme MakeTheme(ThemeMode mode, DWORD accentRgb = kDesignAccent);
ThemeMode ParseThemeMode(const std::wstring& value);
const wchar_t* ThemeModeName(ThemeMode mode);
// "system" → 系统强调色；"#RRGGBB"/"RRGGBB" → 指定颜色；其他 → fallback。
DWORD ParseAccent(const std::wstring& value, DWORD fallback = kDesignAccent);

// 面板高度 = 列表起点（有筛选芯片时下移 filterRow）+ 行高×行数 + 行间距 + 底部内边距
float PanelHeight(const Theme& theme, int rows, bool withChips = false);
