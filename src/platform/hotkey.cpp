#include "platform/hotkey.h"

#include "core/log.h"

namespace {

const HotkeyBinding kBindings[] = {
    {L"Alt+Space", MOD_ALT, VK_SPACE},
    {L"Ctrl+Alt+Space", MOD_CONTROL | MOD_ALT, VK_SPACE},
    {L"Ctrl+Shift+Space", MOD_CONTROL | MOD_SHIFT, VK_SPACE},
};

}  // namespace

size_t HotkeyBindingCount() { return ARRAYSIZE(kBindings); }

const HotkeyBinding* DefaultHotkeyBinding(size_t index) {
  if (index >= ARRAYSIZE(kBindings)) return nullptr;
  return &kBindings[index];
}

const HotkeyBinding* FindHotkeyBinding(const std::wstring& name) {
  for (const HotkeyBinding& binding : kBindings) {
    if (name == binding.name) return &binding;
  }
  return nullptr;
}

bool RegisterHotkeyBinding(HWND hwnd, int id, const HotkeyBinding& binding, std::wstring* error) {
  if (RegisterHotKey(hwnd, id, binding.modifiers | MOD_NOREPEAT, binding.virtualKey)) {
    Log(L"热键已注册: %ls", binding.name);
    return true;
  }
  const DWORD lastError = GetLastError();
  Log(L"热键注册失败 (%ls): %lu", binding.name, lastError);
  if (error) {
    wchar_t buffer[96];
    swprintf(buffer, ARRAYSIZE(buffer), L"热键 %ls 注册失败 (Win32 %lu)", binding.name, lastError);
    *error = buffer;
  }
  return false;
}

void UnregisterHotkey(HWND hwnd, int id) { UnregisterHotKey(hwnd, id); }
