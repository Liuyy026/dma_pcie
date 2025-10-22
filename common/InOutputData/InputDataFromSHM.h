#ifndef INPUT_DATA_FROM_SHM_H
#define INPUT_DATA_FROM_SHM_H

#include "InputBlock.h"
#include "OrderedDataProcessor.h"
#include "../utils/AlignedBufferPool.h"
#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
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
  void ResetDiagnostics();
  void LogAcquireWait(std::chrono::nanoseconds wait_ns,
                      std::uint64_t slot_index);
  void LogSlowLoop(std::chrono::nanoseconds loop_ns,
                   std::uint64_t slot_index);

  InputDataFromSHMInitParam init_param_;
  COrderedDataProcessor m_OrderedDataProcessor;
  std::unique_ptr<std::thread> consumer_thread_;
  std::atomic<bool> running_;
  std::atomic<bool> stop_requested_;
  std::unique_ptr<class ShmRing> shm_;
  struct ShmDiagnostics {
    std::uint64_t acquire_wait_events = 0;
    std::uint64_t acquire_wait_ns = 0;
    std::uint64_t acquire_wait_ns_max = 0;
    std::uint64_t slow_loop_events = 0;
    std::uint64_t slow_loop_ns = 0;
    std::uint64_t slow_loop_ns_max = 0;
  } diagnostics_;
  std::chrono::steady_clock::time_point last_metrics_log_;
  std::mutex diagnostics_mutex_;
};

#endif // INPUT_DATA_FROM_SHM_H
