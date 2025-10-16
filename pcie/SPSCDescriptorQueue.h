#pragma once

#include <atomic>
#include <cstdint>

struct BufferDescriptor {
  unsigned char *ptr;
  uint32_t length;
  void *context;
};

class SPSCDescriptorQueue {
public:
  SPSCDescriptorQueue();
  ~SPSCDescriptorQueue();

  bool Init(uint32_t capacity_hint);
  void Reset();

  bool PutDescriptor(const BufferDescriptor &desc);
  bool GetDescriptor(BufferDescriptor &out);

  bool IsEmpty() const;
  uint32_t GetDataSize() const;
  uint32_t Capacity() const { return capacity_; }

private:
  uint32_t capacity_;
  uint32_t mask_;
  BufferDescriptor *buffer_;
  std::atomic<uint64_t> get_index_;
  std::atomic<uint64_t> put_index_;
};
