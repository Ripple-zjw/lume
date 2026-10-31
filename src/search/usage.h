#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>

// 使用频次/最近性：用于空查询的最近/常用排序与查询加分。
class UsageStore {
 public:
  bool Load();
  bool Save() const;
  void Record(const std::wstring& target);
  size_t Bonus(const std::wstring& target) const;

 private:
  struct Entry {
    uint32_t count = 0;
    uint64_t last = 0;
  };
  std::wstring Path() const;

  std::unordered_map<std::wstring, Entry> entries_;
  mutable bool dirty_ = false;
};
