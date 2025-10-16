#ifndef VIRTUAL_MEMORY_BUFFER_H
#define VIRTUAL_MEMORY_BUFFER_H

#include <cstddef>

#ifdef _WIN32
#include <Windows.h>
#else
#include <cstdlib>
#include <memory>
#endif

class VirtualMemoryBuffer {
public:
  explicit VirtualMemoryBuffer(std::size_t size) : buffer_(nullptr), size_(0) {
#ifdef _WIN32
    buffer_ = VirtualAlloc(nullptr, size, MEM_COMMIT | MEM_RESERVE,
                           PAGE_READWRITE);
    size_ = buffer_ ? size : 0;
#else
    constexpr std::size_t alignment = 4096;
    void *ptr = nullptr;
    if (posix_memalign(&ptr, alignment, size) == 0) {
      buffer_ = ptr;
      size_ = size;
    }
#endif
  }

  ~VirtualMemoryBuffer() {
    if (buffer_) {
#ifdef _WIN32
      VirtualFree(buffer_, 0, MEM_RELEASE);
#else
      std::free(buffer_);
#endif
      buffer_ = nullptr;
      size_ = 0;
    }
  }

  VirtualMemoryBuffer(const VirtualMemoryBuffer &) = delete;
  VirtualMemoryBuffer &operator=(const VirtualMemoryBuffer &) = delete;

  VirtualMemoryBuffer(VirtualMemoryBuffer &&other) noexcept
      : buffer_(other.buffer_), size_(other.size_) {
    other.buffer_ = nullptr;
    other.size_ = 0;
  }

  VirtualMemoryBuffer &operator=(VirtualMemoryBuffer &&other) noexcept {
    if (this != &other) {
      Reset();
      buffer_ = other.buffer_;
      size_ = other.size_;
      other.buffer_ = nullptr;
      other.size_ = 0;
    }
    return *this;
  }

  bool isValid() const { return buffer_ != nullptr; }

  void *get() const { return buffer_; }

  std::size_t size() const { return size_; }

  template <typename T> T *as() const {
    return static_cast<T *>(buffer_);
  }

private:
  void Reset() {
    if (buffer_) {
#ifdef _WIN32
      VirtualFree(buffer_, 0, MEM_RELEASE);
#else
      std::free(buffer_);
#endif
      buffer_ = nullptr;
      size_ = 0;
    }
  }

  void *buffer_;
  std::size_t size_;
};

#endif // VIRTUAL_MEMORY_BUFFER_H
