#pragma once

#include <windows.h>

#include <cstring>
#include <string>

// 日志等级（数字越大越啰嗦）：等级决定「写不写这一行」，文件里每行都带单字母标签。
//   Error 只记失败         Warn 失败 + 自动降级/回退
//   Info  正常使用的轨迹（默认）   Debug 排查用的细节（每次按键、鼠标悬停、Everything 往返）
enum class LogLevel : int {
  Error = 0,
  Warn = 1,
  Info = 2,
  Debug = 3,
};

// 日志同时写调试器和文件，并保留在控制台输出（进程附带控制台时，用于 --shot/--capture 等诊断）。
// 默认落在 exe 同目录的 Lume.log（免安装/绿色运行最常见）；exe 目录不可写（装进
// Program Files 之类）时退回 %LOCALAPPDATA%\Lume\logs\lume.log；--log=<路径> 可显式覆盖。
// 每次启动先写一行会话横幅（日期时间/版本/等级/PID），文件超过 2MB 时轮转为 Lume.log.1。
void LogInit(const std::wstring& path = std::wstring());
void LogShutdown();
std::wstring LogPath();

// 等级：LogInit 之前设置同样有效（横幅之外的行都按当前等级过滤）。
void LogSetLevel(LogLevel level);
LogLevel LogLevelCurrent();
bool LogEnabled(LogLevel level);
// error / warn / info / debug（配置与命令行用的规范名，始终小写 ASCII）。
const wchar_t* LogLevelName(LogLevel level);
// 无法识别时返回 false；LogLevelFromText 则宽松地退回 Info。
bool ParseLogLevel(const std::wstring& text, LogLevel* level);
LogLevel LogLevelFromText(const std::wstring& text);

// 正常使用的日志入口（Info）。
void Log(const wchar_t* format, ...);
void LogWarn(const wchar_t* format, ...);
void LogError(const wchar_t* format, ...);
void LogDebug(const wchar_t* format, ...);
// 失败调用：默认按 Error 记；非致命（DWM 属性、WIC 之类）传 Warn。
void LogHr(const wchar_t* what, HRESULT hr, LogLevel level = LogLevel::Error);

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
