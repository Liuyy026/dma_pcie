#ifndef INPUT_DATA_FROM_SHM_H
#define INPUT_DATA_FROM_SHM_H

#include "InputBlock.h"
#include "OrderedDataProcessor.h"
#include "../utils/AlignedBufferPool.h"
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>

class ShmRing;

struct InputDataFromSHMInitParam {
  std::string shm_name = "/pcie_shm_ring";
  uint32_t slot_count = 32;
  uint32_t slot_size = 512 * 1024;
  int cpu_id = -1;
  bool create_if_missing = false; // test env useful
};

class CInputDataFromSHM {
public:
  CInputDataFromSHM();
  ~CInputDataFromSHM();

  bool Init(void *pInitParam);
  void Destroy();
  bool Start();
  bool Stop();
  bool IsRunning() const { return running_.load(std::memory_order_relaxed); }

  void SetInputDataListener(IInputDataListener *listener) {
    m_OrderedDataProcessor.SetInputDataListener(listener);
  }

  // 当前 SHM 方案未提供零拷贝缓冲池，保持接口兼容
  std::shared_ptr<AlignedBufferPool> GetBufferPool() const { return std::shared_ptr<AlignedBufferPool>(); }

private:
  void ConsumerThreadFunc();

  InputDataFromSHMInitParam init_param_;
  COrderedDataProcessor m_OrderedDataProcessor;
  std::unique_ptr<std::thread> consumer_thread_;
  std::atomic<bool> running_;
  std::atomic<bool> stop_requested_;
  std::unique_ptr<class ShmRing> shm_;
};

#endif // INPUT_DATA_FROM_SHM_H
