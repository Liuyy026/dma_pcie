#ifndef INPUT_DATA_FROM_MEMORY_H
#define INPUT_DATA_FROM_MEMORY_H

#include "InputBlock.h"
#include "OrderedDataProcessor.h"
#include "../utils/AlignedBufferPool.h"
#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

struct InputDataFromMemoryInitParam {
  unsigned int block_size = 512 * 1024;
  unsigned int io_request_num = 8;
  bool loop = true;
  int cpu_id = -1;
  // 生产速度，bytes per second，0 表示尽最大速率
  std::uint64_t produce_rate_bps = 0;
  // 生成的总字节数，如果为0且 loop=true 则无限循环
  std::uint64_t total_bytes = 0;
};

class CInputDataFromMemory {
public:
  CInputDataFromMemory();
  ~CInputDataFromMemory();

  bool Init(void *pInitParam);
  void Destroy();
  void Reset();

  bool Start();
  bool Stop();
  bool IsRunning() const { return running_.load(std::memory_order_relaxed); }

  // 若存在，返回用于零拷贝的缓冲池
  std::shared_ptr<AlignedBufferPool> GetBufferPool() const { return buffer_pool_; }
  bool IsPoolPrefilled() const { return pool_prefilled_; }

  void SetInputDataListener(IInputDataListener *listener) {
    m_OrderedDataProcessor.SetInputDataListener(listener);
  }

  std::uint64_t GetInputDataNum(void * = nullptr) const { return produced_bytes_.load(); }
  std::uint64_t GetOutputDataNum(void * = nullptr) const { return dispatched_bytes_.load(); }

private:
  void ProduceThreadFunc();
  void ResetDiagnostics();
  void LogAcquireWait(std::chrono::nanoseconds wait_ns,
                      std::uint64_t request_index);
  void LogSlowLoop(std::chrono::nanoseconds loop_ns,
                   std::uint64_t request_index,
                   bool used_external_memory);
  void BuildPatternBuffer(unsigned int block_size);
  bool PrefillBufferPool();
  bool ShouldCopyIntoBlock(unsigned int data_length) const;

  struct ProducerDiagnostics {
    std::uint64_t acquire_wait_events = 0;
    std::uint64_t acquire_wait_ns = 0;
    std::uint64_t acquire_wait_ns_max = 0;
    std::uint64_t slow_loop_events = 0;
    std::uint64_t slow_loop_ns = 0;
    std::uint64_t slow_loop_ns_max = 0;
  } diagnostics_;

  std::chrono::steady_clock::time_point last_metrics_log_;

  InputDataFromMemoryInitParam init_param_;
  COrderedDataProcessor m_OrderedDataProcessor;

  std::unique_ptr<std::thread> produce_thread_;
  std::atomic<bool> stop_requested_;
  std::atomic<bool> running_;

  std::shared_ptr<AlignedBufferPool> buffer_pool_;
  std::vector<unsigned char> pattern_buffer_;
  std::size_t pool_block_count_;
  bool pool_prefilled_;

  std::atomic<std::uint64_t> produced_bytes_;
  std::atomic<std::uint64_t> dispatched_bytes_;
};

#endif // INPUT_DATA_FROM_MEMORY_H
