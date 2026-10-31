#pragma once

#include <windows.h>

#include <functional>
#include <string>

#include "ui/scene.h"
#include "ui/theme.h"

class Renderer;

const wchar_t* WindowClassName();
constexpr UINT kMsgToggle = WM_APP + 1;

class LauncherWindow {
 public:
  LauncherWindow() = default;
  ~LauncherWindow();
  LauncherWindow(const LauncherWindow&) = delete;
  LauncherWindow& operator=(const LauncherWindow&) = delete;

  bool Create(HINSTANCE instance, Renderer* renderer, Scene* scene);
  void SetTheme(const Theme& theme);
  void SetSceneState(const SceneState& state);
  void SetOnHotkey(std::function<void()> callback);
  void SetOnTimer(std::function<void(UINT_PTR)> callback);

  void Toggle();
  void ShowAndFocus();
  void Hide();
  // 隐藏状态下先渲染一帧：把 Direct2D/DirectWrite 首次绘制的着色器与布局成本
  // 挪到启动阶段，使首次唤出不再承担冷启动开销。
  void RenderNow();
  bool visible() const { return visible_; }
  HWND hwnd() const { return hwnd_; }

 private:
  static LRESULT CALLBACK Thunk(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
  LRESULT Handle(UINT message, WPARAM wParam, LPARAM lParam);

  void ApplyBackdrop();
  void ApplyDpi(UINT dpi);
  void PositionOnCursorMonitor();

  HWND hwnd_ = nullptr;
  HINSTANCE instance_ = nullptr;
  Renderer* renderer_ = nullptr;
  Scene* scene_ = nullptr;
  Theme theme_;
  SceneState state_;
  std::function<void()> onHotkey_;
  std::function<void(UINT_PTR)> onTimer_;
  bool visible_ = false;
  UINT dpi_ = 96;
  UINT widthPx_ = 0;
  UINT heightPx_ = 0;
  LARGE_INTEGER showRequestedAt_{};
  bool measureFirstFrame_ = false;
};
