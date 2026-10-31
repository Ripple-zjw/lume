#include "platform/autostart.h"

#include <windows.h>

#include "core/log.h"

namespace {

constexpr wchar_t kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t kValueName[] = L"eTools";

std::wstring ExePath() {
  wchar_t path[MAX_PATH] = {};
  if (GetModuleFileNameW(nullptr, path, ARRAYSIZE(path)) == 0) return {};
  return path;
}

}  // namespace

bool SetAutostart(bool enabled, std::wstring* error) {
  HKEY key = nullptr;
  if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_SET_VALUE, &key) != ERROR_SUCCESS) {
    if (error) *error = L"无法打开自启注册表项";
    return false;
  }
  LONG status = ERROR_SUCCESS;
  if (enabled) {
    const std::wstring command = L"\"" + ExePath() + L"\" --autostart";
    status = RegSetValueExW(key, kValueName, 0, REG_SZ,
                            reinterpret_cast<const BYTE*>(command.c_str()),
                            static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t)));
  } else {
    status = RegDeleteValueW(key, kValueName);
    if (status == ERROR_FILE_NOT_FOUND) status = ERROR_SUCCESS;
  }
  RegCloseKey(key);
  if (status != ERROR_SUCCESS) {
    if (error) *error = L"写入自启注册表失败";
    Log(L"自启设置失败: %ld", status);
    return false;
  }
  Log(enabled ? L"已开启开机自启" : L"已关闭开机自启");
  return true;
}

bool IsAutostartEnabled() {
  HKEY key = nullptr;
  if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS) {
    return false;
  }
  wchar_t buffer[MAX_PATH * 2] = {};
  DWORD bytes = sizeof(buffer);
  DWORD type = 0;
  const LONG status =
      RegQueryValueExW(key, kValueName, nullptr, &type, reinterpret_cast<LPBYTE>(buffer), &bytes);
  RegCloseKey(key);
  return status == ERROR_SUCCESS && type == REG_SZ && buffer[0] != L'\0';
}
