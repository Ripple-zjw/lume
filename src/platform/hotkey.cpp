#include "platform/hotkey.h"

#include <cstdlib>
#include <cwctype>
#include <vector>

#include "core/log.h"

namespace {

const HotkeyBinding kPresets[] = {
    {L"Alt+Space", MOD_ALT, VK_SPACE},
    {L"Ctrl+Alt+Space", MOD_CONTROL | MOD_ALT, VK_SPACE},
    {L"Ctrl+Shift+Space", MOD_CONTROL | MOD_SHIFT, VK_SPACE},
};

std::wstring ToLower(const std::wstring& text) {
  std::wstring out = text;
  for (wchar_t& ch : out) ch = static_cast<wchar_t>(std::towlower(static_cast<wint_t>(ch)));
  return out;
}

std::wstring Trim(const std::wstring& value) {
  const size_t first = value.find_first_not_of(L" \t");
  if (first == std::wstring::npos) return {};
  const size_t last = value.find_last_not_of(L" \t");
  return value.substr(first, last - first + 1);
}

std::wstring KeyName(UINT vk) {
  if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9')) {
    return std::wstring(1, static_cast<wchar_t>(vk));
  }
  if (vk >= VK_F1 && vk <= VK_F24) {
    wchar_t buffer[8];
    swprintf(buffer, ARRAYSIZE(buffer), L"F%u", vk - VK_F1 + 1);
    return buffer;
  }
  switch (vk) {
    case VK_SPACE:
      return L"Space";
    case VK_UP:
      return L"Up";
    case VK_DOWN:
      return L"Down";
    case VK_LEFT:
      return L"Left";
    case VK_RIGHT:
      return L"Right";
    case VK_HOME:
      return L"Home";
    case VK_END:
      return L"End";
    case VK_PRIOR:
      return L"PageUp";
    case VK_NEXT:
      return L"PageDown";
    case VK_TAB:
      return L"Tab";
    case VK_OEM_3:
      return L"`";
    case VK_OEM_MINUS:
      return L"-";
    case VK_OEM_PLUS:
      return L"=";
    case VK_OEM_4:
      return L"[";
    case VK_OEM_6:
      return L"]";
    case VK_OEM_5:
      return L"\\";
    case VK_OEM_1:
      return L";";
    case VK_OEM_7:
      return L"'";
    case VK_OEM_COMMA:
      return L",";
    case VK_OEM_PERIOD:
      return L".";
    case VK_OEM_2:
      return L"/";
    default:
      break;
  }
  return {};
}

bool IsModifierKey(UINT vk) {
  return vk == VK_CONTROL || vk == VK_LCONTROL || vk == VK_RCONTROL || vk == VK_MENU ||
         vk == VK_LMENU || vk == VK_RMENU || vk == VK_SHIFT || vk == VK_LSHIFT ||
         vk == VK_RSHIFT || vk == VK_LWIN || vk == VK_RWIN;
}

}  // namespace

size_t HotkeyBindingCount() { return ARRAYSIZE(kPresets); }

const HotkeyBinding* DefaultHotkeyBinding(size_t index) {
  if (index >= ARRAYSIZE(kPresets)) return nullptr;
  return &kPresets[index];
}

const HotkeyBinding* FindHotkeyBinding(const std::wstring& name) {
  for (const HotkeyBinding& binding : kPresets) {
    if (binding.name == name) return &binding;
  }
  return nullptr;
}

std::wstring FormatHotkey(UINT modifiers, UINT virtualKey) {
  std::wstring name;
  if (modifiers & MOD_CONTROL) name += L"Ctrl+";
  if (modifiers & MOD_ALT) name += L"Alt+";
  if (modifiers & MOD_SHIFT) name += L"Shift+";
  if (modifiers & MOD_WIN) name += L"Win+";
  const std::wstring key = KeyName(virtualKey);
  if (key.empty()) return name.empty() ? std::wstring() : name.substr(0, name.size() - 1);
  return name + key;
}

bool ParseHotkey(const std::wstring& text, HotkeyBinding* binding) {
  if (!binding) return false;
  binding->modifiers = 0;
  binding->virtualKey = 0;
  binding->name.clear();

  size_t start = 0;
  while (start <= text.size()) {
    const size_t plus = text.find(L'+', start);
    const std::wstring token =
        Trim(text.substr(start, plus == std::wstring::npos ? std::wstring::npos : plus - start));
    if (!token.empty()) {
      const std::wstring lower = ToLower(token);
      if (lower == L"ctrl" || lower == L"control") {
        binding->modifiers |= MOD_CONTROL;
      } else if (lower == L"alt") {
        binding->modifiers |= MOD_ALT;
      } else if (lower == L"shift") {
        binding->modifiers |= MOD_SHIFT;
      } else if (lower == L"win" || lower == L"windows") {
        binding->modifiers |= MOD_WIN;
      } else if (binding->virtualKey == 0) {
        if (token.size() == 1) {
          const wchar_t ch = static_cast<wchar_t>(std::towupper(token[0]));
          if ((ch >= L'A' && ch <= L'Z') || (ch >= L'0' && ch <= L'9')) {
            binding->virtualKey = static_cast<UINT>(ch);
          }
        }
        if (binding->virtualKey == 0) {
          if (lower == L"space") {
            binding->virtualKey = VK_SPACE;
          } else if (lower == L"up") {
            binding->virtualKey = VK_UP;
          } else if (lower == L"down") {
            binding->virtualKey = VK_DOWN;
          } else if (lower == L"left") {
            binding->virtualKey = VK_LEFT;
          } else if (lower == L"right") {
            binding->virtualKey = VK_RIGHT;
          } else if (lower == L"home") {
            binding->virtualKey = VK_HOME;
          } else if (lower == L"end") {
            binding->virtualKey = VK_END;
          } else if (lower == L"pageup") {
            binding->virtualKey = VK_PRIOR;
          } else if (lower == L"pagedown") {
            binding->virtualKey = VK_NEXT;
          } else if (lower == L"tab") {
            binding->virtualKey = VK_TAB;
          } else if (lower[0] == L'f' && lower.size() <= 3) {
            const int number = _wtoi(lower.c_str() + 1);
            if (number >= 1 && number <= 24) binding->virtualKey = VK_F1 + number - 1;
          }
        }
      }
    }
    if (plus == std::wstring::npos) break;
    start = plus + 1;
  }

  if (binding->modifiers == 0 || binding->virtualKey == 0) return false;
  if (IsModifierKey(binding->virtualKey)) return false;
  binding->name = FormatHotkey(binding->modifiers, binding->virtualKey);
  return !binding->name.empty();
}

bool RegisterHotkeyBinding(HWND hwnd, int id, const HotkeyBinding& binding, std::wstring* error) {
  if (RegisterHotKey(hwnd, id, binding.modifiers | MOD_NOREPEAT, binding.virtualKey)) {
    Log(L"热键已注册: %ls", binding.name.c_str());
    return true;
  }
  const DWORD lastError = GetLastError();
  // 注册失败最常见的原因是组合被占用（Alt+Space 就常被系统/输入法拿走），算「降级」而不是错误。
  LogWarn(L"热键注册失败 (%ls): %lu", binding.name.c_str(), lastError);
  if (error) {
    wchar_t buffer[96];
    swprintf(buffer, ARRAYSIZE(buffer), L"热键 %ls 注册失败 (Win32 %lu)", binding.name.c_str(),
             lastError);
    *error = buffer;
  }
  return false;
}

void UnregisterHotkey(HWND hwnd, int id) { UnregisterHotKey(hwnd, id); }
