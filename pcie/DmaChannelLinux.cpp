#define DESCRIPTOR_QUEUE_MODE
#include "DmaChannelLinux.h"
#include "utils/ThreadAffinity.h"
#include <chrono>
#include <cstring>
#include <errno.h>
#include <unistd.h>
#include <fstream>
#include <mutex>
#include <cstdlib>
#include <sstream>
#include <iomanip>
#include <sys/syscall.h>
#include <sys/types.h>
#include <thread>
#include <cstdio>
#include <string>

namespace {
const bool g_pcie_dma_verbose_log = []() -> bool {
  const char *env = std::getenv("PCIE_DMA_VERBOSE_LOG");
  if (!env) {
    return false;
  }
  std::string value(env);
  if (value == "0" || value == "false" || value == "False" ||
      value == "off" || value == "OFF") {
    return false;
  }
  return true;
}();
}

DmaChannelLinux::DmaChannelLinux(int device_fd, unsigned int cpu_id)
    : device_fd_(device_fd), cpu_id_(cpu_id), dma_size_(0), queue_size_(0),
      desc_queue_capacity_(0), stop_flag_(false),
    thread_state_(ThreadState::NotStarted), step_(0), total_sent_(0),
    monitor_stop_flag_(false) {
  status_.threadState = ThreadState::NotStarted;
}

DmaChannelLinux::~DmaChannelLinux() { Stop(); }

bool DmaChannelLinux::Initialize(unsigned int dmaSize,
                                 unsigned int queueSize) {
  if (thread_state_.load(std::memory_order_relaxed) == ThreadState::Running) {
    LOG_ERROR("DMA通道正在运行，无法初始化");
    return false;
  }

  dma_size_ = dmaSize;
  queue_size_ = queueSize;

  if (!send_queue_.Init(queue_size_)) {
    LOG_ERROR("初始化发送队列失败");
    return false;
  }

#ifdef DESCRIPTOR_QUEUE_MODE
  if (!desc_queue_.Init(queue_size_)) {
    LOG_ERROR("初始化描述符队列失败");
    return false;
  }
  desc_queue_capacity_ = desc_queue_.Capacity();
#else
  desc_queue_capacity_ = 0;
#endif

  total_sent_.store(0, std::memory_order_relaxed);
  first_send_ns_.store(0, std::memory_order_relaxed);
  step_.store(0, std::memory_order_relaxed);

  status_.queueSize = queue_size_;
  status_.queueUsed = 0;
  status_.queueUsagePercent = 0.0f;
  status_.threadState = ThreadState::NotStarted;
  status_.totalBytesSent = 0;
  status_.currentSpeed = 0;
  status_.elapsedNs = 0;
  status_.step = 0;

  return true;
}

bool DmaChannelLinux::Start() {
  if (thread_state_.load(std::memory_order_relaxed) == ThreadState::Running) {
    LOG_ERROR("DMA通道已经在运行");
    return false;
  }

  stop_flag_.store(false, std::memory_order_relaxed);

  try {
    worker_thread_ = std::thread(&DmaChannelLinux::SendThreadFunc, this);
    if (!ThreadAffinity::GetInstance().SetThreadAffinity(
            worker_thread_.native_handle(),
            static_cast<int>(cpu_id_))) {
      LOG_WARN("设置DMA发送线程亲和性失败");
    }

    monitor_stop_flag_.store(false, std::memory_order_relaxed);
    monitor_thread_ = std::thread([this]() {
      const auto interval = std::chrono::seconds(1);
      std::uint64_t prev_total = total_sent_.load(std::memory_order_relaxed);
      auto prev_time = std::chrono::steady_clock::now();
      while (!monitor_stop_flag_.load(std::memory_order_relaxed)) {
        std::this_thread::sleep_for(interval);
        auto now_time = std::chrono::steady_clock::now();
        auto elapsed_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                              now_time - prev_time)
                              .count();
        prev_time = now_time;

        UpdateStatus();
        // Periodic diagnostic logging: counts and average latencies
        std::uint64_t dcount = desc_write_count_.load(std::memory_order_relaxed);
        std::uint64_t dsum = desc_write_ns_total_.load(std::memory_order_relaxed);
        std::uint64_t ccount = copy_write_count_.load(std::memory_order_relaxed);
        std::uint64_t csum = copy_write_ns_total_.load(std::memory_order_relaxed);
        double davg_ms = dcount ? (double)dsum / (double)dcount / 1e6 : 0.0;
        double cavg_ms = ccount ? (double)csum / (double)ccount / 1e6 : 0.0;
        std::uint64_t total_bytes = total_sent_.load(std::memory_order_relaxed);
        std::uint64_t delta = (total_bytes >= prev_total) ? (total_bytes - prev_total) : 0;
        prev_total = total_bytes;
        std::uint64_t speed_bps = 0;
        if (elapsed_ns > 0) {
          speed_bps = static_cast<std::uint64_t>(
              (static_cast<long double>(delta) * 1e9L) /
              static_cast<long double>(elapsed_ns));
        }
        status_.currentSpeed = speed_bps;
        // Write a small JSON live snapshot atomically to /tmp/pcie_live.json
        try {
          std::ostringstream oss;
          auto now_ns = static_cast<std::uint64_t>(
              std::chrono::duration_cast<std::chrono::nanoseconds>(
                  std::chrono::steady_clock::now().time_since_epoch())
                  .count());
          oss << "{\n";
          oss << "  \"ts_ns\": " << now_ns << ",\n";
          oss << "  \"currentSpeed\": " << (unsigned long long)speed_bps << ",\n";
          oss << "  \"deltaBytes\": " << (unsigned long long)delta << ",\n";
          oss << "  \"intervalNs\": " << (unsigned long long)elapsed_ns << ",\n";
          oss << "  \"totalBytes\": " << (unsigned long long)total_bytes << ",\n";
          oss << "  \"queueUsed\": " << status_.queueUsed << ",\n";
          oss << "  \"queueSize\": " << status_.queueSize << ",\n";
          oss << "  \"queueUsagePercent\": " << status_.queueUsagePercent << ",\n";
          oss << "  \"descWriteCount\": " << (unsigned long long)dcount << ",\n";
          oss << "  \"descAvgMs\": " << davg_ms << ",\n";
          oss << "  \"copyWriteCount\": " << (unsigned long long)ccount << ",\n";
          oss << "  \"copyAvgMs\": " << cavg_ms << ",\n";
          oss << "  \"step\": " << status_.step << "\n";
          oss << "}\n";
          std::string tmpPath = "/tmp/pcie_live.json.tmp";
          std::string finalPath = "/tmp/pcie_live.json";
          std::ofstream ofs(tmpPath, std::ios::out | std::ios::trunc);
          if (ofs.is_open()) {
            ofs << oss.str();
            ofs.flush();
            ofs.close();
            std::rename(tmpPath.c_str(), finalPath.c_str());
          }
        } catch (...) {
          // ignore write errors
        }
        if (g_pcie_dma_verbose_log) {
          LOG_DEBUG("DMA status: speed=%llu B/s, deltaBytes=%llu, totalBytes=%llu, queueUsed=%zu/%zu (%.1f%%), descWrites=%llu, descAvg=%.3f ms, copyWrites=%llu, copyAvg=%.3f ms, step=%d",
                   (unsigned long long)status_.currentSpeed,
                   (unsigned long long)delta,
                   (unsigned long long)total_bytes,
                   status_.queueUsed, status_.queueSize, status_.queueUsagePercent,
                   (unsigned long long)dcount, davg_ms,
                   (unsigned long long)ccount, cavg_ms,
                   status_.step);
        }
      }
    });
  } catch (const std::exception &ex) {
    LOG_ERROR("启动DMA线程失败: %s", ex.what());
    Stop();
    return false;
  }

  thread_state_.store(ThreadState::Running, std::memory_order_release);
  status_.threadState = ThreadState::Running;
  return true;
}

// CSV logging helpers (controlled by environment variable PCIE_WRITE_CSV)
static std::ofstream g_pcie_csv;
static std::mutex g_pcie_csv_mutex;
static bool g_pcie_csv_enabled = []() -> bool {
  const char *e = std::getenv("PCIE_WRITE_CSV");
  if (!e) return false;
  std::string ev(e);
  if (ev == "0" || ev == "false" || ev == "False") return false;
  const char *path = std::getenv("PCIE_WRITE_CSV_PATH");
  std::string p = path ? path : "/tmp/pcie_write_log.csv";
  // try open file
  try {
    g_pcie_csv.open(p, std::ios::out | std::ios::app);
    if (g_pcie_csv.is_open()) {
      // write header if file was empty
      g_pcie_csv << "ts_ns,thread_id,write_type,req_len,written,latency_ns,errno\n";
      g_pcie_csv.flush();
      return true;
    }
  } catch (...) {
  }
  return false;
}();

bool DmaChannelLinux::Stop() {
  stop_flag_.store(true, std::memory_order_release);
  data_cv_.notify_all();

  if (worker_thread_.joinable()) {
    worker_thread_.join();
  }
  if (monitor_thread_.joinable()) {
    monitor_stop_flag_.store(true, std::memory_order_relaxed);
    monitor_thread_.join();
  }

  DrainDescriptorQueue();

  thread_state_.store(ThreadState::Stopped, std::memory_order_release);
  status_.threadState = ThreadState::Stopped;
  first_send_ns_.store(0, std::memory_order_relaxed);
  // Dump a small JSON summary to a well-known path so external tools
  // (or the user) can read condensed statistics without needing CSV.
  try {
    std::ofstream ofs("/tmp/pcie_summary.json", std::ios::out | std::ios::trunc);
    if (ofs.is_open()) {
      std::uint64_t dcount = desc_write_count_.load(std::memory_order_relaxed);
      std::uint64_t dsum = desc_write_ns_total_.load(std::memory_order_relaxed);
      std::uint64_t ccount = copy_write_count_.load(std::memory_order_relaxed);
      std::uint64_t csum = copy_write_ns_total_.load(std::memory_order_relaxed);
      std::uint64_t total = total_sent_.load(std::memory_order_relaxed);
      double davg_ms = dcount ? (double)dsum / (double)dcount / 1e6 : 0.0;
      double cavg_ms = ccount ? (double)csum / (double)ccount / 1e6 : 0.0;
      ofs << "{\n";
      ofs << "  \"descWriteCount\": " << dcount << ",\n";
      ofs << "  \"descWriteNsTotal\": " << dsum << ",\n";
      ofs << "  \"descAvgMs\": " << davg_ms << ",\n";
      ofs << "  \"copyWriteCount\": " << ccount << ",\n";
      ofs << "  \"copyWriteNsTotal\": " << csum << ",\n";
      ofs << "  \"copyAvgMs\": " << cavg_ms << ",\n";
      ofs << "  \"totalBytesSent\": " << total << "\n";
      ofs << "}\n";
      ofs.flush();
      ofs.close();
    }
  } catch (...) {
    // ignore errors writing summary
  }

  return true;
}

bool DmaChannelLinux::IsRunning() const {
  return thread_state_.load(std::memory_order_relaxed) == ThreadState::Running;
}

bool DmaChannelLinux::Reset() {
  send_queue_.Reset();
#ifdef DESCRIPTOR_QUEUE_MODE
  desc_queue_.Reset();
#endif
  total_sent_.store(0, std::memory_order_relaxed);
  first_send_ns_.store(0, std::memory_order_relaxed);
  status_.queueUsed = 0;
  status_.queueUsagePercent = 0.0f;
  status_.totalBytesSent = 0;
  status_.currentSpeed = 0;
  status_.elapsedNs = 0;
  status_.step = 0;
  return true;
}

bool DmaChannelLinux::Send(unsigned char *data, unsigned int length) {
  if (!IsRunning()) {
    LOG_ERROR("DMA通道未运行，无法发送数据");
    return false;
  }

  bool descriptor_enqueued = false;

#ifdef DESCRIPTOR_QUEUE_MODE
  bool release_after_copy = false;
  bool pointer_from_pool = buffer_pool_ && buffer_pool_->Owns(data);
  if (pointer_from_pool) {
    BufferDescriptor desc{data, length, buffer_pool_.get()};
    if (desc_queue_.PutDescriptor(desc)) {
      descriptor_enqueued = true;
    } else {
      release_after_copy = true;
      LOG_WARN("描述符队列已满，回退到拷贝路径: %u", length);
    }
  }
#endif

  if (!descriptor_enqueued) {
    if (!send_queue_.PutFixedLength(data, length)) {
      return false;
    }
  LOG_DEBUG("Send 路径: 已放入 send_queue_, len=%u", length);
#ifdef DESCRIPTOR_QUEUE_MODE
    if (release_after_copy && buffer_pool_) {
      buffer_pool_->Release(data);
    }
#endif
  }

  {
    std::lock_guard<std::mutex> lock(data_mutex_);
#ifdef DESCRIPTOR_QUEUE_MODE
    if (descriptor_enqueued) {
      status_.queueUsed = desc_queue_.GetDataSize();
      status_.queueSize = desc_queue_capacity_ ? desc_queue_capacity_ : 1;
      if (desc_queue_capacity_ > 0) {
        status_.queueUsagePercent =
            static_cast<float>(status_.queueUsed) / desc_queue_capacity_ *
            100.0f;
      } else {
        status_.queueUsagePercent = 0.0f;
      }
    } else {
      status_.queueUsed = send_queue_.GetDataSize();
      status_.queueSize = queue_size_;
      if (queue_size_ > 0) {
        status_.queueUsagePercent =
            static_cast<float>(status_.queueUsed) / queue_size_ * 100.0f;
      } else {
        status_.queueUsagePercent = 0.0f;
      }
    }
#else
    status_.queueUsed = send_queue_.GetDataSize();
    if (queue_size_ > 0) {
      status_.queueUsagePercent =
          static_cast<float>(status_.queueUsed) / queue_size_ * 100.0f;
    }
#endif
  }

  data_cv_.notify_one();
  return true;
}

void DmaChannelLinux::SetBufferPool(std::shared_ptr<AlignedBufferPool> pool) {
#ifdef DESCRIPTOR_QUEUE_MODE
  std::lock_guard<std::mutex> lock(data_mutex_);
  if (!pool) {
    DrainDescriptorQueue();
  }
  buffer_pool_ = std::move(pool);
#else
  (void)pool;
#endif
}

DmaChannelStatus DmaChannelLinux::GetStatus() const {
  return status_;
}

void DmaChannelLinux::SendThreadFunc() {
  std::vector<unsigned char> buffer(dma_size_);
  step_.store(0, std::memory_order_relaxed);

  try {
    while (!stop_flag_.load(std::memory_order_relaxed)) {
      step_.store(1, std::memory_order_relaxed);
      {
        std::unique_lock<std::mutex> lock(data_mutex_);
        data_cv_.wait(lock, [this] {
          return stop_flag_.load(std::memory_order_relaxed) ||
                 HasPendingData();
        });
      }

      if (stop_flag_.load(std::memory_order_relaxed)) {
        break;
      }

#ifdef DESCRIPTOR_QUEUE_MODE
      BufferDescriptor desc;
      if (!desc_queue_.GetDescriptor(desc)) {
        step_.store(2, std::memory_order_relaxed);
      } else {
        step_.store(3, std::memory_order_relaxed);
    // descriptor-path write: measure latency
  if (g_pcie_dma_verbose_log) {
    LOG_DEBUG("描述符路径: 准备写入 device_fd=%d, ptr=%p, len=%u", device_fd_, desc.ptr, desc.length);
  }
  auto t0 = std::chrono::steady_clock::now();
  ssize_t bytes_written = ::write(device_fd_, desc.ptr, static_cast<size_t>(desc.length));
  auto t1 = std::chrono::steady_clock::now();
  if (g_pcie_dma_verbose_log) {
    LOG_DEBUG("描述符路径: write 返回: %zd, errno=%d (%s)", bytes_written, errno, std::strerror(errno));
  }

        if (bytes_written < 0) {
          LOG_ERROR("写入XDMA设备失败: %s", std::strerror(errno));
          ReleaseDescriptor(desc);
          thread_state_.store(ThreadState::Error, std::memory_order_relaxed);
          status_.threadState = ThreadState::Error;
          break;
        }

        // accumulate descriptor-path timing
        auto delta_ns = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0)
                .count());
    // CSV logging (if enabled)
    if (g_pcie_csv_enabled && g_pcie_csv.is_open()) {
      std::uint64_t ts = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now().time_since_epoch())
          .count());
      std::ostringstream oss;
      oss << ts << "," << std::this_thread::get_id() << ",desc,"
        << desc.length << "," << (bytes_written < 0 ? 0 : bytes_written)
        << "," << delta_ns << "," << (bytes_written < 0 ? errno : 0)
        << "\n";
      std::lock_guard<std::mutex> lg(g_pcie_csv_mutex);
      g_pcie_csv << oss.str();
      g_pcie_csv.flush();
    }
        desc_write_count_.fetch_add(1, std::memory_order_relaxed);
        desc_write_ns_total_.fetch_add(delta_ns, std::memory_order_relaxed);

        ReleaseDescriptor(desc);
        total_sent_.fetch_add(static_cast<std::uint64_t>(bytes_written),
                              std::memory_order_relaxed);
        UpdateStatus();
        step_.store(4, std::memory_order_relaxed);
        continue;
      }
      step_.store(2, std::memory_order_relaxed);
#endif
      unsigned int data_size =
          send_queue_.Get(buffer.data(), static_cast<uint32_t>(buffer.size()));

      if (data_size == 0) {
        continue;
      }

      step_.store(3, std::memory_order_relaxed);
      // copy-path write: measure latency
    if (g_pcie_dma_verbose_log) {
      LOG_DEBUG("拷贝路径: 准备写入 device_fd=%d, buf=%p, len=%u", device_fd_, buffer.data(), data_size);
    }
    auto t0c = std::chrono::steady_clock::now();
    ssize_t bytes_written = ::write(device_fd_, buffer.data(), static_cast<size_t>(data_size));
    auto t1c = std::chrono::steady_clock::now();
    if (g_pcie_dma_verbose_log) {
      LOG_DEBUG("拷贝路径: write 返回: %zd, errno=%d (%s)", bytes_written, errno, std::strerror(errno));
    }

      if (bytes_written < 0) {
        LOG_ERROR("写入XDMA设备失败: %s", std::strerror(errno));
        thread_state_.store(ThreadState::Error, std::memory_order_relaxed);
        status_.threadState = ThreadState::Error;
        break;
      }

      auto delta_ns_c = static_cast<std::uint64_t>(
          std::chrono::duration_cast<std::chrono::nanoseconds>(t1c - t0c)
              .count());
      // CSV logging (if enabled)
      if (g_pcie_csv_enabled && g_pcie_csv.is_open()) {
      std::uint64_t ts = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now().time_since_epoch())
          .count());
      std::ostringstream oss;
      oss << ts << "," << std::this_thread::get_id() << ",copy,"
        << data_size << "," << (bytes_written < 0 ? 0 : bytes_written)
        << "," << delta_ns_c << "," << (bytes_written < 0 ? errno : 0)
        << "\n";
      std::lock_guard<std::mutex> lg(g_pcie_csv_mutex);
      g_pcie_csv << oss.str();
      g_pcie_csv.flush();
      }
      copy_write_count_.fetch_add(1, std::memory_order_relaxed);
      copy_write_ns_total_.fetch_add(delta_ns_c, std::memory_order_relaxed);

      total_sent_.fetch_add(static_cast<std::uint64_t>(bytes_written),
                            std::memory_order_relaxed);
      UpdateStatus();
      step_.store(4, std::memory_order_relaxed);
    }
    step_.store(5, std::memory_order_relaxed);
  } catch (const std::exception &ex) {
    step_.store(6, std::memory_order_relaxed);
    LOG_ERROR("DMA发送线程发生异常: %s", ex.what());
  }
}

void DmaChannelLinux::UpdateStatus() {
  status_.queueUsagePercent = 0.0f;

#ifdef DESCRIPTOR_QUEUE_MODE
  uint32_t desc_used = desc_queue_.GetDataSize();
  if (desc_used > 0) {
    status_.queueUsed = desc_used;
    status_.queueSize = desc_queue_capacity_ ? desc_queue_capacity_ : 1;
    if (desc_queue_capacity_ > 0) {
      status_.queueUsagePercent =
          static_cast<float>(desc_used) / desc_queue_capacity_ * 100.0f;
    }
  } else {
    status_.queueUsed = send_queue_.GetDataSize();
    status_.queueSize = queue_size_;
    if (queue_size_ > 0) {
      status_.queueUsagePercent =
          static_cast<float>(status_.queueUsed) / queue_size_ * 100.0f;
    }
  }
#else
  status_.queueUsed = send_queue_.GetDataSize();
  status_.queueSize = queue_size_;
  if (queue_size_ > 0) {
    status_.queueUsagePercent =
        static_cast<float>(status_.queueUsed) / queue_size_ * 100.0f;
  }
#endif

  auto total = total_sent_.load(std::memory_order_relaxed);
  status_.totalBytesSent = total;

  std::uint64_t now_ns = static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now().time_since_epoch())
          .count());

  if (total > 0) {
    std::uint64_t expected = 0;
    first_send_ns_.compare_exchange_strong(expected, now_ns,
                                           std::memory_order_relaxed,
                                           std::memory_order_relaxed);
  }
  std::uint64_t start_ns = first_send_ns_.load(std::memory_order_relaxed);
  if (start_ns > 0 && now_ns >= start_ns) {
    status_.elapsedNs = now_ns - start_ns;
  } else {
    status_.elapsedNs = 0;
  }

  status_.step = step_.load(std::memory_order_relaxed);

  // Publish diagnostic counters
  status_.descWriteCount = desc_write_count_.load(std::memory_order_relaxed);
  status_.descWriteNsTotal =
      desc_write_ns_total_.load(std::memory_order_relaxed);
  status_.copyWriteCount = copy_write_count_.load(std::memory_order_relaxed);
  status_.copyWriteNsTotal =
      copy_write_ns_total_.load(std::memory_order_relaxed);
}

bool DmaChannelLinux::HasPendingData() const {
#ifdef DESCRIPTOR_QUEUE_MODE
  if (desc_queue_.GetDataSize() > 0) {
    return true;
  }
#endif
  return !send_queue_.IsEmpty();
}

void DmaChannelLinux::DrainDescriptorQueue() {
#ifdef DESCRIPTOR_QUEUE_MODE
  BufferDescriptor desc;
  while (desc_queue_.GetDescriptor(desc)) {
    ReleaseDescriptor(desc);
  }
  desc_queue_.Reset();
#endif
}

void DmaChannelLinux::ReleaseDescriptor(const BufferDescriptor &desc) {
#ifdef DESCRIPTOR_QUEUE_MODE
  if (!desc.ptr) {
    return;
  }
  AlignedBufferPool *pool = static_cast<AlignedBufferPool *>(desc.context);
  if (pool) {
    pool->Release(desc.ptr);
  } else if (buffer_pool_ && buffer_pool_->Owns(desc.ptr)) {
    buffer_pool_->Release(desc.ptr);
  }
#else
  (void)desc;
#endif
}
