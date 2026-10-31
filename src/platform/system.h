#pragma once

#include <windows.h>

#include <string>

struct OsVersion {
  DWORD major = 0;
  DWORD minor = 0;
  DWORD build = 0;
};

OsVersion QueryOsVersion();
// 支持范围：Windows 11 22H2（build 22621）及以上，x64。
bool IsSupportedOs();
std::wstring FormatOsVersion(const OsVersion& version);

// 进程级 Per-Monitor V2 DPI 感知；失败时回退系统 DPI 感知。
bool EnablePerMonitorDpi();

RECT WorkAreaForPoint(POINT pt);
UINT DpiForPoint(POINT pt);
UINT DpiForWindow(HWND hwnd);

bool IsSystemDarkTheme();
// 0xRRGGBB
DWORD SystemAccentColor();
