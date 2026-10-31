#include "ui/theme.h"

#include <cstdlib>

#include "platform/system.h"

namespace {

D2D1_COLOR_F Rgba(UINT32 rgb, float alpha) {
  return D2D1::ColorF(static_cast<float>((rgb >> 16) & 0xFF) / 255.0f,
                      static_cast<float>((rgb >> 8) & 0xFF) / 255.0f,
                      static_cast<float>(rgb & 0xFF) / 255.0f, alpha);
}

}  // namespace

float PanelHeight(const Theme& theme, int rows) {
  const int safeRows = rows < 1 ? 1 : rows;
  return theme.listTop + safeRows * (theme.rowHeight + theme.rowGap) - theme.rowGap +
         theme.bottomPadding;
}

Theme MakeTheme(ThemeMode mode, DWORD accentRgb) {
  if (mode == ThemeMode::System) {
    mode = IsSystemDarkTheme() ? ThemeMode::Dark : ThemeMode::Light;
  }

  Theme theme;
  theme.dark = (mode == ThemeMode::Dark);
  const D2D1_COLOR_F accent = Rgba(accentRgb, 1.0f);
  theme.accent = accent;
  theme.accentOn = Rgba(0xFFFFFF, 0.98f);

  if (theme.dark) {
    theme.panel = Rgba(0x1B1B1F, 0.88f);
    theme.border = Rgba(0xFFFFFF, 0.10f);
    theme.textPrimary = Rgba(0xFFFFFF, 0.95f);
    theme.textSecondary = Rgba(0xFFFFFF, 0.50f);
    theme.textPlaceholder = Rgba(0xFFFFFF, 0.36f);
    theme.selectedFill = D2D1::ColorF(accent.r, accent.g, accent.b, 0.16f);
    theme.hoverFill = Rgba(0xFFFFFF, 0.06f);
    theme.divider = Rgba(0xFFFFFF, 0.07f);
    theme.glow = Rgba(0xFFFFFF, 0.07f);
    theme.shadow = Rgba(0x000000, 0.55f);
    theme.tileText = Rgba(0xFFFFFF, 0.97f);
  } else {
    theme.panel = Rgba(0xF7F7FA, 0.86f);
    theme.border = Rgba(0x000000, 0.09f);
    theme.textPrimary = Rgba(0x17171C, 0.92f);
    theme.textSecondary = Rgba(0x17171C, 0.55f);
    theme.textPlaceholder = Rgba(0x17171C, 0.38f);
    theme.selectedFill = D2D1::ColorF(accent.r, accent.g, accent.b, 0.14f);
    theme.hoverFill = Rgba(0x000000, 0.04f);
    theme.divider = Rgba(0x000000, 0.07f);
    theme.glow = Rgba(0xFFFFFF, 0.55f);
    theme.shadow = Rgba(0x000000, 0.22f);
    theme.tileText = Rgba(0xFFFFFF, 0.97f);
  }
  return theme;
}

ThemeMode ParseThemeMode(const std::wstring& value) {
  if (value == L"light") return ThemeMode::Light;
  if (value == L"system") return ThemeMode::System;
  return ThemeMode::Dark;
}

const wchar_t* ThemeModeName(ThemeMode mode) {
  switch (mode) {
    case ThemeMode::Light:
      return L"light";
    case ThemeMode::System:
      return L"system";
    default:
      return L"dark";
  }
}

DWORD ParseAccent(const std::wstring& value, DWORD fallback) {
  if (value.empty()) return fallback;
  if (value == L"system") return SystemAccentColor();
  std::wstring hex = value;
  if (!hex.empty() && hex[0] == L'#') hex.erase(0, 1);
  if (hex.size() != 6) return fallback;
  wchar_t* end = nullptr;
  const unsigned long parsed = wcstoul(hex.c_str(), &end, 16);
  if (!end || *end != L'\0') return fallback;
  return static_cast<DWORD>(parsed & 0xFFFFFFu);
}
