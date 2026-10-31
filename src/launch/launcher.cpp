#include "launch/launcher.h"

#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>

#include <cstdio>

#include "core/com_ptr.h"
#include "core/log.h"

namespace {

bool RunShellExecute(const std::wstring& file, const wchar_t* verb, const std::wstring& parameters,
                  std::wstring* error) {
  SHELLEXECUTEINFOW info{};
  info.cbSize = sizeof(info);
  info.fMask = SEE_MASK_FLAG_NO_UI | SEE_MASK_NOASYNC;
  info.lpVerb = verb;
  info.lpFile = file.c_str();
  info.lpParameters = parameters.empty() ? nullptr : parameters.c_str();
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
    LogHr(L"ActivateApplication 失败，尝试 shell 回退", hr);
  } else {
    LogHr(L"创建激活管理器失败，尝试 shell 回退", hr);
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
  return RunShellExecute(entry.launchTarget, verb, L"", error);
}
