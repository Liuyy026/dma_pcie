#include "InputDataFromFileUsePOSIX.h"
#include "../utils/Logger.h"
#include "../utils/ThreadAffinity.h"
#include "InputBlock.h"
#include <algorithm>
#include <chrono>
#include <cstring>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

struct CInputDataFromFileUsePOSIX::FileContext {
  int fd;
  std::string path;
  std::atomic<std::size_t> pending_blocks;
  bool use_odirect;

  FileContext(int file_descriptor, std::string file_path,
              std::size_t block_count, bool odirect)
      : fd(file_descriptor), path(std::move(file_path)),
        pending_blocks(block_count), use_odirect(odirect) {}

  ~FileContext() {
    if (fd >= 0) {
      ::close(fd);
      fd = -1;
    }
  }
};

CInputDataFromFileUsePOSIX::CInputDataFromFileUsePOSIX()
    : stop_requested_(false), running_(false), pending_tasks_(0),
      global_request_counter_(0), next_file_id_(1), first_request_(true),
      input_bytes_(0), output_bytes_(0), step_(0) {}

CInputDataFromFileUsePOSIX::~CInputDataFromFileUsePOSIX() { Destroy(); }

bool CInputDataFromFileUsePOSIX::Init(void *pInitParam) {
  if (!pInitParam) {
    LOG_ERROR("POSIX读取初始化参数为空");
    return false;
  }

  init_param_ =
      *static_cast<InputDataFromFileUsePOSIXInitParam *>(pInitParam);

  if (init_param_.file_list.empty()) {
    LOG_ERROR("POSIX读取文件列表为空");
    return false;
  }

  buffer_pool_.reset();
  std::size_t alignment =
      static_cast<std::size_t>(sysconf(_SC_PAGESIZE));
  std::size_t pool_block_count =
      std::max<std::size_t>(1, static_cast<std::size_t>(MAX_DISORDER_GROUPS) *
                                   init_param_.io_request_num);
  if (pool_block_count == 0) {
    pool_block_count = 1;
  }

  if (init_param_.block_size > 0) {
    auto pool = std::make_shared<AlignedBufferPool>();
    if (pool->Initialize(pool_block_count, init_param_.block_size,
                         alignment)) {
      buffer_pool_ = std::move(pool);
      m_OrderedDataProcessor.EnableExternalMemoryMode(true);
    } else {
      LOG_WARN("对齐缓冲池初始化失败，将回退到拷贝模式");
      buffer_pool_.reset();
      m_OrderedDataProcessor.EnableExternalMemoryMode(false);
    }
  } else {
    m_OrderedDataProcessor.EnableExternalMemoryMode(false);
  }

  if (!m_OrderedDataProcessor.Initialize(init_param_.io_request_num,
                                         init_param_.block_size,
                                         init_param_.cpu_id + 1)) {
    LOG_ERROR("POSIX有序数据处理器初始化失败");
    return false;
  }

  return true;
}

void CInputDataFromFileUsePOSIX::Destroy() {
  Stop();
  Reset();
  m_OrderedDataProcessor.Destroy();
  if (buffer_pool_) {
    buffer_pool_->Shutdown();
    buffer_pool_.reset();
  }
}

void CInputDataFromFileUsePOSIX::Reset() {
  {
    std::lock_guard<std::mutex> lock(task_mutex_);
    task_queue_.clear();
  }

  stop_requested_.store(false, std::memory_order_relaxed);
  running_.store(false, std::memory_order_relaxed);
  pending_tasks_.store(0, std::memory_order_relaxed);
  global_request_counter_.store(0, std::memory_order_relaxed);
  next_file_id_.store(1, std::memory_order_relaxed);
  first_request_.store(true, std::memory_order_relaxed);
  input_bytes_.store(0, std::memory_order_relaxed);
  output_bytes_.store(0, std::memory_order_relaxed);
  step_.store(0, std::memory_order_relaxed);

  m_OrderedDataProcessor.Reset();
  if (buffer_pool_) {
    buffer_pool_->Reset();
  }
}

bool CInputDataFromFileUsePOSIX::Start() {
  if (IsRunning()) {
    LOG_INFO("POSIX读取线程已在运行");
    return true;
  }

  if (init_param_.file_list.empty()) {
    LOG_ERROR("未提供任何文件路径");
    return false;
  }

  input_bytes_.store(0, std::memory_order_relaxed);
  output_bytes_.store(0, std::memory_order_relaxed);
  stop_requested_.store(false, std::memory_order_relaxed);
  first_request_.store(true, std::memory_order_relaxed);

  if (!m_OrderedDataProcessor.StartProcessing()) {
    LOG_ERROR("POSIX有序数据处理器启动失败");
    return false;
  }

  std::size_t worker_count =
      std::max(1u, init_param_.io_request_num > 0 ? init_param_.io_request_num
                                                  : 1u);

  try {
    worker_threads_.reserve(worker_count);
    for (std::size_t i = 0; i < worker_count; ++i) {
      worker_threads_.emplace_back(&CInputDataFromFileUsePOSIX::WorkerThread,
                                   this, i);
      if (init_param_.cpu_id >= 0) {
        ThreadAffinity::GetInstance().SetThreadAffinity(
            worker_threads_.back().native_handle(),
            static_cast<int>(init_param_.cpu_id));
      }
    }
    producer_thread_ = std::make_unique<std::thread>(
        &CInputDataFromFileUsePOSIX::ProducerThread, this);
    if (init_param_.cpu_id >= 0 && producer_thread_) {
      ThreadAffinity::GetInstance().SetThreadAffinity(
          producer_thread_->native_handle(),
          static_cast<int>(init_param_.cpu_id));
    }
  } catch (const std::exception &ex) {
    LOG_ERROR("启动POSIX读取线程失败: %s", ex.what());
    Stop();
    return false;
  }

  running_.store(true, std::memory_order_release);
  return true;
}

bool CInputDataFromFileUsePOSIX::Stop() {
  stop_requested_.store(true, std::memory_order_release);
  task_cv_.notify_all();
  if (buffer_pool_) {
    buffer_pool_->NotifyAll();
  }

  if (producer_thread_) {
    if (producer_thread_->joinable()) {
      producer_thread_->join();
    }
    producer_thread_.reset();
  }

  for (auto &thread : worker_threads_) {
    if (thread.joinable()) {
      thread.join();
    }
  }
  worker_threads_.clear();

  m_OrderedDataProcessor.StopProcessing();
  running_.store(false, std::memory_order_release);

  return true;
}

void CInputDataFromFileUsePOSIX::ProducerThread() {
  step_.store(0, std::memory_order_relaxed);
  const auto total_slots =
      static_cast<std::uint64_t>(MAX_DISORDER_GROUPS *
                                 init_param_.io_request_num);

  auto enqueue_task = [&](const ReadTask &task) {
    {
      std::lock_guard<std::mutex> lock(task_mutex_);
      task_queue_.push_back(task);
      pending_tasks_.fetch_add(1, std::memory_order_relaxed);
    }
    task_cv_.notify_one();
  };

  do {
    for (const auto &path : init_param_.file_list) {
      if (stop_requested_.load(std::memory_order_relaxed))
        break;

      // 尝试使用 O_DIRECT 打开文件以减少页缓存拷贝，若失败则回退
      int fd = ::open(path.c_str(), O_RDONLY | O_DIRECT);
      bool odirect = true;
      if (fd < 0) {
        // 某些文件系统或文件不支持 O_DIRECT，回退到普通打开
        fd = ::open(path.c_str(), O_RDONLY);
        odirect = false;
      }
      if (fd < 0) {
        LOG_ERROR("打开文件失败: %s, 错误: %s", path.c_str(), std::strerror(errno));
        continue;
      }

      struct stat st {
      };
      if (fstat(fd, &st) != 0) {
        LOG_ERROR("获取文件大小失败: %s, 错误: %s", path.c_str(),
                  std::strerror(errno));
        ::close(fd);
        continue;
      }

      if (st.st_size <= 0) {
        LOG_WARN("文件大小为0，跳过: %s", path.c_str());
        ::close(fd);
        continue;
      }

      std::uint64_t file_size = static_cast<std::uint64_t>(st.st_size);
      std::size_t block_count =
          static_cast<std::size_t>((file_size + init_param_.block_size - 1) /
                                   init_param_.block_size);

    auto context =
      std::make_shared<FileContext>(fd, path, block_count, odirect);
      std::uint64_t file_id =
          next_file_id_.fetch_add(1, std::memory_order_relaxed);

      std::uint64_t remaining = file_size;
      std::uint64_t offset = 0;

      while (remaining > 0 &&
             !stop_requested_.load(std::memory_order_relaxed)) {
        std::size_t read_len = static_cast<std::size_t>(
            std::min<std::uint64_t>(init_param_.block_size, remaining));

        std::uint64_t request_index =
            global_request_counter_.fetch_add(1, std::memory_order_relaxed);
        std::uint64_t slot_index =
            total_slots > 0 ? (request_index % total_slots) : 0;

        if (!first_request_.exchange(false, std::memory_order_relaxed)) {
          while (!stop_requested_.load(std::memory_order_relaxed) &&
                 slot_index ==
                     m_OrderedDataProcessor.GetNextOutputSlot()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
          }
        }

        enqueue_task(ReadTask{context, file_id, offset, read_len, slot_index});

        remaining -= read_len;
        offset += read_len;
      }

      if (stop_requested_.load(std::memory_order_relaxed)) {
        break;
      }
    }
  } while (init_param_.loop &&
           !stop_requested_.load(std::memory_order_relaxed));

  step_.store(1, std::memory_order_relaxed);

  while (pending_tasks_.load(std::memory_order_relaxed) > 0 &&
         !stop_requested_.load(std::memory_order_relaxed)) {
    step_.store(2, std::memory_order_relaxed);
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }

  stop_requested_.store(true, std::memory_order_relaxed);
  task_cv_.notify_all();
  step_.store(3, std::memory_order_relaxed);
}

void CInputDataFromFileUsePOSIX::WorkerThread(std::size_t worker_index) {
  void *aligned_buf = nullptr;
  size_t buf_size = init_param_.block_size;
  size_t page_size = static_cast<size_t>(sysconf(_SC_PAGESIZE));
  if (buf_size == 0)
    buf_size = page_size;
  int posix_ret = posix_memalign(&aligned_buf, page_size, buf_size);
  if (posix_ret != 0) {
    aligned_buf = nullptr;
  }

  std::vector<std::uint8_t> fallback_buffer;
  if (!aligned_buf) {
    fallback_buffer.resize(buf_size);
  }

  try {
    while (true) {
      ReadTask task;
      {
        std::unique_lock<std::mutex> lock(task_mutex_);
        task_cv_.wait(lock, [this] {
          return stop_requested_.load(std::memory_order_relaxed) ||
                 !task_queue_.empty();
        });

        if (task_queue_.empty()) {
          if (stop_requested_.load(std::memory_order_relaxed))
            break;
          continue;
        }

        task = task_queue_.front();
        task_queue_.pop_front();
      }

      AlignedBufferPool::Block pool_block{};
      bool pool_acquired = false;
      unsigned char *read_ptr = nullptr;
      std::size_t buffer_capacity = 0;

      if (buffer_pool_) {
        pool_acquired =
            buffer_pool_->Acquire(pool_block, &stop_requested_);
        if (pool_acquired) {
          read_ptr = pool_block.ptr;
          buffer_capacity = pool_block.size;
        }
      }

      if (!read_ptr) {
        read_ptr = aligned_buf ? static_cast<unsigned char *>(aligned_buf)
                               : fallback_buffer.data();
        buffer_capacity = aligned_buf ? buf_size : fallback_buffer.size();
      }

      if (stop_requested_.load(std::memory_order_relaxed)) {
        if (pool_acquired && buffer_pool_) {
          buffer_pool_->Release(pool_block.ptr);
        }
        pending_tasks_.fetch_sub(1, std::memory_order_relaxed);
        break;
      }

      ssize_t bytes_read = ::pread(task.context->fd, read_ptr, task.length,
                                   static_cast<off_t>(task.offset));
      if (bytes_read < 0 && errno == EINVAL && task.context->use_odirect) {
        ::close(task.context->fd);
        int fd2 = ::open(task.context->path.c_str(), O_RDONLY);
        if (fd2 >= 0) {
          task.context->fd = fd2;
          task.context->use_odirect = false;
          bytes_read = ::pread(task.context->fd, read_ptr, task.length,
                               static_cast<off_t>(task.offset));
        }
      }

      if (bytes_read < 0) {
        LOG_ERROR("读取文件失败: %s, offset=%llu, len=%zu, err=%s",
                  task.context->path.c_str(),
                  static_cast<unsigned long long>(task.offset), task.length,
                  std::strerror(errno));
        bytes_read = 0;
      }

      bool release_pool = (bytes_read <= 0);

      if (bytes_read > 0) {
        input_bytes_.fetch_add(static_cast<std::uint64_t>(bytes_read),
                               std::memory_order_relaxed);
        output_bytes_.fetch_add(static_cast<std::uint64_t>(bytes_read),
                                std::memory_order_relaxed);

        InputBlock block;
        block.buffer = read_ptr;
        block.buffer_capacity = buffer_capacity;
        block.data_length = static_cast<std::size_t>(bytes_read);
        block.index = task.index;
        block.file_id = task.file_id;
        block.file_offset = static_cast<std::int64_t>(task.offset);
        block.user_context =
            pool_acquired && buffer_pool_ ? buffer_pool_.get() : nullptr;

        if (!m_OrderedDataProcessor.ProcessData(block)) {
          LOG_ERROR("处理数据失败: file_id=%llu, offset=%llu",
                    static_cast<unsigned long long>(task.file_id),
                    static_cast<unsigned long long>(task.offset));
          release_pool = true;
        }
      }

      if (release_pool && pool_acquired && buffer_pool_) {
        buffer_pool_->Release(pool_block.ptr);
      }

      if (task.context->pending_blocks.fetch_sub(1,
                                                 std::memory_order_relaxed) ==
          1) {
        // 最后一个block处理完毕，FileContext析构时会关闭文件
      }

      pending_tasks_.fetch_sub(1, std::memory_order_relaxed);
    }
  } catch (const std::exception &ex) {
    LOG_ERROR("POSIX读取工作线程异常: %s", ex.what());
  }

  if (aligned_buf) {
    free(aligned_buf);
  }
}

void CInputDataFromFileUsePOSIX::CloseAllFiles() {
  std::lock_guard<std::mutex> lock(task_mutex_);
  task_queue_.clear();
}
