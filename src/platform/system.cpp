#include "platform/system.h"

#include "core/log.h"

namespace {

// 与 RTL_OSVERSIONINFOW 布局一致；不直接包含 winternl.h 以避免宏冲突。
struct RtlOsVersionInfo {
  ULONG size;
  ULONG major;
  ULONG minor;
  ULONG build;
  ULONG platformId;
  WCHAR servicePack[128];
};
using RtlGetVersionFn = LONG(WINAPI*)(RtlOsVersionInfo*);

using SetDpiAwarenessFn = BOOL(WINAPI*)(HANDLE);
using GetDpiForWindowFn = UINT(WINAPI*)(HWND);
using GetDpiForSystemFn = UINT(WINAPI*)();
using GetDpiForMonitorFn = HRESULT(WINAPI*)(HMONITOR, int, UINT*, UINT*);

constexpr int kMdtEffectiveDpi = 0;
constexpr DWORD kDefaultAccent = 0x6EA8FF;

}  // namespace

OsVersion QueryOsVersion() {
  OsVersion version;
  RtlOsVersionInfo info{};
  info.size = sizeof(info);
  auto rtlGetVersion = LoadProc<RtlGetVersionFn>(L"ntdll.dll", "RtlGetVersion");
  if (rtlGetVersion && rtlGetVersion(&info) == 0) {
    version.major = info.major;
    version.minor = info.minor;
    version.build = info.build;
    return version;
  }
  // 兜底：GetVersionEx 在新系统上会受兼容性清单影响，这里只用于日志。
  OSVERSIONINFOW fallback{};
  fallback.dwOSVersionInfoSize = sizeof(fallback);
  if (GetVersionExW(&fallback)) {
    version.major = fallback.dwMajorVersion;
    version.minor = fallback.dwMinorVersion;
    version.build = fallback.dwBuildNumber;
  }
  return version;
}

std::wstring FormatOsVersion(const OsVersion& version) {
  wchar_t buffer[64];
  swprintf(buffer, ARRAYSIZE(buffer), L"%lu.%lu.%lu", version.major, version.minor, version.build);
  return buffer;
}

bool IsSupportedOs() {
  const OsVersion version = QueryOsVersion();
  return version.major > 10 || (version.major == 10 && version.build >= 22621);
}

bool EnablePerMonitorDpi() {
  auto setContext = LoadProc<SetDpiAwarenessFn>(L"user32.dll", "SetProcessDpiAwarenessContext");
  if (setContext && setContext(reinterpret_cast<HANDLE>(-4))) {
    return true;  // PER_MONITOR_AWARE_V2
  }
  if (setContext && setContext(reinterpret_cast<HANDLE>(-3))) {
    return true;  // PER_MONITOR_AWARE
  }
  return SetProcessDPIAware() != FALSE;
}

RECT WorkAreaForPoint(POINT pt) {
  HMONITOR monitor = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
  MONITORINFO info{};
  info.cbSize = sizeof(info);
  RECT rect{};
  if (GetMonitorInfoW(monitor, &info)) {
    rect = info.rcWork;
  } else {
    rect.left = 0;
    rect.top = 0;
    rect.right = GetSystemMetrics(SM_CXSCREEN);
    rect.bottom = GetSystemMetrics(SM_CYSCREEN);
  }
  return rect;
}

UINT DpiForPoint(POINT pt) {
  HMONITOR monitor = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
  auto getDpiForMonitor = LoadProc<GetDpiForMonitorFn>(L"shcore.dll", "GetDpiForMonitor");
  if (getDpiForMonitor) {
    UINT dpiX = 96;
    UINT dpiY = 96;
    if (SUCCEEDED(getDpiForMonitor(monitor, kMdtEffectiveDpi, &dpiX, &dpiY)) && dpiX >= 48) {
      return dpiX;
    }
  }
  auto getDpiForSystem = LoadProc<GetDpiForSystemFn>(L"user32.dll", "GetDpiForSystem");
  return getDpiForSystem ? getDpiForSystem() : 96;
}

UINT DpiForWindow(HWND hwnd) {
  auto getDpiForWindow = LoadProc<GetDpiForWindowFn>(L"user32.dll", "GetDpiForWindow");
  return getDpiForWindow ? getDpiForWindow(hwnd) : 96;
}

bool IsSystemDarkTheme() {
  HKEY key = nullptr;
  if (RegOpenKeyExW(HKEY_CURRENT_USER,
                    L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", 0,
                    KEY_QUERY_VALUE, &key) == ERROR_SUCCESS) {
    DWORD value = 1;
    DWORD type = 0;
    DWORD bytes = sizeof(value);
    const LONG status =
        RegQueryValueExW(key, L"AppsUseLightTheme", nullptr, &type, reinterpret_cast<LPBYTE>(&value),
                         &bytes);
    RegCloseKey(key);
    if (status == ERROR_SUCCESS && type == REG_DWORD) {
      return value == 0;
    }
  }
  return true;  // 默认深色
}

DWORD SystemAccentColor() {
  HKEY key = nullptr;
  if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\DWM", 0, KEY_QUERY_VALUE,
                    &key) == ERROR_SUCCESS) {
    DWORD value = 0;
    DWORD type = 0;
    DWORD bytes = sizeof(value);
    const LONG status = RegQueryValueExW(key, L"AccentColor", nullptr, &type,
                                         reinterpret_cast<LPBYTE>(&value), &bytes);
    RegCloseKey(key);
    if (status == ERROR_SUCCESS && type == REG_DWORD) {
      // 存储格式为 0xAABBGGRR
      const DWORD r = value & 0xFF;
      const DWORD g = (value >> 8) & 0xFF;
      const DWORD b = (value >> 16) & 0xFF;
      if ((r + g + b) > 24) {
        return (r << 16) | (g << 8) | b;
      }
    }
  }
  return kDefaultAccent;
}
