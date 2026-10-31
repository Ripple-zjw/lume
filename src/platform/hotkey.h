#pragma once

#include <windows.h>

#include <string>

// 依次尝试 Alt+Space / Ctrl+Alt+Space / Ctrl+Shift+Space，成功返回 true 并给出名称。
bool RegisterDefaultHotkey(HWND hwnd, int id, std::wstring* outName);
void UnregisterHotkey(HWND hwnd, int id);
