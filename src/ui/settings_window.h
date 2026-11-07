#pragma once

#include <windows.h>

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "app/settings.h"
#include "search/everything_index.h"

// 索引页的只读状态（由 LauncherWindow 从 FileIndex / EverythingIndex 汇总后回填）。
struct IndexStatus {
  std::wstring fileSearch;      // local / everything（实际生效的来源）
  std::wstring directory;       // 生效的快照目录（空 = 默认）
  std::wstring defaultDirectory;// 默认快照目录（%LOCALAPPDATA%\Lume 或 exe 目录）
  std::wstring snapshotPath;    // <directory>\files.idx
  uint64_t snapshotBytes = 0;   // 0 = 尚未生成
  size_t entries = 0;
  uint64_t lastRebuildDurationMs = 0;
  int limitMB = 512;
  bool rebuilding = false;
  bool snapshotWritable = true;
  // fileSearch == everything 时的连接状态
  bool evAvailable = false;
  bool evDbLoaded = false;
  std::wstring evVersion;   // "1.4.1.1028"
  std::wstring evError;     // 不可用原因
};

// 「关于」页的只读信息（由 LauncherWindow 汇总）。
struct AboutInfo {
  std::wstring version;       // "0.2.5"
  std::wstring os;            // "Windows 11 (build 26200)"
  std::wstring arch;          // "x64"
  std::wstring dpi;           // "120 (125%)"
  std::wstring hotkey;        // 当前生效热键
  std::wstring settingsPath;  // settings.conf
  std::wstring usagePath;     // usage.conf
  std::wstring logPath;       // 当前日志文件
  std::wstring logLevel;      // 当前日志等级（error/warn/info/debug）
  std::wstring indexDir;      // 索引/快照目录
  std::wstring fileSearch;    // local / everything
  std::wstring everything;    // "1.4.1.1028（已连接，索引就绪）" 等
};

class SettingsWindow {
 public:
  using SaveCallback = std::function<void(const AppSettings&)>;

  struct Callbacks {
    SaveCallback onSave;
    std::function<IndexStatus()> queryIndex;      // 打开/刷新索引页时调用
    std::function<EverythingStatus()> queryEverything;  // 文件搜索来源状态
    std::function<AboutInfo()> queryAbout;        // 关于页信息（版本/系统/路径）
    std::function<void()> onClearIndex;           // 删除快照 + 清空内存索引
    std::function<void()> onRebuildIndex;         // 立即后台重建
    std::function<void()> onOpenIndexDirectory;   // 用资源管理器打开快照目录
    std::function<bool()> onOpenLog;              // 打开当前日志文件（返回是否成功）
  };

  ~SettingsWindow();

  bool Show(HINSTANCE instance, HWND owner, const AppSettings& current, Callbacks callbacks);
  bool visible() const { return hwnd_ != nullptr; }
  HWND hwnd() const { return hwnd_; }
  // 0 = 常规，1 = 索引，2 = 筛选，3 = 关于（探针用）
  int page() const { return currentPage_; }

 private:
  static LRESULT CALLBACK Thunk(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
  LRESULT Handle(UINT message, WPARAM wParam, LPARAM lParam);
  void CreateControls();
  void Populate();
  // 应用配置；close = true 时应用后关闭窗口（保存），false 时留在窗口里（应用）。
  bool ApplyConfiguration(bool close);
  void SetPage(int page);
  void RefreshIndexInfo();
  // 按 pendingFileSearch_ 切换「本地索引」专用控件的可用性并更新来源按钮文案。
  void UpdateSearchSourceLayout();
  // 关于页：把 AboutInfo 渲染成多行文本（也用于「复制版本信息」）。
  std::wstring BuildAboutText() const;
  void RefreshAboutInfo();
  void CopyAboutInfo();
  // 筛选页：重建组列表、编辑器与工作副本之间的同步。
  void RebuildFilterGroups();
  void LoadFilterGroupIntoEditors();
  void CommitFilterEdits();
  void UpdateFilterControlState();
  FilterGroup* SelectedFilterGroup();
  // 重建索引目录下拉项：当前值 + 默认目录 + 最近使用（只收仍然存在的目录）。
  void RebuildIndexDirChoices(const std::wstring& selected);
  std::wstring DefaultIndexDir() const;
  std::wstring SelectedIndexDir() const;
  void RememberIndexDir(const std::wstring& directory);
  void AddRootViaDialog();
  void RemoveSelectedRoot();
  std::wstring PickFolder(const wchar_t* title);
  bool ValidateIndexDirectory(const std::wstring& path, std::wstring* error) const;
  void DrawButton(const DRAWITEMSTRUCT* item);
  void DrawComboItem(const DRAWITEMSTRUCT* item);
  int Scale(int value) const;

  HWND hwnd_ = nullptr;
  HWND owner_ = nullptr;

  // 页签
  HWND tabGeneral_ = nullptr;
  HWND tabIndex_ = nullptr;
  HWND tabFilter_ = nullptr;
  HWND tabAbout_ = nullptr;
  std::vector<HWND> generalControls_;
  std::vector<HWND> indexControls_;
  std::vector<HWND> filterControls_;
  std::vector<HWND> aboutControls_;

  // 常规页
  HWND themeButton_ = nullptr;
  HWND accentButton_ = nullptr;
  HWND hotkeyCapture_ = nullptr;
  HWND autostartCheck_ = nullptr;
  HWND logLevelCheck_ = nullptr;  // 「详细日志」勾选：info <-> debug
  HWND openLogButton_ = nullptr;  // 打开 Lume.log

  // 索引页
  HWND searchSourceButton_ = nullptr;  // 文件搜索来源（本地索引 / Everything）
  HWND capEdit_ = nullptr;
  HWND indexDirCombo_ = nullptr;  // 索引目录下拉框（不可编辑，只选）
  HWND indexInfo_ = nullptr;
  HWND rootsList_ = nullptr;
  HWND clearIndexButton_ = nullptr;
  HWND rebuildIndexButton_ = nullptr;
  // 仅在「本地索引」来源下可用的控件（Everything 模式下整体禁用）。
  std::vector<HWND> localOnlyControls_;
  std::wstring pendingFileSearch_ = L"local";  // 待应用的来源（点击后立即，应用/保存才生效）

  // 关于页
  HWND aboutInfo_ = nullptr;
  HWND copyInfoButton_ = nullptr;
  HWND aboutHint_ = nullptr;
  std::vector<std::wstring> indexDirChoices_;  // 下拉项对应的真实目录
  std::vector<std::wstring> indexDirHistory_;  // 本次编辑中的最近目录（≤5，应用后持久化）

  // 筛选页（工作副本，点「应用 / 保存」才写回 settings_）
  HWND filterEnableCheck_ = nullptr;
  HWND filterGroupsList_ = nullptr;
  HWND filterNameEdit_ = nullptr;
  HWND filterRulesEdit_ = nullptr;
  HWND filterAppsCheck_ = nullptr;
  HWND filterFoldersCheck_ = nullptr;
  HWND filterNoExtCheck_ = nullptr;
  HWND filterAddButton_ = nullptr;
  HWND filterRemoveButton_ = nullptr;
  HWND filterUpButton_ = nullptr;
  HWND filterDownButton_ = nullptr;
  HWND filterResetButton_ = nullptr;
  HWND filterHint_ = nullptr;
  FilterSet filters_;
  int filterSelection_ = 0;  // 0 = 内置「全部」

  // 共用
  HWND status_ = nullptr;
  HFONT font_ = nullptr;
  HBRUSH backgroundBrush_ = nullptr;
  HBRUSH controlBrush_ = nullptr;
  int dpi_ = 96;
  int themeIndex_ = 0;
  int accentIndex_ = 0;
  int currentPage_ = 0;
  bool hotkeyConflict_ = false;
  AppSettings settings_;
  std::vector<std::wstring> roots_;
  Callbacks callbacks_;
};
