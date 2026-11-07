#include "search/everything_index.h"

#include <algorithm>
#include <cwctype>
#include <cstdio>

// 官方 SDK 头（third_party/everything/include，见该目录 README.md）。
// 构建时通过 -DEVERYTHINGUSERAPI= 去掉 __declspec(dllimport)，改为静态链接。
#include "Everything.h"

#include "core/log.h"
#include "search/file_score.h"

namespace {

// 单次查询向 Everything 要多少条候选：先多拿一些，再用本地评分挑前 kMaxFileHits 条。
// 单次查询向 Everything 要多少条候选：先多拿一些，再用本地评分挑前 kMaxFileHits 条。
// 实测往返耗时与条数无关（本机 ~25 ms 固定开销），所以多拿不亏。
constexpr DWORD kMaxResults = 500;
// 长路径支持已开（manifest longPathAware），按 32K 字符准备缓冲区。
constexpr size_t kPathBufferChars = 32768;

std::wstring ToLower(std::wstring text) {
  for (wchar_t& ch : text) ch = static_cast<wchar_t>(std::towlower(static_cast<wint_t>(ch)));
  return text;
}

// 净化后的查询带引号（Everything 语法），但打分要用不带引号的字面词。
std::wstring StripQuotes(const std::wstring& text) {
  std::wstring result;
  result.reserve(text.size());
  for (wchar_t ch : text) {
    if (ch != L'"') result.push_back(ch);
  }
  return result;
}

}  // namespace

std::wstring EverythingErrorText(DWORD code) {
  switch (code) {
    case EVERYTHING_OK:
      return L"正常";
    case EVERYTHING_ERROR_MEMORY:
      return L"内存不足";
    case EVERYTHING_ERROR_IPC:
      return L"未检测到正在运行的 Everything（IPC 不可用）";
    case EVERYTHING_ERROR_REGISTERCLASSEX:
      return L"无法注册查询窗口类";
    case EVERYTHING_ERROR_CREATEWINDOW:
      return L"无法创建查询窗口";
    case EVERYTHING_ERROR_CREATETHREAD:
      return L"无法创建查询线程";
    case EVERYTHING_ERROR_INVALIDINDEX:
      return L"结果索引越界";
    case EVERYTHING_ERROR_INVALIDCALL:
      return L"调用顺序错误（需要先设置回复窗口）";
    case EVERYTHING_ERROR_INVALIDREQUEST:
      return L"请求数据无效";
    case EVERYTHING_ERROR_INVALIDPARAMETER:
      return L"参数无效";
    default: {
      wchar_t text[64] = {};
      swprintf(text, ARRAYSIZE(text), L"未知错误 (%lu)", code);
      return text;
    }
  }
}

EverythingIndex::~EverythingIndex() { Detach(); }

void EverythingIndex::Attach(HWND hwnd) {
  if (!hwnd) return;
  hwnd_ = hwnd;
  // Lume 以管理员身份运行时，非提权的 Everything 发来的 WM_COPYDATA 会被 UIPI 丢掉。
  ChangeWindowMessageFilterEx(hwnd_, WM_COPYDATA, 1 /*MSGFLT_ALLOW*/, nullptr);
  Everything_SetReplyWindow(hwnd_);
  Log(L"Everything: 已登记结果窗口 %p", static_cast<void*>(hwnd_));
}

void EverythingIndex::Detach() {
  if (hwnd_) {
    Everything_SetReplyWindow(nullptr);
    hwnd_ = nullptr;
  }
  Everything_CleanUp();
}

EverythingStatus EverythingIndex::Status() const {
  EverythingStatus status;
  const DWORD major = Everything_GetMajorVersion();
  if (major == 0) {
    status.error = EverythingErrorText(Everything_GetLastError());
    return status;
  }
  status.available = true;
  status.dbLoaded = Everything_IsDBLoaded() != FALSE;
  wchar_t version[64] = {};
  swprintf(version, ARRAYSIZE(version), L"%u.%u.%u.%u", static_cast<unsigned>(major),
           static_cast<unsigned>(Everything_GetMinorVersion()),
           static_cast<unsigned>(Everything_GetRevision()),
           static_cast<unsigned>(Everything_GetBuildNumber()));
  status.version = version;
  return status;
}

std::wstring EverythingIndex::SanitizeQuery(const std::wstring& raw) {
  std::wstring result;
  size_t index = 0;
  while (index < raw.size()) {
    while (index < raw.size() && std::iswspace(static_cast<wint_t>(raw[index]))) ++index;
    if (index >= raw.size()) break;

    std::wstring term;
    bool needsQuote = false;
    while (index < raw.size() && !std::iswspace(static_cast<wint_t>(raw[index]))) {
      const wchar_t ch = raw[index++];
      // Everything 的语法字符：AND/OR/NOT/分组/通配/短语引号。全部去掉，按字面处理。
      if (ch == L'&' || ch == L'|' || ch == L'!' || ch == L'<' || ch == L'>' || ch == L'*' ||
          ch == L'?' || ch == L'"') {
        continue;
      }
      // 冒号可能构成 regex:/content: 这类修饰符，路径分隔符也可能被当成路径语法。
      if (ch == L':' || ch == L'\\' || ch == L'/') needsQuote = true;
      term.push_back(ch);
    }
    if (term.empty()) continue;
    if (!result.empty()) result.push_back(L' ');
    if (needsQuote) {
      result.push_back(L'"');
      result += term;
      result.push_back(L'"');
    } else {
      result += term;
    }
  }
  return result;
}

void EverythingIndex::ApplySearchState(const std::wstring& search, const std::wstring& needleLower,
                                       uint32_t generation, bool needsReplyWindow,
                                       const FilterGroup* filter) {
  lastNeedleLower_ = needleLower;
  lastSearch_ = search;
  pendingGeneration_ = generation;
  hasFilter_ = filter != nullptr;
  if (hasFilter_) {
    activeFilter_ = *filter;
  } else {
    activeFilter_ = FilterGroup{};
  }

  Everything_Reset();
  Everything_SetSearchW(search.c_str());
  const bool pathLike =
      search.find(L'\\') != std::wstring::npos || search.find(L'/') != std::wstring::npos;
  Everything_SetMatchPath(pathLike ? TRUE : FALSE);
  Everything_SetMatchCase(FALSE);
  Everything_SetMatchWholeWord(FALSE);
  Everything_SetRegex(FALSE);
  Everything_SetMax(kMaxResults);
  // 按名称升序是 Everything 的最快路径（其它排序可能需要额外开启 fast sort）。
  Everything_SetSort(EVERYTHING_SORT_NAME_ASCENDING);
  Everything_SetRequestFlags(EVERYTHING_REQUEST_FULL_PATH_AND_FILE_NAME);
  if (needsReplyWindow) {
    Everything_SetReplyWindow(hwnd_);
    Everything_SetReplyID(generation);
  }
}

bool EverythingIndex::Request(const std::wstring& query, uint32_t generation,
                              const FilterGroup* filter, bool allowPushdown) {
  if (!hwnd_) return false;
  const std::wstring cleaned = SanitizeQuery(query);
  if (cleaned.empty()) return false;
  // 纯扩展名白名单可以直接下推给 Everything；含「文件夹 / 无扩展名」的组只做本地后过滤。
  std::wstring search = cleaned;
  bool pushed = false;
  if (allowPushdown && filter && FilterIsExtensionWhitelist(*filter)) {
    search += L" ext:";
    search += FilterEverythingSpec(*filter);
    pushed = true;
  }
  lastRequestHadFilter_ = pushed;
  ApplySearchState(search, ToLower(StripQuotes(cleaned)), generation, true, filter);
  const BOOL ok = Everything_Query(FALSE);
  if (!ok) {
    LogError(L"Everything 查询请求失败: %ls", EverythingErrorText(Everything_GetLastError()).c_str());
  }
  return ok != FALSE;
}

bool EverythingIndex::HandleReply(UINT message, WPARAM wParam, LPARAM lParam,
                                  uint32_t* generation, std::vector<FileHit>* hits) {
  if (!hwnd_ || message != WM_COPYDATA) return false;
  // IsQueryReply 同时校验 dwData 与当前回复 ID；不匹配的（过期）回复不会被消费。
  if (!Everything_IsQueryReply(message, wParam, lParam, pendingGeneration_)) return false;
  if (generation) *generation = pendingGeneration_;
  if (hits) {
    hits->clear();
    CollectHits(hits);
  }
  return true;
}

bool EverythingIndex::QueryBlocking(const std::wstring& query, std::vector<FileHit>* hits,
                                    uint64_t* elapsedMs, const FilterGroup* filter) {
  const std::wstring cleaned = SanitizeQuery(query);
  if (cleaned.empty()) return false;
  std::wstring search = cleaned;
  bool pushed = false;
  if (filter && FilterIsExtensionWhitelist(*filter)) {
    search += L" ext:";
    search += FilterEverythingSpec(*filter);
    pushed = true;
  }
  lastRequestHadFilter_ = pushed;
  ApplySearchState(search, ToLower(StripQuotes(cleaned)), 0, false, filter);

  LARGE_INTEGER frequency{};
  LARGE_INTEGER start{};
  LARGE_INTEGER finish{};
  QueryPerformanceFrequency(&frequency);
  QueryPerformanceCounter(&start);
  const BOOL ok = Everything_Query(TRUE);
  QueryPerformanceCounter(&finish);
  if (elapsedMs) {
    *elapsedMs = static_cast<uint64_t>(
        static_cast<double>(finish.QuadPart - start.QuadPart) * 1000.0 /
        static_cast<double>(frequency.QuadPart));
  }
  if (!ok) {
    LogError(L"Everything 阻塞查询失败: %ls", EverythingErrorText(Everything_GetLastError()).c_str());
    return false;
  }
  if (hits) {
    hits->clear();
    CollectHits(hits);
  }
  return true;
}

void EverythingIndex::CollectHits(std::vector<FileHit>* hits) const {
  if (!hits) return;
  hits->clear();
  const DWORD total = Everything_GetNumResults();
  if (total == 0) return;

  std::vector<wchar_t> buffer(kPathBufferChars);
  std::vector<FileHit> candidates;
  candidates.reserve(std::min<DWORD>(total, kMaxResults));

  for (DWORD index = 0; index < total; ++index) {
    if (Everything_IsVolumeResult(index)) continue;  // 盘符/卷根不当作文件结果
    const bool isDirectory = Everything_IsFolderResult(index) != FALSE;
    buffer[0] = 0;
    if (Everything_GetResultFullPathNameW(index, buffer.data(),
                                          static_cast<DWORD>(buffer.size())) == 0) {
      continue;
    }
    std::wstring path(buffer.data());
    // 组规则总是再过滤一遍：即使 ext: 下推的语义有出入，也不会返回别的类型。
    if (hasFilter_ && !FilterMatchesPath(activeFilter_, path, isDirectory)) continue;
    const size_t slash = path.find_last_of(L"\\/");
    const std::wstring name = slash == std::wstring::npos ? path : path.substr(slash + 1);
    const std::wstring lowerName = ToLower(name);
    FileNameScore scored = ScoreNameForQuery(lowerName, lastNeedleLower_);
    bool pathOnly = false;
    if (scored.score == 0) {
      // 文件名没命中：与本地索引一样退到"路径边界处的子串"，优先级最低。
      const size_t pathScore = ScorePathOnly(ToLower(path), lastNeedleLower_);
      if (pathScore == 0) continue;
      scored.score = pathScore;
      scored.quality = 0;
      pathOnly = true;
    }

    FileHit hit;
    hit.path = std::move(path);
    hit.isDirectory = isDirectory;
    hit.score = scored.score + 40;  // 与 FileIndex::Search 的 +40 对齐
    hit.quality = scored.quality;
    hit.pathOnly = pathOnly;
    candidates.push_back(std::move(hit));
  }

  std::sort(candidates.begin(), candidates.end(), [](const FileHit& a, const FileHit& b) {
    if (a.quality != b.quality) return a.quality > b.quality;
    if (a.score != b.score) return a.score > b.score;
    if (a.isDirectory != b.isDirectory) return !a.isDirectory;
    if (a.path.size() != b.path.size()) return a.path.size() < b.path.size();
    return _wcsicmp(a.path.c_str(), b.path.c_str()) < 0;  // 全序：同查询结果稳定
  });
  if (candidates.size() > kMaxFileHits) candidates.resize(kMaxFileHits);
  *hits = std::move(candidates);
}
