#include "core/log.h"

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <cwctype>
#include <vector>

#include "app/version.h"

namespace {

CRITICAL_SECTION g_lock;
bool g_lockReady = false;
FILE* g_file = nullptr;
bool g_console = false;
std::wstring g_logPath;
std::atomic<int> g_level{static_cast<int>(LogLevel::Info)};

// 超过这个大小就轮转成 <日志>.1（常驻进程正常使用一天不到 100KB，2MB 够排查好几轮）。
constexpr ULONGLONG kMaxLogBytes = 2ULL * 1024ULL * 1024ULL;

void EnsureLock() {
  if (!g_lockReady) {
    InitializeCriticalSection(&g_lock);
    g_lockReady = true;
  }
}

std::wstring ExeDirectory() {
  wchar_t path[MAX_PATH] = {};
  if (GetModuleFileNameW(nullptr, path, ARRAYSIZE(path)) == 0) return {};
  std::wstring dir(path);
  const size_t slash = dir.find_last_of(L"\\/");
  if (slash == std::wstring::npos) return {};
  dir.resize(slash);
  return dir;
}

bool FileSizeOf(const std::wstring& path, ULONGLONG* bytes) {
  WIN32_FILE_ATTRIBUTE_DATA data{};
  if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data)) return false;
  ULARGE_INTEGER size{};
  size.LowPart = data.nFileSizeLow;
  size.HighPart = data.nFileSizeHigh;
  *bytes = size.QuadPart;
  return true;
}

// 太大时改名成 <name>.1（旧的 .1 直接覆盖），避免日志无上限地长下去。
void RotateIfNeeded(const std::wstring& path) {
  ULONGLONG bytes = 0;
  if (!FileSizeOf(path, &bytes) || bytes < kMaxLogBytes) return;
  const std::wstring backup = path + L".1";
  DeleteFileW(backup.c_str());
  MoveFileExW(path.c_str(), backup.c_str(), MOVEFILE_REPLACE_EXISTING);
}

// 单行输出：`[日期 时间][等级] 正文`，同时送调试器 / 控制台 / 文件。
// 不做等级过滤（调用方负责判级），横幅也走这里。
void WriteLine(wchar_t tag, const wchar_t* message) {
  SYSTEMTIME now{};
  GetLocalTime(&now);
  wchar_t prefix[64];
  swprintf(prefix, ARRAYSIZE(prefix), L"[%04u-%02u-%02u %02u:%02u:%02u.%03u]", now.wYear,
           now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond, now.wMilliseconds);

  std::wstring text(prefix);
  text += L'[';
  text += tag;
  text += L"] ";
  text += message;

  OutputDebugStringW((text + L"\n").c_str());

  const std::string utf8 = WideToUtf8(text) + "\n";
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

wchar_t LevelTag(LogLevel level) {
  switch (level) {
    case LogLevel::Error:
      return L'E';
    case LogLevel::Warn:
      return L'W';
    case LogLevel::Debug:
      return L'D';
    case LogLevel::Info:
    default:
      return L'I';
  }
}

void LogAt(LogLevel level, const wchar_t* format, va_list args) {
  if (!LogEnabled(level)) return;
  wchar_t message[2048];
  if (vswprintf(message, ARRAYSIZE(message), format, args) < 0) return;
  WriteLine(LevelTag(level), message);
}

// 会话横幅：不按等级过滤，保证每份日志都能看出「哪次启动、什么版本、什么等级」。
void WriteBanner() {
  SYSTEMTIME now{};
  GetLocalTime(&now);
  wchar_t message[512];
  swprintf(message, ARRAYSIZE(message),
           L"===== %ls %ls 启动 %04u-%02u-%02u %02u:%02u:%02u 等级=%ls pid=%lu =====", kAppName,
           kAppVersion, now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond,
           LogLevelName(LogLevelCurrent()), static_cast<unsigned long>(GetCurrentProcessId()));
  WriteLine(L'*', message);
}

std::wstring TrimLower(const std::wstring& text) {
  std::wstring value;
  for (wchar_t ch : text) {
    if (ch == L' ' || ch == L'\t' || ch == L'"' || ch == L'\'') continue;
    value.push_back(static_cast<wchar_t>(towlower(ch)));
  }
  return value;
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

const wchar_t* LogLevelName(LogLevel level) {
  switch (level) {
    case LogLevel::Error:
      return L"error";
    case LogLevel::Warn:
      return L"warn";
    case LogLevel::Debug:
      return L"debug";
    case LogLevel::Info:
    default:
      return L"info";
  }
}

bool ParseLogLevel(const std::wstring& text, LogLevel* level) {
  if (!level) return false;
  const std::wstring value = TrimLower(text);
  if (value == L"error" || value == L"err" || value == L"e") {
    *level = LogLevel::Error;
  } else if (value == L"warn" || value == L"warning" || value == L"w") {
    *level = LogLevel::Warn;
  } else if (value == L"info" || value == L"i" || value == L"normal") {
    *level = LogLevel::Info;
  } else if (value == L"debug" || value == L"d" || value == L"verbose" || value == L"dev") {
    *level = LogLevel::Debug;
  } else {
    return false;
  }
  return true;
}

LogLevel LogLevelFromText(const std::wstring& text) {
  LogLevel level = LogLevel::Info;
  if (!ParseLogLevel(text, &level)) return LogLevel::Info;
  return level;
}

void LogSetLevel(LogLevel level) { g_level.store(static_cast<int>(level)); }

LogLevel LogLevelCurrent() { return static_cast<LogLevel>(g_level.load()); }

bool LogEnabled(LogLevel level) { return static_cast<int>(level) <= g_level.load(); }

void LogInit(const std::wstring& path) {
  EnsureLock();
  g_console = GetConsoleWindow() != nullptr;
  if (g_file) return;  // 幂等：重复调用不重复打开文件

  std::vector<std::wstring> candidates;
  const std::wstring exeDir = ExeDirectory();
  if (!path.empty()) {
    const bool bareName = path.find(L'\\') == std::wstring::npos;
    // 只给了文件名时相对于 exe 目录（双击启动时 shell 的工作目录不一定是程序目录）。
    candidates.push_back(bareName && !exeDir.empty() ? exeDir + L"\\" + path : path);
  } else {
    if (!exeDir.empty()) candidates.push_back(exeDir + L"\\Lume.log");
    const std::wstring base = LocalAppDataDir();
    if (!base.empty()) candidates.push_back(base + L"\\Lume\\logs\\lume.log");
  }

  for (const std::wstring& candidate : candidates) {
    if (candidate.empty()) continue;
    const size_t slash = candidate.find_last_of(L"\\/");
    if (slash != std::wstring::npos) EnsureDirectory(candidate.substr(0, slash));
    RotateIfNeeded(candidate);
    FILE* file = _wfopen(candidate.c_str(), L"ab");
    if (!file) {
      // 不可写（Program Files / 只读介质）就换下一个候选，不让日志静默丢失。
      OutputDebugStringW((L"[Lume] 日志文件无法创建: " + candidate + L"\n").c_str());
      continue;
    }
    g_file = file;
    g_logPath = candidate;
    break;
  }

  if (!g_file) {
    OutputDebugStringW(L"[Lume] 日志文件无法创建，仅输出到调试器/控制台\n");
    return;
  }
  WriteBanner();
  if (LogEnabled(LogLevel::Info)) Log(L"日志文件: %ls", g_logPath.c_str());
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
  va_list args;
  va_start(args, format);
  LogAt(LogLevel::Info, format, args);
  va_end(args);
}

void LogWarn(const wchar_t* format, ...) {
  va_list args;
  va_start(args, format);
  LogAt(LogLevel::Warn, format, args);
  va_end(args);
}

void LogError(const wchar_t* format, ...) {
  va_list args;
  va_start(args, format);
  LogAt(LogLevel::Error, format, args);
  va_end(args);
}

void LogDebug(const wchar_t* format, ...) {
  va_list args;
  va_start(args, format);
  LogAt(LogLevel::Debug, format, args);
  va_end(args);
}

void LogHr(const wchar_t* what, HRESULT hr, LogLevel level) {
  if (!LogEnabled(level)) return;
  wchar_t buffer[512];
  swprintf(buffer, ARRAYSIZE(buffer), L"%ls hr=0x%08lX", what, static_cast<unsigned long>(hr));
  WriteLine(LevelTag(level), buffer);
}
