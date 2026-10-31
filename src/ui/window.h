#pragma once

#include <windows.h>

#include <functional>
#include <string>
#include <vector>

#include "search/app_entry.h"
#include "search/file_index.h"
#include "search/result.h"
#include "ui/icon_cache.h"
#include "ui/input.h"
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

  bool Create(HINSTANCE instance, Renderer* renderer, Scene* scene, IconCache* icons,
              const std::wstring& fileRoot = std::wstring());
  void SetTheme(const Theme& theme);
  void SetGlass(bool glass);
  void SetQuery(const std::wstring& query);
  // 测试钩子：直接注入组字状态，用于验证组字串渲染（真实 IME 由 WM_IME_* 驱动）。
  void SimulateComposition(const std::wstring& composition, size_t caretInComposition);
  void SetOnHotkey(std::function<void()> callback);
  void SetOnTimer(std::function<void(UINT_PTR)> callback);

  void Toggle();
  void ShowAndFocus();
  void Hide();
  void Shutdown();
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
  void UpdateSceneState();
  void UpdateImeWindow();
  void EnsureSelectionVisible();
  void MoveSelection(int delta);
  void ScrollBy(int delta);
  void RebuildRows();
  void Refilter();
  void PreloadIcons(bool warm);
  bool HandleKeyDown(WPARAM key);
  bool HandleChar(wchar_t ch);
  bool HandleIme(UINT message, WPARAM wParam, LPARAM lParam);
  void ResetQuery();
  void StartSelectionAnimation();
  void StartContentFade();
  void StartShowAnimation();
  void TickAnimation();

  HWND hwnd_ = nullptr;
  HINSTANCE instance_ = nullptr;
  Renderer* renderer_ = nullptr;
  Scene* scene_ = nullptr;
  Theme theme_;
  TextInput input_;
  std::vector<AppEntry> apps_;
  FileIndex files_;
  uint64_t fileVersionSeen_ = 0;
  uint64_t iconVersionSeen_ = 0;
  IconCache* icons_ = nullptr;
  std::vector<ResultRow> rows_;
  SceneState sceneState_;
  std::function<void()> onHotkey_;
  std::function<void(UINT_PTR)> onTimer_;

  int selected_ = 0;
  int scroll_ = 0;
  int visibleRows_ = Scene::kMaxVisibleRows;
  float selectionRow_ = 0.0f;
  float contentAlpha_ = 1.0f;
  float showAlpha_ = 1.0f;
  bool caretVisible_ = true;
  bool glass_ = false;
  bool visible_ = false;
  UINT dpi_ = 96;
  UINT widthPx_ = 0;
  UINT heightPx_ = 0;

  bool animating_ = false;
  bool showAnimating_ = false;
  bool selectionAnimating_ = false;
  bool contentAnimating_ = false;
  LARGE_INTEGER showStart_{};
  LARGE_INTEGER selectionStart_{};
  LARGE_INTEGER contentStart_{};
  float selectionFrom_ = 0.0f;
  float selectionTo_ = 0.0f;

  LARGE_INTEGER showRequestedAt_{};
  bool measureFirstFrame_ = false;
};
