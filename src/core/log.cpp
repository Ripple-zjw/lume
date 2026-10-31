#include "core/log.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <vector>

namespace {

CRITICAL_SECTION g_lock;
bool g_lockReady = false;
FILE* g_file = nullptr;
bool g_console = false;
std::wstring g_logPath;

void EnsureLock() {
  if (!g_lockReady) {
    InitializeCriticalSection(&g_lock);
    g_lockReady = true;
  }
}

}  // namespace

std::string WideToUtf8(const std::wstring& text) {
  if (text.empty()) return {};
  const int bytes = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()),
                                        nullptr, 0, nullptr, nullptr);
  if (bytes <= 0) return {};
  std::string out(static_cast<size_t>(bytes), '\0');
  WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), out.data(), bytes,
                      nullptr, nullptr);
  return out;
}

std::wstring Utf8ToWide(const std::string& text) {
  if (text.empty()) return {};
  const int chars = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()),
                                        nullptr, 0);
  if (chars <= 0) return {};
  std::wstring out(static_cast<size_t>(chars), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), out.data(), chars);
  return out;
}

bool EnsureDirectory(const std::wstring& path) {
  if (path.size() < 4) return false;
  std::wstring current;
  for (size_t i = 0; i < path.size(); ++i) {
    current.push_back(path[i]);
    const bool separator = (path[i] == L'\\' || path[i] == L'/');
    if (separator && current.size() > 3) {
      CreateDirectoryW(current.c_str(), nullptr);
    }
  }
  CreateDirectoryW(path.c_str(), nullptr);
  return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

std::wstring LocalAppDataDir() {
  wchar_t buffer[MAX_PATH * 2] = {};
  const DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", buffer, ARRAYSIZE(buffer));
  return (length > 0 && length < ARRAYSIZE(buffer)) ? std::wstring(buffer, length) : std::wstring();
}

std::wstring RoamingAppDataDir() {
  wchar_t buffer[MAX_PATH * 2] = {};
  const DWORD length = GetEnvironmentVariableW(L"APPDATA", buffer, ARRAYSIZE(buffer));
  return (length > 0 && length < ARRAYSIZE(buffer)) ? std::wstring(buffer, length) : std::wstring();
}

void LogInit(const std::wstring& path) {
  EnsureLock();
  g_console = GetConsoleWindow() != nullptr;
  if (g_file) return;  // 幂等：重复调用不重复打开文件

  std::wstring target = path;
  if (target.empty()) {
    const std::wstring base = LocalAppDataDir();
    if (!base.empty()) {
      target = base + L"\\Lume\\logs\\lume.log";
    }
  }
  if (target.empty()) return;

  const size_t slash = target.find_last_of(L"\\/");
  if (slash != std::wstring::npos) {
    EnsureDirectory(target.substr(0, slash));
  }
  FILE* file = _wfopen(target.c_str(), L"ab");
  if (!file) {
    // AppData 不可写（受限账户/绿色运行/沙箱）时退回 exe 同目录，避免日志静默丢失。
    wchar_t exePath[MAX_PATH] = {};
    if (GetModuleFileNameW(nullptr, exePath, ARRAYSIZE(exePath)) > 0) {
      std::wstring exeDir(exePath);
      const size_t slash = exeDir.find_last_of(L"\\/");
      if (slash != std::wstring::npos) {
        exeDir.resize(slash);
        const std::wstring fallback = exeDir + L"\\Lume.log";
        if (fallback != target) {
          file = _wfopen(fallback.c_str(), L"ab");
          if (file) {
            g_logPath = fallback;
          }
        }
      }
    }
  }
  if (file) {
    g_file = file;
    if (g_logPath.empty()) g_logPath = target;
    Log(L"日志文件: %ls", g_logPath.c_str());
  } else {
    OutputDebugStringW(L"[Lume] 日志文件无法创建，仅输出到调试器/控制台\n");
  }
}

std::wstring LogPath() { return g_logPath; }

void LogShutdown() {
  if (!g_lockReady) return;
  EnterCriticalSection(&g_lock);
  if (g_file) {
    fclose(g_file);
    g_file = nullptr;
  }
  LeaveCriticalSection(&g_lock);
  DeleteCriticalSection(&g_lock);
  g_lockReady = false;
}

void Log(const wchar_t* format, ...) {
  wchar_t message[2048];
  va_list args;
  va_start(args, format);
  const int written = vswprintf(message, ARRAYSIZE(message), format, args);
  va_end(args);
  if (written < 0) return;

  SYSTEMTIME now{};
  GetLocalTime(&now);
  wchar_t prefix[32];
  swprintf(prefix, ARRAYSIZE(prefix), L"[%02u:%02u:%02u.%03u] ", now.wHour, now.wMinute, now.wSecond,
           now.wMilliseconds);

  const std::wstring line = std::wstring(prefix) + message;
  OutputDebugStringW((line + L"\n").c_str());

  const std::string utf8 = WideToUtf8(line) + "\n";
  if (g_console) {
    fputs(utf8.c_str(), stdout);
    fflush(stdout);
  }

  if (!g_lockReady) return;
  EnterCriticalSection(&g_lock);
  if (g_file) {
    fwrite(utf8.data(), 1, utf8.size(), g_file);
    fflush(g_file);
  }
  LeaveCriticalSection(&g_lock);
}

void LogHr(const wchar_t* what, HRESULT hr) {
  Log(L"%-44ls hr=0x%08lX", what, static_cast<unsigned long>(hr));
}
