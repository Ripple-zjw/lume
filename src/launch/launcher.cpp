#include "launch/launcher.h"

#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>

#include <cstdio>

#include "core/com_ptr.h"
#include "core/log.h"

namespace {

std::wstring ParentDirectory(const std::wstring& path) {
  const size_t slash = path.find_last_of(L"\\/");
  if (slash == std::wstring::npos || slash == 0) return {};
  return path.substr(0, slash);
}

bool EndsWithNoCase(const std::wstring& text, const wchar_t* suffix) {
  const size_t length = wcslen(suffix);
  if (text.size() < length) return false;
  return _wcsicmp(text.c_str() + text.size() - length, suffix) == 0;
}

bool RunShellExecute(const std::wstring& file, const wchar_t* verb, const std::wstring& parameters,
                     std::wstring* error, const std::wstring& directory = std::wstring()) {
  SHELLEXECUTEINFOW info{};
  info.cbSize = sizeof(info);
  info.fMask = SEE_MASK_FLAG_NO_UI | SEE_MASK_NOASYNC;
  info.lpVerb = verb;
  info.lpFile = file.c_str();
  info.lpParameters = parameters.empty() ? nullptr : parameters.c_str();
  info.lpDirectory = directory.empty() ? nullptr : directory.c_str();
  info.nShow = SW_SHOWNORMAL;
  if (!ShellExecuteExW(&info)) {
    const DWORD lastError = GetLastError();
    if (error) {
      wchar_t buffer[96];
      swprintf(buffer, ARRAYSIZE(buffer), L"ShellExecuteEx 失败 (Win32 %lu)", lastError);
      *error = buffer;
    }
    return false;
  }
  if (!directory.empty()) {
    Log(L"已启动: %ls (工作目录 %ls)", file.c_str(), directory.c_str());
  }
  return true;
}

bool ActivateUwp(const std::wstring& aumid, std::wstring* error) {
  ComPtr<IApplicationActivationManager> manager;
  HRESULT hr = CoCreateInstance(CLSID_ApplicationActivationManager, nullptr, CLSCTX_LOCAL_SERVER,
                                __uuidof(IApplicationActivationManager), manager.PutVoid());
  if (SUCCEEDED(hr) && manager) {
    DWORD processId = 0;
    hr = manager->ActivateApplication(aumid.c_str(), nullptr, AO_NONE, &processId);
    if (SUCCEEDED(hr)) {
      Log(L"已启动 UWP 应用 pid=%lu", processId);
      return true;
    }
    LogHr(L"ActivateApplication 失败，尝试 shell 回退", hr, LogLevel::Warn);
  } else {
    LogHr(L"创建激活管理器失败，尝试 shell 回退", hr, LogLevel::Warn);
  }

  // 回退：把激活交给 shell 的 AppsFolder 命名空间（受限令牌/沙箱下常能成功）
  const std::wstring uri = L"shell:AppsFolder\\" + aumid;
  if (RunShellExecute(uri, L"open", L"", error)) {
    Log(L"已通过 shell 启动: %ls", uri.c_str());
    return true;
  }
  return false;
}

bool RevealTarget(const AppEntry& entry, std::wstring* error) {
  if (entry.IsUwp()) {
    return RunShellExecute(L"explorer.exe", L"open", L"shell:AppsFolder", error);
  }
  const std::wstring parameters = L"/select,\"" + entry.launchTarget + L"\"";
  return RunShellExecute(L"explorer.exe", L"open", parameters, error);
}

}  // namespace

bool LaunchEntry(const AppEntry& entry, LaunchMode mode, std::wstring* error) {
  if (error) error->clear();
  if (entry.launchTarget.empty()) {
    if (error) *error = L"条目没有可启动目标";
    return false;
  }

  if (entry.IsSettings()) {
    // 设置页是 shell 协议：不能定位（Shift）也不能提权（Ctrl），两种修饰键都退化成直接打开。
    if (mode != LaunchMode::Normal) {
      Log(L"系统设置页不支持定位/提权，改为直接打开: %ls", entry.launchTarget.c_str());
    }
    return OpenSettingsPage(entry.launchTarget, error);
  }

  if (mode == LaunchMode::Reveal) {
    return RevealTarget(entry, error);
  }

  if (entry.IsUwp()) {
    if (mode == LaunchMode::Elevated) {
      if (error) *error = L"商店应用不支持以管理员运行";
      return false;
    }
    return ActivateUwp(entry.launchTarget, error);
  }

  const wchar_t* verb = (mode == LaunchMode::Elevated) ? L"runas" : L"open";
  std::wstring directory;
  if (!EndsWithNoCase(entry.launchTarget, L".lnk")) {
    const DWORD attributes = GetFileAttributesW(entry.launchTarget.c_str());
    if (attributes != INVALID_FILE_ATTRIBUTES &&
        (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
      directory = ParentDirectory(entry.launchTarget);
    }
  }
  return RunShellExecute(entry.launchTarget, verb, L"", error, directory);
}

bool OpenPath(const std::wstring& path, std::wstring* error) {
  if (error) error->clear();
  if (path.empty()) {
    if (error) *error = L"路径为空";
    return false;
  }
  const DWORD attributes = GetFileAttributesW(path.c_str());
  if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
    // 直接 ShellExecute 目录有时只激活资源管理器不导航，显式让 explorer 打开该目录。
    Log(L"打开目录: %ls", path.c_str());
    return RunShellExecute(L"explorer.exe", L"open", L"\"" + path + L"\"", error);
  }
  std::wstring directory;
  if (attributes != INVALID_FILE_ATTRIBUTES) {
    // 很多游戏/程序按自身目录找 data，必须继承文件所在目录而不是启动器的 cwd。
    directory = ParentDirectory(path);
  }
  return RunShellExecute(path, L"open", L"", error, directory);
}

bool RevealPath(const std::wstring& path, std::wstring* error) {
  if (error) error->clear();
  if (path.empty()) {
    if (error) *error = L"路径为空";
    return false;
  }
  return RunShellExecute(L"explorer.exe", L"open", L"/select,\"" + path + L"\"", error);
}

bool OpenSettingsPage(const std::wstring& uri, std::wstring* error) {
  if (error) error->clear();
  if (uri.rfind(L"ms-settings:", 0) != 0) {
    if (error) *error = L"不是系统设置页链接: " + uri;
    return false;
  }
  if (RunShellExecute(uri, L"open", L"", error)) {
    Log(L"已打开设置页: %ls", uri.c_str());
    return true;
  }
  // 管理员身份运行时 Settings 常拒绝被提权进程激活，让中等完整性的 explorer 中转一次。
  std::wstring fallbackError;
  if (RunShellExecute(L"explorer.exe", L"open", L"\"" + uri + L"\"", &fallbackError)) {
    Log(L"已通过 explorer 打开设置页: %ls", uri.c_str());
    return true;
  }
  if (error) *error = L"系统设置页打开失败: " + fallbackError;
  return false;
}
