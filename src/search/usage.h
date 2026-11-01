#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

// 一条使用记录（供“空查询推荐”用）。
struct UsageRecord {
  std::wstring target;
  uint32_t count = 0;
  uint64_t last = 0;
  size_t score = 0;  // 与 Bonus 同口径：min(120, count*6) + 近期加成
};

// 使用频次/最近性：用于空查询的最近/常用排序与查询加分。
class UsageStore {
 public:
  bool Load();
  bool Save() const;
  void Record(const std::wstring& target);
  size_t Bonus(const std::wstring& target) const;
  // 按使用热度（频次 + 近期）降序返回前 limit 条，用于空查询的推荐列表。
  std::vector<UsageRecord> Top(size_t limit) const;

  // usage.conf 的路径（%APPDATA%\Lume，不可写时退回 exe 目录）。
  static std::wstring DefaultPath();

 private:
  struct Entry {
    uint32_t count = 0;
    uint64_t last = 0;
  };

  std::unordered_map<std::wstring, Entry> entries_;
  mutable bool dirty_ = false;
};
