#pragma once

// 最小 COM 智能指针：只覆盖项目需要的语义，避免引入 WRL。
template <typename T>
class ComPtr {
 public:
  ComPtr() = default;
  ~ComPtr() { Reset(); }
  ComPtr(const ComPtr&) = delete;
  ComPtr& operator=(const ComPtr&) = delete;
  ComPtr(ComPtr&& other) noexcept : ptr_(other.ptr_) { other.ptr_ = nullptr; }
  ComPtr& operator=(ComPtr&& other) noexcept {
    if (this != &other) {
      Reset();
      ptr_ = other.ptr_;
      other.ptr_ = nullptr;
    }
    return *this;
  }

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
  // 接管一个已经 AddRef 的原始指针（不增加引用计数）。
  void Attach(T* ptr) {
    Reset();
    ptr_ = ptr;
  }
  T* Detach() {
    T* ptr = ptr_;
    ptr_ = nullptr;
    return ptr;
  }

 private:
  T* ptr_ = nullptr;
};
