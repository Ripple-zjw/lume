#pragma once

// 最小 COM 智能指针：只覆盖项目需要的语义，避免引入 WRL。
template <typename T>
class ComPtr {
 public:
  ComPtr() = default;
  ~ComPtr() { Reset(); }
  ComPtr(const ComPtr&) = delete;
  ComPtr& operator=(const ComPtr&) = delete;

  T* Get() const { return ptr_; }
  T* operator->() const { return ptr_; }
  explicit operator bool() const { return ptr_ != nullptr; }

  T** Put() {
    Reset();
    return &ptr_;
  }
  void** PutVoid() {
    Reset();
    return reinterpret_cast<void**>(&ptr_);
  }
  void Reset() {
    if (ptr_) {
      ptr_->Release();
      ptr_ = nullptr;
    }
  }

 private:
  T* ptr_ = nullptr;
};
