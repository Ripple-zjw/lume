#pragma once

#include <windows.h>

#include <functional>
#include <string>
#include <vector>

#include "app/settings.h"

class SettingsWindow {
 public:
  using SaveCallback = std::function<void(const AppSettings&)>;
  ~SettingsWindow();

  bool Show(HINSTANCE instance, HWND owner, const AppSettings& current, SaveCallback onSave);
  bool visible() const { return hwnd_ != nullptr; }

 private:
  static LRESULT CALLBACK Thunk(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
  LRESULT Handle(UINT message, WPARAM wParam, LPARAM lParam);
  void CreateControls();
  void Populate();
  void OnSave();
  void AddRootViaDialog();
  void RemoveSelectedRoot();
  int Scale(int value) const;

  HWND hwnd_ = nullptr;
  HWND owner_ = nullptr;
  HWND themeCombo_ = nullptr;
  HWND accentCombo_ = nullptr;
  HWND hotkeyCombo_ = nullptr;
  HWND autostartCheck_ = nullptr;
  HWND rootsList_ = nullptr;
  HWND status_ = nullptr;
  HFONT font_ = nullptr;
  int dpi_ = 96;
  AppSettings settings_;
  std::vector<std::wstring> roots_;
  SaveCallback onSave_;
};
