#pragma once

#include <windows.h>

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "app/settings.h"

// 索引页的只读状态（由 LauncherWindow 从 FileIndex 汇总后回填）。
struct IndexStatus {
  std::wstring directory;       // 生效的快照目录（空 = 默认）
  std::wstring defaultDirectory;// 默认快照目录（%LOCALAPPDATA%\Lume 或 exe 目录）
  std::wstring snapshotPath;    // <directory>\files.idx
  uint64_t snapshotBytes = 0;   // 0 = 尚未生成
  size_t entries = 0;
  uint64_t lastRebuildDurationMs = 0;
  int limitMB = 512;
  bool rebuilding = false;
  bool snapshotWritable = true;
};

class SettingsWindow {
 public:
  using SaveCallback = std::function<void(const AppSettings&)>;

  struct Callbacks {
    SaveCallback onSave;
    std::function<IndexStatus()> queryIndex;      // 打开/刷新索引页时调用
    std::function<void()> onClearIndex;           // 删除快照 + 清空内存索引
    std::function<void()> onRebuildIndex;         // 立即后台重建
    std::function<void()> onOpenIndexDirectory;   // 用资源管理器打开快照目录
  };

  ~SettingsWindow();

  bool Show(HINSTANCE instance, HWND owner, const AppSettings& current, Callbacks callbacks);
  bool visible() const { return hwnd_ != nullptr; }
  HWND hwnd() const { return hwnd_; }
  int page() const { return currentPage_; }  // 0 = 常规，1 = 索引（探针用）

 private:
  static LRESULT CALLBACK Thunk(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
  LRESULT Handle(UINT message, WPARAM wParam, LPARAM lParam);
  void CreateControls();
  void Populate();
  // 应用配置；close = true 时应用后关闭窗口（保存），false 时留在窗口里（应用）。
  bool ApplyConfiguration(bool close);
  void SetPage(int page);
  void RefreshIndexInfo();
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
  std::vector<HWND> generalControls_;
  std::vector<HWND> indexControls_;

  // 常规页
  HWND themeButton_ = nullptr;
  HWND accentButton_ = nullptr;
  HWND hotkeyCapture_ = nullptr;
  HWND autostartCheck_ = nullptr;

  // 索引页
  HWND capEdit_ = nullptr;
  HWND indexDirCombo_ = nullptr;  // 索引目录下拉框（不可编辑，只选）
  HWND indexInfo_ = nullptr;
  HWND rootsList_ = nullptr;
  HWND clearIndexButton_ = nullptr;
  HWND rebuildIndexButton_ = nullptr;
  std::vector<std::wstring> indexDirChoices_;  // 下拉项对应的真实目录
  std::vector<std::wstring> indexDirHistory_;  // 本次编辑中的最近目录（≤5，应用后持久化）

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
