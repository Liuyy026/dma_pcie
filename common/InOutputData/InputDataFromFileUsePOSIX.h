#ifndef INPUT_DATA_FROM_FILE_USE_POSIX_H
#define INPUT_DATA_FROM_FILE_USE_POSIX_H

#include "InputBlock.h"
#include "OrderedDataProcessor.h"
#include "../utils/AlignedBufferPool.h"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct InputDataFromFileUsePOSIXInitParam {
  std::vector<std::string> file_list;
  unsigned int block_size = 512 * 1024;
  unsigned int io_request_num = 16;
  unsigned int pending_slack = 0;
  bool use_streaming_reader = false;
  bool loop = false;
  int cpu_id = -1;
  int cpu_id_span = 0;                       // >0 时将 worker 线程分布到 [cpu_id, cpu_id + span)
  std::vector<int> worker_cpu_ids;           // 若非空，逐线程指定 CPU，长度不足时其余线程不绑核
  int producer_cpu_id = -1;                  // 若 >=0，显式指定生产者/输出线程的 CPU
};

class CInputDataFromFileUsePOSIX {
public:
  CInputDataFromFileUsePOSIX();
  ~CInputDataFromFileUsePOSIX();

  bool Init(void *pInitParam);
  void Destroy();
  void Reset();

  bool Start();
  bool Stop();
  bool IsRunning() const { return running_.load(std::memory_order_relaxed); }

  std::shared_ptr<AlignedBufferPool> GetBufferPool() const {
    return buffer_pool_;
  }

  void SetInputDataListener(IInputDataListener *listener) {
    m_OrderedDataProcessor.SetInputDataListener(listener);
  }

  std::uint64_t GetInputDataNum(void * = nullptr) const {
    return input_bytes_.load(std::memory_order_relaxed);
  }
  std::uint64_t GetOutputDataNum(void * = nullptr) const {
    return output_bytes_.load(std::memory_order_relaxed);
  }

  unsigned int GetStep() const { return step_.load(std::memory_order_relaxed); }

private:
  struct FileContext;

  struct ReadTask {
    std::shared_ptr<FileContext> context;
    std::uint64_t file_id = 0;
    std::uint64_t offset = 0;
    std::size_t length = 0;
    std::uint64_t index = 0;
    std::uint64_t request_id = 0;
  };

  void ProducerThread();
  void WorkerThread(std::size_t worker_index);
  void CloseAllFiles();
  void ResetDiagnostics();
  void LogAcquireWait(std::chrono::nanoseconds wait_ns,
                      std::uint64_t request_id,
                      std::uint64_t file_id,
                      std::size_t worker_index);
  void LogSlowLoop(std::chrono::nanoseconds loop_ns,
                   std::uint64_t request_id,
                   std::uint64_t file_id,
                   bool used_external_memory,
                   std::size_t worker_index);
  void StreamingReadLoop(std::uint64_t total_slots,
                         std::size_t min_soft_limit,
                         std::size_t configured_slack,
                         std::size_t soft_limit);
  void MaybePrefetch(const std::shared_ptr<FileContext> &context,
                     std::uint64_t upto_offset);
  bool WaitForPendingCapacity(std::size_t soft_limit);

  InputDataFromFileUsePOSIXInitParam init_param_;
  COrderedDataProcessor m_OrderedDataProcessor;

  std::vector<std::thread> worker_threads_;
  std::unique_ptr<std::thread> producer_thread_;
  std::size_t worker_thread_count_{0};

  std::mutex task_mutex_;
  std::condition_variable task_cv_;
  std::deque<ReadTask> task_queue_;
  std::mutex capacity_mutex_;
  std::condition_variable capacity_cv_;
  std::atomic<bool> stop_requested_;
  std::atomic<bool> running_;
  std::atomic<std::size_t> pending_tasks_;
  std::atomic<std::uint64_t> global_request_counter_;
  std::atomic<std::uint64_t> next_file_id_;

  std::atomic<bool> first_request_;

  std::atomic<std::uint64_t> input_bytes_;
  std::atomic<std::uint64_t> output_bytes_;
  std::atomic<unsigned int> step_;

  std::shared_ptr<AlignedBufferPool> buffer_pool_;
  struct WorkerDiagnostics {
    std::uint64_t acquire_wait_events = 0;
    std::uint64_t acquire_wait_ns = 0;
    std::uint64_t acquire_wait_ns_max = 0;
    std::uint64_t slow_loop_events = 0;
    std::uint64_t slow_loop_ns = 0;
    std::uint64_t slow_loop_ns_max = 0;
  } diagnostics_;
  std::chrono::steady_clock::time_point last_metrics_log_;
  std::mutex diagnostics_mutex_;

  bool use_streaming_reader_{false};
};

#endif // INPUT_DATA_FROM_FILE_USE_POSIX_H
