#ifndef INPUT_DATA_FROM_MEMORY_H
#define INPUT_DATA_FROM_MEMORY_H

#include "InputBlock.h"
#include "OrderedDataProcessor.h"
#include "../utils/AlignedBufferPool.h"
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>

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

  void SetInputDataListener(IInputDataListener *listener) {
    m_OrderedDataProcessor.SetInputDataListener(listener);
  }

  std::uint64_t GetInputDataNum(void * = nullptr) const { return produced_bytes_.load(); }
  std::uint64_t GetOutputDataNum(void * = nullptr) const { return dispatched_bytes_.load(); }

private:
  void ProduceThreadFunc();

  InputDataFromMemoryInitParam init_param_;
  COrderedDataProcessor m_OrderedDataProcessor;

  std::unique_ptr<std::thread> produce_thread_;
  std::atomic<bool> stop_requested_;
  std::atomic<bool> running_;

  std::shared_ptr<AlignedBufferPool> buffer_pool_;

  std::atomic<std::uint64_t> produced_bytes_;
  std::atomic<std::uint64_t> dispatched_bytes_;
};

#endif // INPUT_DATA_FROM_MEMORY_H
