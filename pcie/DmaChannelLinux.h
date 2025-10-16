#pragma once

#include "DmaChannelCommon.h"
#include "Logger.h"
#include "SPSCMemoryQueue.h"
#include "SPSCDescriptorQueue.h"
#include <atomic>
#include <condition_variable>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "AlignedBufferPool.h"

class DmaChannelLinux {
public:
  DmaChannelLinux(int device_fd, unsigned int cpu_id);
  ~DmaChannelLinux();

  DmaChannelLinux(const DmaChannelLinux &) = delete;
  DmaChannelLinux &operator=(const DmaChannelLinux &) = delete;

  bool Initialize(unsigned int dmaSize, unsigned int queueSize);
  bool Start();
  bool Stop();
  bool IsRunning() const;
  bool Reset();
  bool Send(unsigned char *data, unsigned int length);
  void SetBufferPool(std::shared_ptr<AlignedBufferPool> pool);
  DmaChannelStatus GetStatus() const;

private:
  void SendThreadFunc();
  void UpdateStatus();
  bool HasPendingData() const;
  void DrainDescriptorQueue();
  void ReleaseDescriptor(const BufferDescriptor &desc);

  int device_fd_;
  unsigned int cpu_id_;
  unsigned int dma_size_;
  unsigned int queue_size_;

  SPSCMemoryQueue send_queue_;
  // Descriptor queue used when DESCRIPTOR_QUEUE_MODE is enabled
  SPSCDescriptorQueue desc_queue_;
  uint32_t desc_queue_capacity_;
  std::shared_ptr<AlignedBufferPool> buffer_pool_;

  std::atomic<bool> stop_flag_;
  std::thread worker_thread_;
  std::mutex data_mutex_;
  std::condition_variable data_cv_;
  std::atomic<ThreadState> thread_state_;
  std::atomic<int> step_;

  std::atomic<std::uint64_t> total_sent_;
  std::atomic<std::uint64_t> last_sent_;
  // 时间戳（纳秒），用于基于实际时间间隔计算速率
  std::atomic<std::uint64_t> last_update_ns_;
  // Diagnostic counters (atomic to update from worker thread)
  std::atomic<std::uint64_t> desc_write_count_{0};
  std::atomic<std::uint64_t> desc_write_ns_total_{0};
  std::atomic<std::uint64_t> copy_write_count_{0};
  std::atomic<std::uint64_t> copy_write_ns_total_{0};
  DmaChannelStatus status_;
  std::thread monitor_thread_;
  std::atomic<bool> monitor_stop_flag_;
};
