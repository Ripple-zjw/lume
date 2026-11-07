#pragma once

#include <vector>

#include "search/app_entry.h"

// Windows 设置页（ms-settings: 深链）目录。
// 系统没有公开的「枚举设置页」API，所以这里是一张按 Microsoft Learn 的 ms-settings URI 表
// 整理的静态目录：标题固定中文，关键词中英混排（英文供英文输入，也供 CI 用 ASCII 查询冒烟）。
// 硬件专属页面（电池、触摸板、Surface Dial 等）不收录，避免在无该硬件的机器上落到设置首页。

// HKCR\ms-settings 是否注册（结果缓存）。精简镜像 / Server Core 上可能没有。
bool SettingsProtocolAvailable();

// 收集设置页条目（kind = LaunchKind::SettingsUri）；协议不可用时返回空列表。
std::vector<AppEntry> CollectSettingsPages();
