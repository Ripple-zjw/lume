#pragma once

#include <windows.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "app/settings.h"
#include "platform/tray.h"
#include "search/app_entry.h"
#include "search/everything_index.h"
#include "search/file_index.h"
#include "search/result.h"
#include "search/usage.h"
#include "ui/icon_cache.h"
#include "ui/input.h"
#include "ui/scene.h"
#include "ui/settings_window.h"
#include "ui/theme.h"

class Renderer;

const wchar_t* WindowClassName();
constexpr UINT kMsgToggle = WM_APP + 1;
// Everything 的「去掉 ext: 下推」重试：必须从 WM_COPYDATA 处理里延后出来发，
// 否则会和 Everything 投递回复的 SendMessage 互相等待（它的投递超时是 3 秒）。
constexpr UINT kMsgEverythingRetry = WM_APP + 2;

class LauncherWindow {
 public:
  LauncherWindow() = default;
  ~LauncherWindow();
  LauncherWindow(const LauncherWindow&) = delete;
  LauncherWindow& operator=(const LauncherWindow&) = delete;

  bool Create(HINSTANCE instance, Renderer* renderer, Scene* scene, IconCache* icons,
              const AppSettings& settings, const std::wstring& fileRootOverride = std::wstring());
  void SetTheme(const Theme& theme);
  void SetGlass(bool glass);
  void SetQuery(const std::wstring& query);
  // 测试钩子：直接注入组字状态，验证组字串渲染。
  void SimulateComposition(const std::wstring& composition, size_t caretInComposition);
  void SetOnHotkey(std::function<void()> callback);
  void SetOnTimer(std::function<void(UINT_PTR)> callback);
  void SetOnSettingsChanged(std::function<void(const AppSettings&)> callback);
  void SetOnHotkeyRequest(std::function<void(const AppSettings&)> callback);

  void Toggle();
  void ShowAndFocus();
  void Hide();
  void Shutdown();
  void RenderNow();
  void OpenSettings();
  void ClearFileIndex();
  void RebuildFileIndex();
  void OpenIndexDirectory();
  IndexStatus QueryIndexStatus() const;
  // 文件搜索来源是 Everything 时，设置窗用来显示连接状态。
  bool FileSearchIsEverything() const;
  EverythingStatus QueryEverythingStatus() const;
  // 关于页：版本、系统、热键、Everything 状态与各数据目录。
  AboutInfo QueryAboutInfo() const;
  // 测试钩子：聚焦热键捕获框并合成指定组合，验证低级键盘钩子。
  void TestHotkeyCapture(const std::wstring& combo);
  // 测试钩子：把鼠标消息投到第 row 行（0 起）的中心，验证命中测试与点击启动。
  void InjectMouseRow(int row, bool click);
  // 测试钩子：模拟一次托盘左键（v4 会先后送来 NIN_SELECT 与 WM_LBUTTONUP 两条通知）。
  void InjectTrayClick();
  bool visible() const { return visible_; }
  HWND hwnd() const { return hwnd_; }
  const AppSettings& settings() const { return settings_; }

 private:
  static LRESULT CALLBACK Thunk(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
  LRESULT Handle(UINT message, WPARAM wParam, LPARAM lParam);

  void ApplyBackdrop();
  void ApplyDpi(UINT dpi);
  void PositionOnCursorMonitor();
  void UpdateSceneState();
  void UpdateImeWindow();
  void UpdateLayoutForRows();
  void EnsureSelectionVisible();
  void MoveSelection(int delta);
  void ScrollBy(int delta);
  void RebuildRows();
  // 先按当前来源刷新 fileHits_，再重建行（本地同步搜索 / Everything 发异步请求）。
  void RefreshResults();
  void UpdateFileHits();
  void RequestEverythingRows();
  // 当前生效的筛选组；nullptr = 「全部」或筛选已关闭。
  const FilterGroup* CurrentFilterGroup() const;
  // 切换筛选组（0 = 全部）。会重新查询、淡入、持久化并记日志。
  void SelectFilter(int index);
  void CycleFilter(int delta);
  // 托盘左键：只显示、不切换（一次点击会来两条通知，去抖后只处理一次）。
  void ShowFromTray();
  // Everything 模式下按需删除本地快照：只有连上并索引就绪时才删，否则留到下次重试。
  void TryDeleteSnapshotForEverything();
  void Refilter();
  void PreloadIcons(bool warm);
  void ApplySettings(const AppSettings& settings);
  void HandleTrayCommand(UINT command);
  // 客户端像素坐标 → 面板相对 DIP（扣掉阴影边距与 DPI 缩放）。
  void ToPanelDip(int clientX, int clientY, float* xDip, float* yDip) const;
  // 鼠标下命中的筛选芯片 / 结果行序号（-1 = 没有）。
  int ChipAtPoint(int clientX, int clientY);
  int RowAtPoint(int clientX, int clientY);
  bool RowIsActionable(int index) const;
  // 点击结果行：先把它变成选中行，再按 Shift/Ctrl 启动（与回车同一套逻辑）。
  void ClickRow(int index, bool shift, bool ctrl);
  // 启动/打开第 index 行（回车与鼠标点击共用）。
  void ActivateRow(int index, bool shift, bool ctrl);
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
  IconCache* icons_ = nullptr;
  Theme theme_;
  AppSettings settings_;
  TextInput input_;
  std::vector<AppEntry> apps_;
  FileIndex files_;
  // 当前查询的文件命中（本地索引同步产生，Everything 模式由回复消息异步更新）。
  std::vector<FileHit> fileHits_;
  EverythingIndex everything_;
  uint32_t queryGeneration_ = 0;
  bool filesRunning_ = false;
  // 切到 Everything 时它还没连上：先保留 files.idx，等连上后再删。
  bool pendingSnapshotDelete_ = false;
  uint64_t lastEverythingErrorLogAt_ = 0;
  LARGE_INTEGER everythingRequestedAt_{};
  UsageStore usage_;
  uint64_t fileVersionSeen_ = 0;
  uint64_t iconVersionSeen_ = 0;
  TrayIcon tray_;
  std::unique_ptr<SettingsWindow> settingsWindow_;
  std::vector<ResultRow> rows_;
  SceneState sceneState_;
  std::function<void()> onHotkey_;
  std::function<void(UINT_PTR)> onTimer_;
  std::function<void(const AppSettings&)> onSettingsChanged_;
  std::function<void(const AppSettings&)> onHotkeyRequest_;

  int selected_ = 0;
  int scroll_ = 0;
  int visibleRows_ = Scene::kMaxVisibleRows;
  int hoverChip_ = -1;        // 鼠标悬停的筛选芯片（-1 = 没有）
  int hoverRow_ = -1;         // 鼠标悬停的结果行（绝对值，-1 = 没有）
  int pressedRow_ = -1;       // 左键按下时的行（抬起时同一行才启动）
  // 最近一次托盘左键的时间（GetTickCount64）：用于去抖和「shell 抢前台不算失活」。
  uint64_t lastTrayClickAt_ = 0;
  bool mouseTracking_ = false;  // 已向系统登记 TME_LEAVE
  // 纯扩展名组下推 Everything 后 0 条时的一次性重试标记（"查询\n组名"）。
  std::wstring pushdownRetryKey_;
  // 正在处理 Everything 的回复：此期间绝不能回调 Everything（否则卡 3 秒，见 kMsgEverythingRetry）。
  bool handlingEverythingReply_ = false;
  float selectionRow_ = 0.0f;
  float contentAlpha_ = 1.0f;
  float showAlpha_ = 1.0f;
  bool caretVisible_ = true;
  int trayRetries_ = 0;
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
