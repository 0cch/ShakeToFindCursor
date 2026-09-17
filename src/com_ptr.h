#pragma once
// A 30-line smart pointer, so the Task Scheduler plumbing does not drag in ATL
// (atlbase.h) or comsupp.lib. Owning semantics only: attach on creation,
// Release on destruction, and no AddRef games beyond the copy paths we use.
#include <windows.h>

namespace stfc {

template <class T>
class ComPtr {
 public:
  ComPtr() = default;
  ComPtr(std::nullptr_t) {}
  ~ComPtr() { Reset(); }

  ComPtr(const ComPtr&) = delete;
  ComPtr& operator=(const ComPtr&) = delete;

  // Takes ownership of an already add-ref'd pointer.
  void Attach(T* pointer) {
    Reset();
    pointer_ = pointer;
  }

  T** Receive() {
    Reset();
    return &pointer_;
  }

  // QIs `other` into this pointer; returns false when the interface is absent.
  template <class Other>
  bool From(Other* other, const IID& iid) {
    Reset();
    if (!other) return false;
    void* got = nullptr;
    if (FAILED(other->QueryInterface(iid, &got))) return false;
    pointer_ = static_cast<T*>(got);
    return pointer_ != nullptr;
  }

  T* Get() const { return pointer_; }
  T* operator->() const { return pointer_; }
  explicit operator bool() const { return pointer_ != nullptr; }

  void Reset() {
    if (pointer_) {
      pointer_->Release();
      pointer_ = nullptr;
    }
  }

 private:
  T* pointer_ = nullptr;
};

}  // namespace stfc
