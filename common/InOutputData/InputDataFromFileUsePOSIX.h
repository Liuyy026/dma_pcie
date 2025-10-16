#ifndef INPUT_DATA_FROM_FILE_USE_POSIX_H
#define INPUT_DATA_FROM_FILE_USE_POSIX_H

#include "InputBlock.h"
#include "OrderedDataProcessor.h"
#include "../utils/AlignedBufferPool.h"
#include <atomic>
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
  bool loop = false;
  int cpu_id = -1;
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
  };

  void ProducerThread();
  void WorkerThread(std::size_t worker_index);
  void CloseAllFiles();

  InputDataFromFileUsePOSIXInitParam init_param_;
  COrderedDataProcessor m_OrderedDataProcessor;

  std::vector<std::thread> worker_threads_;
  std::unique_ptr<std::thread> producer_thread_;

  std::mutex task_mutex_;
  std::condition_variable task_cv_;
  std::deque<ReadTask> task_queue_;
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
};

#endif // INPUT_DATA_FROM_FILE_USE_POSIX_H
