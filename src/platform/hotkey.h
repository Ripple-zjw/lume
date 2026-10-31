#pragma once

#include <windows.h>

#include <string>

struct HotkeyBinding {
  const wchar_t* name;
  UINT modifiers;
  UINT virtualKey;
};

const HotkeyBinding* FindHotkeyBinding(const std::wstring& name);
// 默认候选（Alt+Space / Ctrl+Alt+Space / Ctrl+Shift+Space）
const HotkeyBinding* DefaultHotkeyBinding(size_t index);
size_t HotkeyBindingCount();

bool RegisterHotkeyBinding(HWND hwnd, int id, const HotkeyBinding& binding, std::wstring* error);
void UnregisterHotkey(HWND hwnd, int id);
