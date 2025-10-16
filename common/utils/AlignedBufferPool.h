#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

// Fixed-size aligned buffer pool used to avoid extra data copies in the
// producer/consumer pipeline. Multiple producers can acquire buffers while a
// single consumer releases them after DMA completes.
class AlignedBufferPool {
public:
  struct Block {
    unsigned char *ptr{nullptr};
    std::size_t size{0};
  };

  AlignedBufferPool();
  ~AlignedBufferPool();

  AlignedBufferPool(const AlignedBufferPool &) = delete;
  AlignedBufferPool &operator=(const AlignedBufferPool &) = delete;

  bool Initialize(std::size_t block_count, std::size_t block_size,
                  std::size_t alignment);

  void Reset();

  bool Acquire(Block &out, const std::atomic<bool> *stop_flag = nullptr);
  void Release(unsigned char *ptr);
  bool Owns(const unsigned char *ptr) const;

  void NotifyAll();
  void Shutdown();

  std::size_t BlockSize() const { return block_size_; }
  std::size_t BlockCount() const { return buffers_.size(); }

private:
  void FreeAll();

  std::vector<unsigned char *> buffers_;
  std::vector<std::size_t> free_indices_;
  std::unordered_map<const unsigned char *, std::size_t> index_by_ptr_;

  std::size_t block_size_;
  std::size_t alignment_;

  mutable std::mutex mutex_;
  std::condition_variable cv_;
  bool initialized_;
  bool shutdown_;
};

