#pragma once

#include <string>

#include "search/app_entry.h"

enum class LaunchMode { Normal, Elevated, Reveal };

// 启动/定位条目；失败时通过 error 返回可读原因。
bool LaunchEntry(const AppEntry& entry, LaunchMode mode, std::wstring* error);
