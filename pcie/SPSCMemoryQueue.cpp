#include "SPSCMemoryQueue.h"

SPSCMemoryQueue::SPSCMemoryQueue()
    : size_(0), buffer_(nullptr), get_index_(0), put_index_(0) {}

SPSCMemoryQueue::~SPSCMemoryQueue()
{
  if (buffer_)
    std::free(buffer_);
}

bool SPSCMemoryQueue::Init(uint32_t size)
{
  if (buffer_)
    std::free(buffer_);
  buffer_ = static_cast<unsigned char *>(std::malloc(size));
  if (!buffer_)
    return false;

  size_ = size;
  Reset();
  return true;
}

void SPSCMemoryQueue::Reset()
{
  get_index_ = 0;
  put_index_ = 0;
}

uint32_t SPSCMemoryQueue::Put(unsigned char *data, uint32_t data_length)
{
  auto const current_put_index = put_index_.load(std::memory_order_relaxed);
  auto const current_get_index = get_index_.load(std::memory_order_acquire);

  // 实际可读取数据大小
  uint32_t put_length =
      std::min(data_length, size_ - current_put_index + current_get_index);

  // 放置数据至缓冲区尾部
  unsigned int put_to_end_length =
      std::min(put_length, size_ - (current_put_index & (size_ - 1)));
  memcpy(buffer_ + (current_put_index & (size_ - 1)), data, put_to_end_length);

  // 剩余数据从缓冲区头部开始放置
  memcpy(buffer_, data + put_to_end_length, put_length - put_to_end_length);

  put_index_.store(current_put_index + put_length, std::memory_order_release);

  return put_length;
}

bool SPSCMemoryQueue::PutFixedLength(unsigned char *data, uint32_t data_length)
{
  auto const current_put_index = put_index_.load(std::memory_order_relaxed);
  auto const current_get_index = get_index_.load(std::memory_order_acquire);

  // 空闲区域
  uint32_t free_size = size_ - current_put_index + current_get_index;

  // 空闲区域不足，不放置
  if (data_length > free_size)
    return false;

  // 放置数据至缓冲区尾部
  uint32_t put_to_end_length =
      std::min(data_length, size_ - (current_put_index & (size_ - 1)));
  memcpy(buffer_ + (current_put_index & (size_ - 1)), data, put_to_end_length);

  // 剩余数据从缓冲区头部开始放置
  memcpy(buffer_, data + put_to_end_length, data_length - put_to_end_length);

  put_index_.store(current_put_index + data_length, std::memory_order_release);
  return true;
}

uint32_t SPSCMemoryQueue::Get(unsigned char *data, uint32_t data_length)
{
  auto const current_get_index = get_index_.load(std::memory_order_relaxed);
  auto const current_put_index = put_index_.load(std::memory_order_acquire);

  // 实际可读取数据大小
  uint32_t get_length =
      std::min(data_length, current_put_index - current_get_index);

  // 读取数据直至缓冲区尾部
  uint32_t read_to_end_length =
      std::min(get_length, size_ - (current_get_index & (size_ - 1)));
  memcpy(data, buffer_ + (current_get_index & (size_ - 1)), read_to_end_length);

  // 从缓冲区头部继续读取剩余数据
  memcpy(data + read_to_end_length, buffer_, get_length - read_to_end_length);

  get_index_.store(current_get_index + get_length, std::memory_order_release);

  return get_length;
}

bool SPSCMemoryQueue::GetFixedLength(unsigned char *data, uint32_t data_length)
{
  auto const current_get_index = get_index_.load(std::memory_order_relaxed);
  auto const current_put_index = put_index_.load(std::memory_order_acquire);

  // 实际可读取数据大小
  uint32_t can_get_length = current_put_index - current_get_index;
  if (data_length > can_get_length)
    return false;

  // 读取数据直至缓冲区尾部
  uint32_t read_to_end_length =
      std::min(data_length, size_ - (current_get_index & (size_ - 1)));
  memcpy(data, buffer_ + (current_get_index & (size_ - 1)), read_to_end_length);

  // 从缓冲区头部继续读取剩余数据
  memcpy(data + read_to_end_length, buffer_, data_length - read_to_end_length);

  get_index_.store(current_get_index + data_length, std::memory_order_release);

  return true;
}

bool SPSCMemoryQueue::IsEmpty() const
{
  auto const current_get_index = get_index_.load(std::memory_order_acquire);
  auto const current_put_index = put_index_.load(std::memory_order_acquire);

  return current_get_index == current_put_index;
}

uint32_t SPSCMemoryQueue::GetDataSize() const
{
  auto const current_get_index = get_index_.load(std::memory_order_acquire);
  auto const current_put_index = put_index_.load(std::memory_order_acquire);

  return current_put_index - current_get_index;
}