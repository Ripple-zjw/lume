#pragma once

#include <windows.h>

#include <cstring>
#include <string>

// 日志同时写调试器和文件（LLOCALAPPDATAL\eTools\logs\etools.log），并保留在控制台
// 输出（当进程附带控制台时，用于 --shot/--capture 等诊断模式）。
void LogInit(const std::wstring& path = std::wstring());
void LogShutdown();
std::wstring LogPath();
void Log(const wchar_t* format, ...);
void LogHr(const wchar_t* what, HRESULT hr);

// 经由 memcpy 转换 FARPROC，规避 -Wcast-function-type。
template <typename Fn>
Fn LoadProc(const wchar_t* moduleName, const char* procName) {
  static_assert(sizeof(Fn) == sizeof(FARPROC), "unexpected function pointer size");
  FARPROC proc = GetProcAddress(GetModuleHandleW(moduleName), procName);
  Fn fn = nullptr;
  if (proc) {
    std::memcpy(&fn, &proc, sizeof(fn));
  }
  return fn;
}

std::string WideToUtf8(const std::wstring& text);
std::wstring Utf8ToWide(const std::string& text);
bool EnsureDirectory(const std::wstring& path);
std::wstring LocalAppDataDir();
std::wstring RoamingAppDataDir();
