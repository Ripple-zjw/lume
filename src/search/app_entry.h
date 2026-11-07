#pragma once

#include <windows.h>

#include <string>
#include <string_view>
#include <vector>

#include "search/result.h"

enum class LaunchKind { Shell, AppUserModelId, SettingsUri };

struct AppEntry {
  std::wstring title;
  std::wstring detail;
  std::wstring launchTarget;
  std::wstring iconKey;  // SHCreateItemFromParsingName 可解析的路径/名称
  LaunchKind kind = LaunchKind::Shell;
  std::wstring glyph;  // 无图标时的首字回退
  UINT32 color = 0x6EA8FF;
  // 系统设置页的额外搜索词（中英混排）；普通应用为空，评分逻辑因此对旧行为零影响。
  std::vector<std::wstring> keywords;

  bool IsUwp() const { return kind == LaunchKind::AppUserModelId; }
  bool IsSettings() const { return kind == LaunchKind::SettingsUri; }
};

// 枚举开始菜单快捷方式、shell:AppsFolder（含商店应用）与 App Paths 注册表项，去重后按名称排序。
std::vector<AppEntry> CollectAppEntries();

// 应用索引 + 系统设置页目录（窗口、截图与诊断命令用这一份）。
std::vector<AppEntry> CollectAllEntries();

// 一次文本匹配的明细：测试与「匹配质量分层」共用。
// contiguous=true 表示整串连续命中（子串），此时 span=gaps=maxGap=0 意义不重要。
struct TextMatch {
  bool matched = false;
  bool contiguous = false;
  size_t span = 0;         // 首末命中位置的距离（连续命中 = 查询长度）
  size_t gaps = 0;         // 中间跳过的字符总数
  size_t maxGap = 0;       // 单次最大跳过数
  size_t wordStarts = 0;   // 命中落在词首（或开头）的次数
};

// 系统设置页额外支持 keywords（只认连续命中，不做模糊），并先剥掉「设置 / settings」这类
// 命名空间词；设置页不参与 detail/启动目标的兜底匹配，避免 URI 里的 ms-settings 拽出整张表。
size_t ScoreEntry(const AppEntry& entry, const std::wstring& query,
                  bool* matchedTitle = nullptr, uint8_t* quality = nullptr);
// 去掉查询里的命名空间词（设置 / settings）；当词数不足 2 或剥完为空时原样返回。
std::wstring StripNamespaceWords(const std::wstring& query);
// 小写输入上的同款评分（供文件索引复用，避免重复转小写）。
// queryLower 含空格时按「每个词都要命中」处理（与文件名口径一致）；
// 子序列命中会按间隙/跨度扣分，过于松散的模糊命中直接返回 0。
size_t ScoreLowerText(std::wstring_view text, std::wstring_view queryLower,
                      TextMatch* match = nullptr);
// 按空白切分查询（输入已小写）；空串返回空列表。
std::vector<std::wstring> SplitQueryWords(const std::wstring& queryLower);
// 返回按得分降序（同分按标题）的条目指针；空查询返回全部（按标题）。
std::vector<const AppEntry*> FilterEntries(const std::vector<AppEntry>& entries,
                                           const std::wstring& query, size_t limit = 200);
// 显示层转换；entries 为空且 withPlaceholder 时给出一条"未找到"占位行。
std::vector<ResultRow> BuildRowsFromEntries(const std::vector<const AppEntry*>& entries,
                                            bool withPlaceholder);
