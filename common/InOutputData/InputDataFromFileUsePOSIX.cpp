#include "InputDataFromFileUsePOSIX.h"
#include "../utils/Logger.h"
#include "../utils/ThreadAffinity.h"
#include "InputBlock.h"
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <errno.h>
#include <fcntl.h>
#include <limits>
#include <sys/stat.h>
#include <unistd.h>

struct CInputDataFromFileUsePOSIX::FileContext {
  int fd;
  std::string path;
  std::atomic<std::size_t> pending_blocks;
  bool use_odirect;
  std::uint64_t file_size;
  std::uint64_t prefetched_until;
  bool prefetch_failed;

  FileContext(int file_descriptor, std::string file_path,
              std::size_t block_count, bool odirect,
              std::uint64_t size)
      : fd(file_descriptor), path(std::move(file_path)),
        pending_blocks(block_count), use_odirect(odirect),
        file_size(size), prefetched_until(0), prefetch_failed(false) {}

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
      input_bytes_(0), output_bytes_(0), step_(0) {
  ResetDiagnostics();
}

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
    LOG_INFO(
      "初始化对齐缓冲池: block_count=%zu, block_size=%u, alignment=%zu",
      pool_block_count, init_param_.block_size, alignment);
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

  int ordered_cpu = init_param_.producer_cpu_id >= 0
                        ? init_param_.producer_cpu_id
                        : init_param_.cpu_id;

  LOG_INFO(
      "初始化有序数据处理器: io_request_num=%u, block_size=%u, ordered_cpu=%d, "
      "cpu_id=%d, cpu_id_span=%d, worker_cpu_ids=%zu, external=%d",
      init_param_.io_request_num, init_param_.block_size, ordered_cpu,
      init_param_.cpu_id, init_param_.cpu_id_span,
      init_param_.worker_cpu_ids.size(),
      m_OrderedDataProcessor.IsExternalMemoryMode() ? 1 : 0);

  if (!m_OrderedDataProcessor.Initialize(init_param_.io_request_num,
                                         init_param_.block_size,
                                         ordered_cpu)) {
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
  worker_thread_count_ = 0;
  capacity_cv_.notify_all();

  ResetDiagnostics();
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
  ResetDiagnostics();

  if (!m_OrderedDataProcessor.StartProcessing()) {
    LOG_ERROR("POSIX有序数据处理器启动失败");
    return false;
  }

  auto normalize_cpu_affinity = [&]() {
    long cpu_count = ::sysconf(_SC_NPROCESSORS_ONLN);
    if (cpu_count <= 0) {
      return;
    }

    auto is_cpu_valid = [&](int cpu) {
      return cpu >= 0 && cpu < cpu_count;
    };

    if (!init_param_.worker_cpu_ids.empty()) {
      std::vector<int> filtered;
      filtered.reserve(init_param_.worker_cpu_ids.size());
      for (int cpu_id : init_param_.worker_cpu_ids) {
        if (is_cpu_valid(cpu_id)) {
          filtered.push_back(cpu_id);
        } else {
          LOG_WARN("忽略无效的 worker CPU 编号: %d (有效范围 [0, %ld))",
                   cpu_id, cpu_count);
        }
      }

      if (filtered.empty()) {
        if (!init_param_.worker_cpu_ids.empty()) {
          LOG_WARN("提供的 worker CPU 列表无有效编号，将回退为未绑定模式");
        }
        init_param_.worker_cpu_ids.clear();
      } else if (filtered.size() != init_param_.worker_cpu_ids.size()) {
        LOG_WARN("worker CPU 列表包含无效编号，已裁剪: 原=%zu, 有效=%zu",
                 init_param_.worker_cpu_ids.size(), filtered.size());
        init_param_.worker_cpu_ids = std::move(filtered);
      }
    }

    if (init_param_.cpu_id >= 0) {
      if (!is_cpu_valid(init_param_.cpu_id)) {
        LOG_WARN("cpu_id=%d 超出范围 [0, %ld)，将禁用共享 span 绑定", init_param_.cpu_id,
                 cpu_count);
        init_param_.cpu_id = -1;
        init_param_.cpu_id_span = 0;
      } else if (init_param_.cpu_id_span > 0) {
        int max_span = static_cast<int>(cpu_count - init_param_.cpu_id);
        if (max_span <= 0) {
          LOG_WARN("cpu_id=%d 没有可用的 span 范围，将禁用共享 span 绑定",
                   init_param_.cpu_id);
          init_param_.cpu_id_span = 0;
        } else if (init_param_.cpu_id_span > max_span) {
          LOG_WARN("cpu_id_span=%d 超出剩余核心数 %d，将自动裁剪",
                   init_param_.cpu_id_span, max_span);
          init_param_.cpu_id_span = max_span;
        }
      }
    }

    if (init_param_.producer_cpu_id >= 0 &&
        !is_cpu_valid(init_param_.producer_cpu_id)) {
      LOG_WARN("producer_cpu_id=%d 超出范围 [0, %ld)，将回退为未绑定",
               init_param_.producer_cpu_id, cpu_count);
      init_param_.producer_cpu_id = -1;
    }
  };

  normalize_cpu_affinity();

  use_streaming_reader_ = init_param_.use_streaming_reader;

  if (const char *env = std::getenv("PCIE_FILE_STREAMING")) {
    std::string value(env);
    if (value == "1" || value == "true" || value == "TRUE" ||
        value == "on" || value == "ON") {
      use_streaming_reader_ = true;
    } else if (value == "0" || value == "false" || value == "FALSE" ||
               value == "off" || value == "OFF") {
      use_streaming_reader_ = false;
    }
  }

  bool auto_streaming_triggered = false;
  if (!use_streaming_reader_) {
    unsigned int hw_threads = std::thread::hardware_concurrency();
    if (hw_threads == 0) {
      hw_threads = 8;
    }
    if (init_param_.io_request_num >= hw_threads * 2) {
      use_streaming_reader_ = true;
      auto_streaming_triggered = true;
    }
  }

  std::size_t worker_count = 0;
  if (!use_streaming_reader_) {
    worker_count =
        std::max(1u, init_param_.io_request_num > 0 ? init_param_.io_request_num
                                                    : 1u);

    if (!init_param_.worker_cpu_ids.empty()) {
      worker_count = std::min<std::size_t>(worker_count,
                                           init_param_.worker_cpu_ids.size());
      if (worker_count == 0) {
        worker_count = 1;
      }
    }
  }

  if (use_streaming_reader_) {
    worker_threads_.clear();
    worker_thread_count_ = 0;
    LOG_INFO("POSIX 文件读取: 启用 streaming 模式 (io_request_num=%u%s)",
             init_param_.io_request_num,
             auto_streaming_triggered ? ", auto-trigger" : "");
  }

  auto select_worker_cpu = [&](std::size_t worker_index) -> int {
    if (!init_param_.worker_cpu_ids.empty()) {
      if (worker_index < init_param_.worker_cpu_ids.size()) {
        return init_param_.worker_cpu_ids[worker_index];
      }
      return -1;
    }
    if (init_param_.cpu_id >= 0 && init_param_.cpu_id_span > 0) {
      return init_param_.cpu_id +
             static_cast<int>(worker_index %
                              static_cast<std::size_t>(init_param_.cpu_id_span));
    }
    return -1;
  };

  try {
    if (!use_streaming_reader_) {
      worker_threads_.reserve(worker_count);
      worker_thread_count_ = 0;
      for (std::size_t i = 0; i < worker_count; ++i) {
        worker_threads_.emplace_back(&CInputDataFromFileUsePOSIX::WorkerThread,
                                     this, i);
        int worker_cpu = select_worker_cpu(i);
        if (worker_cpu >= 0) {
          if (!ThreadAffinity::GetInstance().SetThreadAffinity(
                  worker_threads_.back().native_handle(), worker_cpu)) {
            LOG_WARN("设置POSIX worker线程CPU亲和性失败: worker=%zu, cpu=%d", i,
                     worker_cpu);
          }
        }
      }
      worker_thread_count_ = worker_threads_.size();
    }
    producer_thread_ = std::make_unique<std::thread>(
        &CInputDataFromFileUsePOSIX::ProducerThread, this);
    if (producer_thread_) {
      int producer_cpu = -1;
      if (init_param_.producer_cpu_id >= 0) {
        producer_cpu = init_param_.producer_cpu_id;
      } else if (init_param_.cpu_id >= 0 &&
                 init_param_.cpu_id_span == 0 &&
                 init_param_.worker_cpu_ids.empty()) {
        // 兼容旧配置：只有 cpu_id 时维持原来行为
        producer_cpu = init_param_.cpu_id;
      } else if (init_param_.cpu_id >= 0 && init_param_.cpu_id_span > 0) {
        producer_cpu = init_param_.cpu_id;
      }

      if (producer_cpu >= 0) {
        if (!ThreadAffinity::GetInstance().SetThreadAffinity(
                producer_thread_->native_handle(), producer_cpu)) {
          LOG_WARN("设置POSIX 生产线程CPU亲和性失败: cpu=%d", producer_cpu);
        }
      }
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
  capacity_cv_.notify_all();
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
  worker_thread_count_ = 0;

  m_OrderedDataProcessor.StopProcessing();
  running_.store(false, std::memory_order_release);

  return true;
}

void CInputDataFromFileUsePOSIX::ProducerThread() {
  step_.store(0, std::memory_order_relaxed);
  const auto total_slots =
      static_cast<std::uint64_t>(MAX_DISORDER_GROUPS *
                                 init_param_.io_request_num);
  std::size_t worker_count = worker_thread_count_ == 0
                                 ? static_cast<std::size_t>(init_param_.io_request_num)
                                 : worker_thread_count_;
  if (worker_count == 0) {
    worker_count = 1;
  }
  std::size_t concurrency =
      init_param_.io_request_num > 0
          ? static_cast<std::size_t>(init_param_.io_request_num)
          : worker_count;
  if (concurrency == 0) {
    concurrency = 1;
  }

  const std::size_t min_soft_limit = concurrency;
  std::size_t configured_slack = 0;
  if (init_param_.pending_slack > 0) {
    configured_slack = static_cast<std::size_t>(init_param_.pending_slack);
  } else {
    configured_slack = std::max<std::size_t>(min_soft_limit / 8, 8);
    std::size_t max_auto_slack = min_soft_limit / 2;
    if (max_auto_slack == 0) {
      max_auto_slack = 1;
    }
    configured_slack = std::min(configured_slack, max_auto_slack);
  }

  std::size_t soft_limit = min_soft_limit + configured_slack;
  if (total_slots > 0) {
    std::size_t slot_cap = static_cast<std::size_t>(total_slots);
    if (slot_cap > 0) {
      std::size_t max_allowed = slot_cap > 1 ? slot_cap - 1 : slot_cap;
      soft_limit = std::min(soft_limit, max_allowed);
    }
  }

  if (soft_limit < min_soft_limit) {
    soft_limit = min_soft_limit;
  }

  if (init_param_.pending_slack == 0) {
    LOG_INFO(
        "POSIX 生产者限流: concurrency=%zu, auto_slack=%zu, soft_limit=%zu, "
        "total_slots=%llu",
        min_soft_limit, configured_slack, soft_limit,
        static_cast<unsigned long long>(total_slots));
  } else {
    LOG_INFO(
        "POSIX 生产者限流: concurrency=%zu, configured_slack=%zu, soft_limit=%zu, "
        "total_slots=%llu",
        min_soft_limit, configured_slack, soft_limit,
        static_cast<unsigned long long>(total_slots));
  }

  if (use_streaming_reader_) {
    StreamingReadLoop(total_slots, min_soft_limit, configured_slack, soft_limit);
    return;
  }

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

#if defined(POSIX_FADV_SEQUENTIAL)
      if (!odirect) {
        ::posix_fadvise(fd, 0, 0, POSIX_FADV_SEQUENTIAL);
      }
#endif

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

    auto context = std::make_shared<FileContext>(fd, path, block_count,
                           odirect, file_size);
    MaybePrefetch(context, 0);
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

        if (!WaitForPendingCapacity(soft_limit)) {
          break;
        }

        MaybePrefetch(context, offset);

  ReadTask task{};
  task.context = context;
  task.file_id = file_id;
  task.offset = offset;
  task.length = read_len;
  task.index = slot_index;
  task.request_id = request_index;
  enqueue_task(task);

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

void CInputDataFromFileUsePOSIX::StreamingReadLoop(
    std::uint64_t total_slots,
    std::size_t min_soft_limit,
    std::size_t configured_slack,
    std::size_t soft_limit) {
  using clock = std::chrono::steady_clock;

  (void)min_soft_limit;
  (void)configured_slack;

  size_t buf_size = init_param_.block_size;
  size_t page_size = static_cast<size_t>(sysconf(_SC_PAGESIZE));
  if (buf_size == 0)
    buf_size = page_size;

  void *aligned_buf = nullptr;
  if (posix_memalign(&aligned_buf, page_size, buf_size) != 0) {
    aligned_buf = nullptr;
  }
  std::vector<std::uint8_t> fallback_buffer;
  if (!aligned_buf) {
    fallback_buffer.resize(buf_size);
  }

  auto cleanup_buffer = [&]() {
    if (aligned_buf) {
      free(aligned_buf);
      aligned_buf = nullptr;
    }
    fallback_buffer.clear();
  };

  bool stop_loop = false;

  do {
    for (const auto &path : init_param_.file_list) {
      if (stop_requested_.load(std::memory_order_relaxed)) {
        stop_loop = true;
        break;
      }

      int fd = ::open(path.c_str(), O_RDONLY | O_DIRECT);
      bool odirect = true;
      if (fd < 0) {
        fd = ::open(path.c_str(), O_RDONLY);
        odirect = false;
      }
      if (fd < 0) {
        LOG_ERROR("打开文件失败: %s, 错误: %s", path.c_str(), std::strerror(errno));
        continue;
      }

#if defined(POSIX_FADV_SEQUENTIAL)
      if (!odirect) {
        ::posix_fadvise(fd, 0, 0, POSIX_FADV_SEQUENTIAL);
      }
#endif

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

      auto context = std::make_shared<FileContext>(fd, path, block_count,
                                                   odirect, file_size);
      MaybePrefetch(context, 0);
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

        if (!WaitForPendingCapacity(soft_limit)) {
          stop_loop = true;
          break;
        }

        MaybePrefetch(context, offset);

        AlignedBufferPool::Block pool_block{};
        bool pool_acquired = false;
        unsigned char *read_ptr = nullptr;
        std::size_t buffer_capacity = 0;
        std::chrono::nanoseconds acquire_wait_ns{0};

        if (buffer_pool_) {
          auto acquire_start = clock::now();
          pool_acquired = buffer_pool_->Acquire(pool_block, &stop_requested_);
          auto acquire_end = clock::now();
          acquire_wait_ns =
              std::chrono::duration_cast<std::chrono::nanoseconds>(acquire_end -
                                                                   acquire_start);
          if (pool_acquired &&
              acquire_wait_ns >= std::chrono::milliseconds(10)) {
            LogAcquireWait(acquire_wait_ns, request_index, file_id, 0);
          }
          if (pool_acquired) {
            read_ptr = pool_block.ptr;
            buffer_capacity = pool_block.size;
          }
        }

        if (!read_ptr) {
          if (aligned_buf) {
            read_ptr = static_cast<unsigned char *>(aligned_buf);
            buffer_capacity = buf_size;
          } else {
            if (fallback_buffer.empty()) {
              fallback_buffer.resize(buf_size ? buf_size : page_size);
            }
            read_ptr = fallback_buffer.data();
            buffer_capacity = fallback_buffer.size();
          }
        }

        if (!read_ptr) {
          LOG_ERROR("无可用缓冲区用于读取文件: %s", path.c_str());
          stop_loop = true;
          break;
        }

        pending_tasks_.fetch_add(1, std::memory_order_relaxed);

        auto iteration_start = clock::now();

        ssize_t bytes_read = ::pread(context->fd, read_ptr, read_len,
                                     static_cast<off_t>(offset));
        if (bytes_read < 0 && errno == EINVAL && context->use_odirect) {
          ::close(context->fd);
          int fd2 = ::open(context->path.c_str(), O_RDONLY);
          if (fd2 >= 0) {
            context->fd = fd2;
            context->use_odirect = false;
            bytes_read = ::pread(context->fd, read_ptr, read_len,
                                 static_cast<off_t>(offset));
          }
        }

        if (bytes_read < 0) {
          LOG_ERROR("读取文件失败: %s, offset=%llu, len=%zu, err=%s",
                    context->path.c_str(),
                    static_cast<unsigned long long>(offset), read_len,
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
          block.index = slot_index;
          block.file_id = file_id;
          block.file_offset = static_cast<std::int64_t>(offset);
          block.user_context =
              pool_acquired && buffer_pool_ ? buffer_pool_.get() : nullptr;

          if (!m_OrderedDataProcessor.ProcessData(block)) {
            LOG_ERROR("处理数据失败: file_id=%llu, offset=%llu",
                      static_cast<unsigned long long>(file_id),
                      static_cast<unsigned long long>(offset));
            release_pool = true;
          }
        }

        if (release_pool && pool_acquired && buffer_pool_) {
          buffer_pool_->Release(pool_block.ptr);
        }

        auto loop_end = clock::now();
        auto loop_ns =
            std::chrono::duration_cast<std::chrono::nanoseconds>(loop_end -
                                                                 iteration_start);
        bool used_external_memory =
            pool_acquired && buffer_pool_ != nullptr && !release_pool;
        if (loop_ns >= std::chrono::milliseconds(100)) {
          LogSlowLoop(loop_ns, request_index, file_id, used_external_memory, 0);
        }

        if (context->pending_blocks.fetch_sub(1, std::memory_order_relaxed) ==
            1) {
          // 最后一个block处理完毕，FileContext析构时会关闭文件
        }

        pending_tasks_.fetch_sub(1, std::memory_order_relaxed);
        capacity_cv_.notify_one();

        if (bytes_read <= 0) {
          break;
        }

        remaining -= static_cast<std::uint64_t>(bytes_read);
        offset += static_cast<std::uint64_t>(bytes_read);
      }

      if (stop_loop || stop_requested_.load(std::memory_order_relaxed)) {
        break;
      }
    }
  } while (init_param_.loop && !stop_requested_.load(std::memory_order_relaxed) &&
           !stop_loop);

  step_.store(1, std::memory_order_relaxed);

  while (pending_tasks_.load(std::memory_order_relaxed) > 0 &&
         !stop_requested_.load(std::memory_order_relaxed)) {
    step_.store(2, std::memory_order_relaxed);
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }

  stop_requested_.store(true, std::memory_order_relaxed);
  capacity_cv_.notify_all();
  task_cv_.notify_all();
  step_.store(3, std::memory_order_relaxed);

  cleanup_buffer();
}

void CInputDataFromFileUsePOSIX::WorkerThread(std::size_t worker_index) {
  using clock = std::chrono::steady_clock;
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

      auto iteration_start = clock::now();

      AlignedBufferPool::Block pool_block{};
      bool pool_acquired = false;
      unsigned char *read_ptr = nullptr;
      std::size_t buffer_capacity = 0;
      std::chrono::nanoseconds acquire_wait_ns{0};

      if (buffer_pool_) {
        auto acquire_start = clock::now();
        pool_acquired = buffer_pool_->Acquire(pool_block, &stop_requested_);
        auto acquire_end = clock::now();
        acquire_wait_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(acquire_end - acquire_start);
        if (pool_acquired && acquire_wait_ns >= std::chrono::milliseconds(10)) {
          LogAcquireWait(acquire_wait_ns, task.request_id, task.file_id, worker_index);
        }
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
        capacity_cv_.notify_one();
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

      auto loop_end = clock::now();
      auto loop_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(loop_end - iteration_start);
      bool used_external_memory = pool_acquired && buffer_pool_ != nullptr && !release_pool;
      if (loop_ns >= std::chrono::milliseconds(100)) {
        LogSlowLoop(loop_ns, task.request_id, task.file_id,
                    used_external_memory, worker_index);
      }

      if (task.context->pending_blocks.fetch_sub(1,
                                                 std::memory_order_relaxed) ==
          1) {
        // 最后一个block处理完毕，FileContext析构时会关闭文件
      }

      pending_tasks_.fetch_sub(1, std::memory_order_relaxed);
      capacity_cv_.notify_one();
    }
  } catch (const std::exception &ex) {
    LOG_ERROR("POSIX读取工作线程异常: %s", ex.what());
  }

  if (aligned_buf) {
    free(aligned_buf);
  }
}

void CInputDataFromFileUsePOSIX::ResetDiagnostics() {
  std::lock_guard<std::mutex> lock(diagnostics_mutex_);
  diagnostics_ = WorkerDiagnostics{};
  last_metrics_log_ = std::chrono::steady_clock::now();
}

void CInputDataFromFileUsePOSIX::LogAcquireWait(std::chrono::nanoseconds wait_ns,
                                                std::uint64_t request_id,
                                                std::uint64_t file_id,
                                                std::size_t worker_index) {
  auto wait_ms = std::chrono::duration_cast<std::chrono::milliseconds>(wait_ns);

  std::uint64_t events = 0;
  std::uint64_t total_ns = 0;
  std::uint64_t max_ns = 0;
  {
    std::lock_guard<std::mutex> lock(diagnostics_mutex_);
    diagnostics_.acquire_wait_events += 1;
    diagnostics_.acquire_wait_ns += static_cast<std::uint64_t>(wait_ns.count());
    diagnostics_.acquire_wait_ns_max =
        std::max(diagnostics_.acquire_wait_ns_max,
                 static_cast<std::uint64_t>(wait_ns.count()));

    auto now = std::chrono::steady_clock::now();
    bool should_log = wait_ms >= std::chrono::milliseconds(200) ||
                      now - last_metrics_log_ >= std::chrono::seconds(1);
    if (!should_log)
      return;

    events = diagnostics_.acquire_wait_events;
    total_ns = diagnostics_.acquire_wait_ns;
    max_ns = diagnostics_.acquire_wait_ns_max;
    last_metrics_log_ = now;
  }

  double avg_ms = events > 0 ? static_cast<double>(total_ns) / 1'000'000.0 /
                                   static_cast<double>(events)
                              : 0.0;
  double max_ms = max_ns / 1'000'000.0;

  LOG_WARN(
    "POSIX 输入线程等待缓冲 %.2f ms (worker=%zu, 请求#%llu, 文件#%llu, 总等待 %llu 次, 均值 %.2f ms, 最大 %.2f ms)",
    static_cast<double>(wait_ms.count()), worker_index,
      static_cast<unsigned long long>(request_id),
      static_cast<unsigned long long>(file_id),
      static_cast<unsigned long long>(diagnostics_.acquire_wait_events), avg_ms,
      max_ms);
}

void CInputDataFromFileUsePOSIX::LogSlowLoop(std::chrono::nanoseconds loop_ns,
                                             std::uint64_t request_id,
                                             std::uint64_t file_id,
                                             bool used_external_memory,
                                             std::size_t worker_index) {
  auto loop_ms = std::chrono::duration_cast<std::chrono::milliseconds>(loop_ns);

  std::uint64_t events = 0;
  std::uint64_t total_ns = 0;
  std::uint64_t max_ns = 0;
  {
    std::lock_guard<std::mutex> lock(diagnostics_mutex_);
    diagnostics_.slow_loop_events += 1;
    diagnostics_.slow_loop_ns += static_cast<std::uint64_t>(loop_ns.count());
    diagnostics_.slow_loop_ns_max =
        std::max(diagnostics_.slow_loop_ns_max,
                 static_cast<std::uint64_t>(loop_ns.count()));

    auto now = std::chrono::steady_clock::now();
    bool should_log = loop_ms >= std::chrono::milliseconds(300) ||
                      now - last_metrics_log_ >= std::chrono::seconds(1);
    if (!should_log)
      return;

    events = diagnostics_.slow_loop_events;
    total_ns = diagnostics_.slow_loop_ns;
    max_ns = diagnostics_.slow_loop_ns_max;
    last_metrics_log_ = now;
  }

  double avg_ms = events > 0 ? static_cast<double>(total_ns) / 1'000'000.0 /
                                   static_cast<double>(events)
                              : 0.0;
  double max_ms = max_ns / 1'000'000.0;

  LOG_WARN(
    "POSIX 输入线程单轮耗时 %.2f ms (worker=%zu, 请求#%llu, 文件#%llu, 模式=%s, 总慢环 %llu 次, 均值 %.2f ms, 最大 %.2f ms)",
    static_cast<double>(loop_ms.count()), worker_index,
      static_cast<unsigned long long>(request_id),
      static_cast<unsigned long long>(file_id),
      used_external_memory ? "external" : "copy",
      static_cast<unsigned long long>(diagnostics_.slow_loop_events), avg_ms,
      max_ms);
}

void CInputDataFromFileUsePOSIX::MaybePrefetch(
    const std::shared_ptr<FileContext> &context,
    std::uint64_t upto_offset) {
#if defined(POSIX_FADV_WILLNEED)
  if (!context || context->use_odirect || init_param_.block_size == 0) {
    return;
  }

  const std::uint64_t limit =
      static_cast<std::uint64_t>(std::numeric_limits<off_t>::max());
  std::uint64_t window_blocks =
      std::max<std::uint64_t>(1, static_cast<std::uint64_t>(init_param_.io_request_num));
  std::uint64_t window_bytes =
      static_cast<std::uint64_t>(init_param_.block_size) * window_blocks;
  if (window_bytes == 0) {
    return;
  }

  std::uint64_t max_window = std::min<std::uint64_t>(limit, 512ull * 1024ull * 1024ull);
  if (window_bytes > max_window) {
    window_bytes = max_window;
  }

  std::uint64_t target = upto_offset + window_bytes;
  if (context->file_size > 0) {
    target = std::min<std::uint64_t>(target, context->file_size);
  }

  if (target <= context->prefetched_until) {
    return;
  }

  std::uint64_t advise_offset = context->prefetched_until;
  if (advise_offset >= target) {
    return;
  }

  std::uint64_t advise_len = target - advise_offset;
  advise_len = std::min<std::uint64_t>(advise_len, max_window);

  off_t advise_off = static_cast<off_t>(std::min<std::uint64_t>(advise_offset, limit));
  off_t advise_length = static_cast<off_t>(std::min<std::uint64_t>(advise_len, limit));

  int err = ::posix_fadvise(context->fd, advise_off, advise_length,
                            POSIX_FADV_WILLNEED);
  if (err == 0) {
    context->prefetched_until = advise_offset + advise_len;
  } else if (!context->prefetch_failed) {
    context->prefetch_failed = true;
    LOG_WARN("posix_fadvise(WILLNEED) 失败: %s, offset=%llu, len=%llu, err=%s",
             context->path.c_str(),
             static_cast<unsigned long long>(advise_offset),
             static_cast<unsigned long long>(advise_len),
             std::strerror(err));
  }
#else
  (void)context;
  (void)upto_offset;
#endif
}

bool CInputDataFromFileUsePOSIX::WaitForPendingCapacity(std::size_t soft_limit) {
  if (soft_limit == 0) {
    return true;
  }

  while (!stop_requested_.load(std::memory_order_relaxed)) {
    if (pending_tasks_.load(std::memory_order_relaxed) < soft_limit) {
      return true;
    }

    std::unique_lock<std::mutex> lock(capacity_mutex_);
    capacity_cv_.wait(lock, [&] {
      return stop_requested_.load(std::memory_order_relaxed) ||
             pending_tasks_.load(std::memory_order_relaxed) < soft_limit;
    });

    if (pending_tasks_.load(std::memory_order_relaxed) < soft_limit) {
      return true;
    }
  }

  return false;
}

void CInputDataFromFileUsePOSIX::CloseAllFiles() {
  std::lock_guard<std::mutex> lock(task_mutex_);
  task_queue_.clear();
}
