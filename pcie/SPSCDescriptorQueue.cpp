#include "SPSCDescriptorQueue.h"
#include <cstdlib>
#include <algorithm>

SPSCDescriptorQueue::SPSCDescriptorQueue()
    : capacity_(0), mask_(0), buffer_(nullptr), get_index_(0),
      put_index_(0) {}

SPSCDescriptorQueue::~SPSCDescriptorQueue() {
  if (buffer_)
    std::free(buffer_);
}

bool SPSCDescriptorQueue::Init(uint32_t capacity_hint) {
  if (buffer_)
    std::free(buffer_);
  uint32_t min_desc = std::max<uint32_t>(1, capacity_hint / 64);
  uint32_t capacity = 1;
  while (capacity < min_desc) {
    capacity <<= 1;
  }

  buffer_ = static_cast<BufferDescriptor *>(
      std::malloc(sizeof(BufferDescriptor) * capacity));
  if (!buffer_)
    return false;
  capacity_ = capacity;
  mask_ = capacity - 1;
  Reset();
  return true;
}

void SPSCDescriptorQueue::Reset() {
  get_index_.store(0, std::memory_order_relaxed);
  put_index_.store(0, std::memory_order_relaxed);
}

bool SPSCDescriptorQueue::PutDescriptor(const BufferDescriptor &desc) {
  auto current_put = put_index_.load(std::memory_order_relaxed);
  auto current_get = get_index_.load(std::memory_order_acquire);

  uint64_t free_slots = capacity_ - (current_put - current_get);
  if (free_slots == 0) return false;

  uint32_t idx = static_cast<uint32_t>(current_put) & mask_;
  buffer_[idx] = desc;

  put_index_.store(current_put + 1, std::memory_order_release);
  return true;
}

bool SPSCDescriptorQueue::GetDescriptor(BufferDescriptor &out) {
  auto current_get = get_index_.load(std::memory_order_relaxed);
  auto current_put = put_index_.load(std::memory_order_acquire);

  if (current_get == current_put) return false;

  uint32_t idx = static_cast<uint32_t>(current_get) & mask_;
  out = buffer_[idx];
  get_index_.store(current_get + 1, std::memory_order_release);
  return true;
}

bool SPSCDescriptorQueue::IsEmpty() const {
  auto current_get = get_index_.load(std::memory_order_acquire);
  auto current_put = put_index_.load(std::memory_order_acquire);
  return current_get == current_put;
}

uint32_t SPSCDescriptorQueue::GetDataSize() const {
  auto current_get = get_index_.load(std::memory_order_acquire);
  auto current_put = put_index_.load(std::memory_order_acquire);
  return static_cast<uint32_t>(current_put - current_get);
}
