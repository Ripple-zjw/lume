#pragma once

#include <string>
#include <string_view>
#include <vector>

// 一个搜索筛选组：扩展名白名单 + 三个开关。
// 规则只在「已经命中查询」的候选上判断，不进全表扫描热路径。
struct FilterGroup {
  std::wstring name;                     // "应用程序"（≤ kMaxFilterNameLength，不含 '|'）
  std::vector<std::wstring> extensions;  // 小写、不带点："exe"、"png"
  bool includeApps = false;              // 命中应用索引条目（开始菜单 / 商店应用 / App Paths）
  bool includeFolders = false;           // 命中目录
  bool includeNoExtension = false;       // 命中无扩展名文件（README、LICENSE）
};

// 全部筛选状态（settings.conf 持久化）。groups 不含内置的「全部」。
struct FilterSet {
  bool enabled = true;  // false = 不显示芯片、不过滤（回到 v0.2.1 行为）
  int active = 0;       // 0 = 内置「全部」；1..N = groups[active - 1]
  std::vector<FilterGroup> groups;
};

constexpr size_t kMaxFilterGroups = 12;
constexpr size_t kMaxFilterNameLength = 12;
constexpr size_t kMaxFilterExtensions = 64;
constexpr size_t kMaxExtensionLength = 12;

// 默认组：应用程序（含应用索引）/ 图片 / 文档 / 音频 / 视频 / 压缩包。
FilterSet DefaultFilterSet();

// 规整一个组：名字 trim + 截断，扩展名去 */点、转小写、只留字母/数字/-/_、去重、限量。
FilterGroup SanitizeFilterGroup(FilterGroup group);

// 把编辑框里的规则文本（空格 / 逗号 / 分号分隔）拆成规整后的扩展名列表。
std::vector<std::wstring> ParseExtensionList(const std::wstring& text);

// 规整整个集合并校验；失败时不修改 *set，*error 给出可直接显示的中文原因。
bool SanitizeFilterSet(FilterSet* set, std::wstring* error);

// active 收敛到 [0, 芯片数 - 1]。
void ClampFilterActive(FilterSet* set);

// 当前生效的组；nullptr = 「全部」或筛选已关闭（= 不过滤）。
const FilterGroup* ActiveFilterGroup(const FilterSet& set);

// 芯片文案，第 0 个固定是「全部」。
std::vector<std::wstring> FilterChipLabels(const FilterSet& set);

// 单条命中是否属于该组。path 可以是完整路径，也可以只是文件名。
bool FilterMatchesPath(const FilterGroup& group, std::wstring_view path, bool isDirectory);

// 是否「纯扩展名白名单」：只有扩展名规则、没有文件夹/无扩展名开关。
// 这种组可以安全下推给 Everything 的 ext: 语法。
bool FilterIsExtensionWhitelist(const FilterGroup& group);

// "exe;png;jpg"（用于 Everything 的 ext:）。
std::wstring FilterEverythingSpec(const FilterGroup& group);

// settings.conf 编码："名字|exe;png|apps|folders|noext"（三个开关是 0/1）。
std::wstring EncodeFilterGroup(const FilterGroup& group);
bool DecodeFilterGroup(const std::wstring& value, FilterGroup* group);

// 解析 --filter=<名称|序号|all|off>：序号从 1 起（1 = 全部），名称不区分大小写。
// 成功写回 enabled/index 并返回 true；无法识别返回 false（调用方沿用原值）。
bool ParseFilterSpec(const FilterSet& set, const std::wstring& spec, bool* enabled, int* index);
