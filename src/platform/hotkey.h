#pragma once

#include <windows.h>

#include <string>

struct HotkeyBinding {
  std::wstring name;
  UINT modifiers = 0;
  UINT virtualKey = 0;
};

const HotkeyBinding* FindHotkeyBinding(const std::wstring& name);
const HotkeyBinding* DefaultHotkeyBinding(size_t index);
size_t HotkeyBindingCount();

bool ParseHotkey(const std::wstring& text, HotkeyBinding* binding);
std::wstring FormatHotkey(UINT modifiers, UINT virtualKey);

bool RegisterHotkeyBinding(HWND hwnd, int id, const HotkeyBinding& binding, std::wstring* error);
void UnregisterHotkey(HWND hwnd, int id);
