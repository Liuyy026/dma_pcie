#include "AlignedBufferPool.h"

#include <algorithm>
#include <cstdlib>

AlignedBufferPool::AlignedBufferPool()
    : block_size_(0), alignment_(0), initialized_(false), shutdown_(false) {}

AlignedBufferPool::~AlignedBufferPool() { FreeAll(); }

bool AlignedBufferPool::Initialize(std::size_t block_count,
                                   std::size_t block_size,
                                   std::size_t alignment) {
  std::lock_guard<std::mutex> lock(mutex_);

  FreeAll();
  buffers_.clear();
  free_indices_.clear();
  index_by_ptr_.clear();
  shutdown_ = false;
  initialized_ = false;

  if (block_count == 0 || block_size == 0) {
    return false;
  }

  alignment_ = std::max<std::size_t>(alignment, 64);
  block_size_ = block_size;
  buffers_.resize(block_count, nullptr);
  free_indices_.reserve(block_count);

  for (std::size_t i = 0; i < block_count; ++i) {
    void *ptr = nullptr;
    int ret = posix_memalign(&ptr, alignment_, block_size_);
    if (ret != 0 || !ptr) {
      FreeAll();
      buffers_.clear();
      free_indices_.clear();
      index_by_ptr_.clear();
      return false;
    }
    buffers_[i] = static_cast<unsigned char *>(ptr);
    index_by_ptr_[buffers_[i]] = i;
    free_indices_.push_back(i);
  }

  initialized_ = true;
  cv_.notify_all();
  return true;
}

void AlignedBufferPool::Reset() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!initialized_) {
    return;
  }
  free_indices_.clear();
  free_indices_.reserve(buffers_.size());
  for (std::size_t i = 0; i < buffers_.size(); ++i) {
    free_indices_.push_back(i);
  }
  shutdown_ = false;
  cv_.notify_all();
}

bool AlignedBufferPool::Acquire(Block &out,
                                const std::atomic<bool> *stop_flag) {
  std::unique_lock<std::mutex> lock(mutex_);

  cv_.wait(lock, [&] {
    if (shutdown_) {
      return true;
    }
    if (stop_flag && stop_flag->load(std::memory_order_relaxed)) {
      return true;
    }
    return !free_indices_.empty();
  });

  if (shutdown_) {
    return false;
  }
  if (stop_flag && stop_flag->load(std::memory_order_relaxed)) {
    return false;
  }

  if (free_indices_.empty()) {
    return false;
  }

  std::size_t index = free_indices_.back();
  free_indices_.pop_back();

  out.ptr = buffers_[index];
  out.size = block_size_;
  return true;
}

void AlignedBufferPool::Release(unsigned char *ptr) {
  if (!ptr) {
    return;
  }

  std::lock_guard<std::mutex> lock(mutex_);
  auto it = index_by_ptr_.find(ptr);
  if (it == index_by_ptr_.end()) {
    return;
  }
  std::size_t index = it->second;
  free_indices_.push_back(index);
  cv_.notify_one();
}

bool AlignedBufferPool::Owns(const unsigned char *ptr) const {
  if (!ptr) {
    return false;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  return index_by_ptr_.find(ptr) != index_by_ptr_.end();
}

void AlignedBufferPool::NotifyAll() { cv_.notify_all(); }

void AlignedBufferPool::Shutdown() {
  std::lock_guard<std::mutex> lock(mutex_);
  shutdown_ = true;
  cv_.notify_all();
}

void AlignedBufferPool::FreeAll() {
  for (auto *ptr : buffers_) {
    std::free(ptr);
  }
  buffers_.clear();
  free_indices_.clear();
  index_by_ptr_.clear();
  initialized_ = false;
  shutdown_ = false;
}
