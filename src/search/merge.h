#pragma once

#include <string>
#include <vector>

#include "search/app_entry.h"

class FileIndex;
class UsageStore;

// 合并应用、文件与路径直通结果，按得分排序（同分时应用在前）。
std::vector<ResultRow> BuildMergedRows(const std::vector<AppEntry>& apps, FileIndex* files,
                                       UsageStore* usage, const std::wstring& query,
                                       size_t limit = 200, bool withPlaceholder = true);
