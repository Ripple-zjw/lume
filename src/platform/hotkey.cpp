#include "platform/hotkey.h"

#include "core/log.h"

namespace {

struct Combo {
  UINT modifiers;
  UINT virtualKey;
  const wchar_t* name;
};

const Combo kCombos[] = {
    {MOD_ALT, VK_SPACE, L"Alt+Space"},
    {MOD_CONTROL | MOD_ALT, VK_SPACE, L"Ctrl+Alt+Space"},
    {MOD_CONTROL | MOD_SHIFT, VK_SPACE, L"Ctrl+Shift+Space"},
};

}  // namespace

bool RegisterDefaultHotkey(HWND hwnd, int id, std::wstring* outName) {
  for (const Combo& combo : kCombos) {
    if (RegisterHotKey(hwnd, id, combo.modifiers | MOD_NOREPEAT, combo.virtualKey)) {
      if (outName) *outName = combo.name;
      Log(L"热键已注册: %ls", combo.name);
      return true;
    }
    Log(L"热键注册失败 (%ls): %lu", combo.name, GetLastError());
  }
  if (outName) outName->clear();
  return false;
}

void UnregisterHotkey(HWND hwnd, int id) { UnregisterHotKey(hwnd, id); }
