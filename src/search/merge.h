#pragma once

#include <string>
#include <vector>

#include "search/app_entry.h"
#include "search/file_hit.h"
#include "search/filter.h"

class UsageStore;

// 合并应用、文件与路径直通结果，按得分排序（同分时应用在前）。
// fileHits 由调用方按当前文件搜索来源（本地索引 / Everything）收集，合并层不关心来源。
// filter 非空时再按组规则兜底过滤一次（应用行看 includeApps，文件行看扩展名）；
// 用户直接输入的完整路径不受筛选影响。
std::vector<ResultRow> BuildMergedRows(const std::vector<AppEntry>& apps,
                                       const std::vector<FileHit>& fileHits, UsageStore* usage,
                                       const std::wstring& query, size_t limit = 200,
                                       bool withPlaceholder = true,
                                       const FilterGroup* filter = nullptr);
