#pragma once

#include <windows.h>

#include <cstdint>
#include <string>
#include <vector>

#include "search/file_hit.h"

// Everything 客户端的连接状态（设置窗 / 日志用）。
struct EverythingStatus {
  bool available = false;  // 找到 Everything 且 IPC 可用
  bool dbLoaded = false;   // Everything 索引已就绪（未就绪时发查询会被取消）
  std::wstring version;    // "1.4.1.1028"；不可用时为空
  std::wstring error;      // 不可用原因（中文，可直接进状态块）
};

// Everything_GetLastError 的中文映射。
std::wstring EverythingErrorText(DWORD code);

// Everything 文件搜索后端。客户端是官方 SDK 源码（third_party/everything），静态编入 exe；
// 使用非阻塞模式：Request 立即返回，结果以 WM_COPYDATA 回到 Attach 登记的窗口。
class EverythingIndex {
 public:
  EverythingIndex() = default;
  ~EverythingIndex();
  EverythingIndex(const EverythingIndex&) = delete;
  EverythingIndex& operator=(const EverythingIndex&) = delete;

  // 登记接收查询回复的窗口（必须是本进程 UI 线程上的窗口）；会放开 WM_COPYDATA 过滤。
  void Attach(HWND hwnd);
  void Detach();

  EverythingStatus Status() const;

  // 发送非阻塞查询。generation 会通过 SetReplyID 编进回复标记，用于丢弃过期回复。
  // 返回 false = Everything 不可用（未发送）。
  bool Request(const std::wstring& query, uint32_t generation);

  // 在窗口过程里调用：若是本次查询的回复，则把结果写入 hits（≤ kMaxFileHits 条）。
  bool HandleReply(UINT message, WPARAM wParam, LPARAM lParam, uint32_t* generation,
                   std::vector<FileHit>* hits);

  // 诊断用：阻塞查询（--everything-check / --self-check），自行完成一次同步往返。
  bool QueryBlocking(const std::wstring& query, std::vector<FileHit>* hits, uint64_t* elapsedMs);

  // 把用户输入净化成 Everything 的"全词 AND + 字面匹配"查询：
  // 去掉 & | ! < > * ? " 等语法字符，含 : 或路径分隔符的词用引号包成字面量。
  static std::wstring SanitizeQuery(const std::wstring& raw);

 private:
  void ApplySearchState(const std::wstring& cleaned, uint32_t generation, bool needsReplyWindow);
  void CollectHits(std::vector<FileHit>* hits) const;

  HWND hwnd_ = nullptr;
  uint32_t pendingGeneration_ = 0;
  std::wstring lastNeedleLower_;
};
