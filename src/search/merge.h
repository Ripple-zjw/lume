#pragma once

#include <string>
#include <vector>

#include "search/app_entry.h"
#include "search/file_hit.h"

class UsageStore;

// 合并应用、文件与路径直通结果，按得分排序（同分时应用在前）。
// fileHits 由调用方按当前文件搜索来源（本地索引 / Everything）收集，合并层不关心来源。
std::vector<ResultRow> BuildMergedRows(const std::vector<AppEntry>& apps,
                                       const std::vector<FileHit>& fileHits, UsageStore* usage,
                                       const std::wstring& query, size_t limit = 200,
                                       bool withPlaceholder = true);
