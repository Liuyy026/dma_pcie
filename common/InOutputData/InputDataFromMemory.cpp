#include "InputDataFromMemory.h"
#include "../utils/Logger.h"
#include "../utils/ThreadAffinity.h"
#include <chrono>
#include <cstring>
#include <stdexcept>

CInputDataFromMemory::CInputDataFromMemory()
    : stop_requested_(false), running_(false), buffer_pool_(),
      pattern_buffer_(), pool_block_count_(0), pool_prefilled_(false),
      produced_bytes_(0), dispatched_bytes_(0) {
  ResetDiagnostics();
  last_metrics_log_ = std::chrono::steady_clock::now();
}

CInputDataFromMemory::~CInputDataFromMemory() { Destroy(); }

bool CInputDataFromMemory::Init(void *pInitParam) {
  if (!pInitParam) {
    LOG_ERROR("内存输入初始化参数为空");
    return false;
  }

  init_param_ = *static_cast<InputDataFromMemoryInitParam *>(pInitParam);
  pool_block_count_ = 0;
  pool_prefilled_ = false;

  // 尝试创建对齐缓冲池以启用外部内存模式，若失败则降级为内部拷贝
  buffer_pool_.reset();
  bool enable_external = false;
  try {
    auto pool = std::make_shared<AlignedBufferPool>();
    std::size_t alignment = static_cast<std::size_t>(sysconf(_SC_PAGESIZE));
    std::size_t pool_block_count =
        std::max<std::size_t>(1, static_cast<std::size_t>(MAX_DISORDER_GROUPS) *
                                        init_param_.io_request_num);
    if (pool->Initialize(pool_block_count, init_param_.block_size, alignment)) {
      buffer_pool_ = std::move(pool);
      pool_block_count_ = pool_block_count;
      enable_external = true;
    } else {
      LOG_WARN("对齐缓冲池初始化失败，回退到拷贝模式");
      buffer_pool_.reset();
      enable_external = false;
    }
  } catch (const std::exception &e) {
    LOG_WARN("创建缓冲池异常，回退到拷贝模式: %s", e.what());
    buffer_pool_.reset();
    enable_external = false;
  }

  // 参数校验与容错：防止过大或为0的值导致后续分配失败
  if (init_param_.io_request_num == 0) {
    LOG_WARN("io_request_num 为0，强制置为1");
    init_param_.io_request_num = 1;
  }

  // 限制 block_size 在一个合理范围（512B 到 16MB）以避免溢出或不合理分配
  const unsigned int MIN_BLOCK = 512;
  const unsigned int MAX_BLOCK = 16 * 1024 * 1024;
  if (init_param_.block_size < MIN_BLOCK) {
    LOG_WARN("block_size 太小，强制置为 %u", MIN_BLOCK);
    init_param_.block_size = MIN_BLOCK;
  } else if (init_param_.block_size > MAX_BLOCK) {
    LOG_WARN("block_size 太大，强制置为 %u", MAX_BLOCK);
    init_param_.block_size = MAX_BLOCK;
  }

  BuildPatternBuffer(init_param_.block_size);

  if (enable_external && buffer_pool_) {
    pool_prefilled_ = PrefillBufferPool();
    if (!pool_prefilled_) {
      LOG_WARN("缓冲池预填充失败，将在运行时逐块填充数据");
    } else {
      LOG_INFO("缓冲池预填充完成，共初始化 %zu 个块", pool_block_count_);
    }
  }

  // 设置外部内存模式（在 Initialize 之前），以便有序处理器按外部/内部模式分配
  m_OrderedDataProcessor.EnableExternalMemoryMode(enable_external);

  // 初始化有序数据处理器（传入 cpu_id 原值，不再 +1）
  LOG_INFO("初始化有序数据处理器: io_request_num=%u, block_size=%u, cpu_id=%d, external=%d",
           init_param_.io_request_num, init_param_.block_size,
           init_param_.cpu_id, enable_external ? 1 : 0);

  if (!m_OrderedDataProcessor.Initialize(init_param_.io_request_num,
                                         init_param_.block_size,
                                         init_param_.cpu_id)) {
    LOG_ERROR("有序数据处理器初始化失败");
    return false;
  }

  return true;
}

void CInputDataFromMemory::Destroy() {
  Stop();
  Reset();
  m_OrderedDataProcessor.Destroy();
  if (buffer_pool_) {
    buffer_pool_->Shutdown();
    buffer_pool_.reset();
  }
  pattern_buffer_.clear();
  pool_block_count_ = 0;
  pool_prefilled_ = false;
}

void CInputDataFromMemory::Reset() {
  stop_requested_.store(false, std::memory_order_relaxed);
  running_.store(false, std::memory_order_relaxed);
  produced_bytes_.store(0, std::memory_order_relaxed);
  dispatched_bytes_.store(0, std::memory_order_relaxed);
  ResetDiagnostics();
  last_metrics_log_ = std::chrono::steady_clock::now();
  m_OrderedDataProcessor.Reset();
  BuildPatternBuffer(init_param_.block_size);
  if (buffer_pool_) {
    buffer_pool_->Reset();
    pool_prefilled_ = PrefillBufferPool();
    if (!pool_prefilled_) {
      LOG_WARN("Reset 后缓冲池预填充失败，将继续使用逐块复制模式");
    }
  } else {
    pool_prefilled_ = false;
  }
}

bool CInputDataFromMemory::Start() {
  if (IsRunning()) return true;

  produced_bytes_.store(0, std::memory_order_relaxed);
  dispatched_bytes_.store(0, std::memory_order_relaxed);
  stop_requested_.store(false, std::memory_order_relaxed);

  if (!m_OrderedDataProcessor.StartProcessing()) {
    LOG_ERROR("有序数据处理器启动失败");
    return false;
  }

  try {
    produce_thread_ = std::make_unique<std::thread>(
        &CInputDataFromMemory::ProduceThreadFunc, this);
    if (init_param_.cpu_id >= 0) {
      ThreadAffinity::GetInstance().SetThreadAffinity(
          produce_thread_->native_handle(), init_param_.cpu_id);
    }
  } catch (const std::exception &e) {
    LOG_ERROR("启动生成线程失败: %s", e.what());
    Stop();
    return false;
  }

  running_.store(true, std::memory_order_release);
  LOG_INFO("内存输入 Start 成功: block_size=%u, io_request_num=%u, cpu_id=%d, external=%d",
           init_param_.block_size, init_param_.io_request_num,
           init_param_.cpu_id, m_OrderedDataProcessor.IsExternalMemoryMode() ? 1 : 0);
  return true;
}

bool CInputDataFromMemory::Stop() {
  stop_requested_.store(true, std::memory_order_release);
  if (produce_thread_ && produce_thread_->joinable()) {
    produce_thread_->join();
    produce_thread_.reset();
  }
  m_OrderedDataProcessor.StopProcessing();
  running_.store(false, std::memory_order_release);
  return true;
}

void CInputDataFromMemory::ProduceThreadFunc() {
  using clock = std::chrono::steady_clock;

  const unsigned int block_size = init_param_.block_size;
  if (pattern_buffer_.size() < block_size) {
    BuildPatternBuffer(block_size);
  }

  std::uint64_t remaining = init_param_.total_bytes;
  bool infinite = (init_param_.total_bytes == 0 && init_param_.loop);

  // 速率控制
  std::uint64_t rate_bps = init_param_.produce_rate_bps;
  double sleep_per_block_ms = 0.0;
  if (rate_bps > 0) {
    sleep_per_block_ms = (static_cast<double>(block_size) * 1000.0) / static_cast<double>(rate_bps);
  }

  // 生产循环
  std::uint64_t global_request_counter = 0;

  while (!stop_requested_.load(std::memory_order_relaxed)) {
    auto loop_start = clock::now();
    if (!infinite && remaining == 0) break;

    unsigned int this_len = block_size;
    if (!infinite && remaining > 0 && remaining < block_size) {
      this_len = static_cast<unsigned int>(remaining);
    }

    InputBlock block;
    block.index = global_request_counter++;
    block.file_id = 0;

    if (m_OrderedDataProcessor.IsExternalMemoryMode()) {
      // 使用缓冲池
      AlignedBufferPool::Block b{};
      auto acquire_start = clock::now();
      if (buffer_pool_ && buffer_pool_->Acquire(b, &stop_requested_)) {
        auto acquire_end = clock::now();
        auto wait_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(acquire_end - acquire_start);
        if (wait_ns >= std::chrono::milliseconds(10)) {
          LogAcquireWait(wait_ns, global_request_counter);
        }
        if (ShouldCopyIntoBlock(this_len)) {
          std::memcpy(b.ptr, pattern_buffer_.data(), this_len);
        }
        block.buffer = b.ptr;
        block.buffer_capacity = b.size;
        block.data_length = this_len;
        block.user_context = nullptr;

        if (!m_OrderedDataProcessor.ProcessData(block)) {
          // 如果提交失败，释放并重试或退出
          buffer_pool_->Release(b.ptr);
          LOG_ERROR("提交生成数据到有序处理器失败");
          break;
        }
        // 由有序处理器通过 listener 回调后，消费者应释放缓冲（如果需要）
      } else {
        LOG_WARN("无法从缓冲池获取块，回退到临时拷贝模式");
        // 回退到拷贝模式
        m_OrderedDataProcessor.EnableExternalMemoryMode(false);
        pool_prefilled_ = false;
        // 继续到下列拷贝路径
      }
    }

    if (!m_OrderedDataProcessor.IsExternalMemoryMode()) {
      // 拷贝到临时 buffer 并提交
      block.buffer = pattern_buffer_.data();
      block.buffer_capacity = static_cast<unsigned int>(pattern_buffer_.size());
      block.data_length = this_len;
      block.user_context = nullptr;

      if (!m_OrderedDataProcessor.ProcessData(block)) {
        LOG_ERROR("提交生成数据到有序处理器失败（拷贝模式）");
        break;
      }
    }

    produced_bytes_.fetch_add(this_len, std::memory_order_relaxed);
    dispatched_bytes_.fetch_add(this_len, std::memory_order_relaxed);

    if (!infinite) {
      if (remaining >= this_len) remaining -= this_len;
      else remaining = 0;
    }

    if (stop_requested_.load(std::memory_order_relaxed)) break;

    if (rate_bps > 0 && sleep_per_block_ms > 0.0) {
      // 精简的速率限制：睡眠指定毫秒
      std::this_thread::sleep_for(
          std::chrono::milliseconds(static_cast<int>(sleep_per_block_ms + 0.5)));
    } else {
      // 给出短暂的让步以避免忙轮询
      std::this_thread::sleep_for(std::chrono::milliseconds(0));
    }

    auto loop_end = clock::now();
    auto loop_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(loop_end - loop_start);
    if (loop_ns >= std::chrono::milliseconds(100)) {
      LogSlowLoop(loop_ns, global_request_counter,
                  m_OrderedDataProcessor.IsExternalMemoryMode());
    }
  }
}

void CInputDataFromMemory::ResetDiagnostics() {
  diagnostics_ = ProducerDiagnostics{};
}

void CInputDataFromMemory::LogAcquireWait(std::chrono::nanoseconds wait_ns,
                                          std::uint64_t request_index) {
  diagnostics_.acquire_wait_events += 1;
  diagnostics_.acquire_wait_ns += static_cast<std::uint64_t>(wait_ns.count());
  diagnostics_.acquire_wait_ns_max =
      std::max(diagnostics_.acquire_wait_ns_max,
               static_cast<std::uint64_t>(wait_ns.count()));

  auto wait_ms = std::chrono::duration_cast<std::chrono::milliseconds>(wait_ns);
  auto now = std::chrono::steady_clock::now();
  bool should_log = now - last_metrics_log_ >= std::chrono::seconds(1) ||
                    wait_ms >= std::chrono::milliseconds(200);
  if (should_log) {
    last_metrics_log_ = now;
    double avg_ms = diagnostics_.acquire_wait_events > 0
                        ? static_cast<double>(diagnostics_.acquire_wait_ns) /
                              1'000'000.0 /
                              static_cast<double>(diagnostics_.acquire_wait_events)
                        : 0.0;
    double max_ms = diagnostics_.acquire_wait_ns_max / 1'000'000.0;
    LOG_WARN(
        "内存生产线程等待缓冲 %.2f ms (请求 #%llu, 总等待 %llu 次, 均值 %.2f ms, 最大 %.2f ms)",
        wait_ms.count(), static_cast<unsigned long long>(request_index),
        static_cast<unsigned long long>(diagnostics_.acquire_wait_events),
        avg_ms, max_ms);
  }
}

void CInputDataFromMemory::LogSlowLoop(std::chrono::nanoseconds loop_ns,
                                       std::uint64_t request_index,
                                       bool used_external_memory) {
  diagnostics_.slow_loop_events += 1;
  diagnostics_.slow_loop_ns += static_cast<std::uint64_t>(loop_ns.count());
  diagnostics_.slow_loop_ns_max =
      std::max(diagnostics_.slow_loop_ns_max,
               static_cast<std::uint64_t>(loop_ns.count()));

  auto loop_ms = std::chrono::duration_cast<std::chrono::milliseconds>(loop_ns);
  auto now = std::chrono::steady_clock::now();
  bool should_log = now - last_metrics_log_ >= std::chrono::seconds(1) ||
                    loop_ms >= std::chrono::milliseconds(300);
  if (should_log) {
    last_metrics_log_ = now;
    double avg_ms = diagnostics_.slow_loop_events > 0
                        ? static_cast<double>(diagnostics_.slow_loop_ns) /
                              1'000'000.0 /
                              static_cast<double>(diagnostics_.slow_loop_events)
                        : 0.0;
    double max_ms = diagnostics_.slow_loop_ns_max / 1'000'000.0;
    LOG_WARN(
        "内存生产线程单轮耗时 %.2f ms (请求 #%llu, 模式=%s, 总慢环 %llu 次, 均值 %.2f ms, 最大 %.2f ms)",
        loop_ms.count(), static_cast<unsigned long long>(request_index),
        used_external_memory ? "external" : "copy",
        static_cast<unsigned long long>(diagnostics_.slow_loop_events), avg_ms,
        max_ms);
  }
}

void CInputDataFromMemory::BuildPatternBuffer(unsigned int block_size) {
  pattern_buffer_.resize(block_size);
  for (unsigned int i = 0; i < block_size; ++i) {
    pattern_buffer_[i] = static_cast<unsigned char>(i & 0xFF);
  }
}

bool CInputDataFromMemory::PrefillBufferPool() {
  if (!buffer_pool_ || pool_block_count_ == 0 || pattern_buffer_.empty()) {
    return false;
  }

  std::vector<AlignedBufferPool::Block> acquired;
  acquired.reserve(pool_block_count_);

  for (std::size_t i = 0; i < pool_block_count_; ++i) {
    AlignedBufferPool::Block block{};
    if (!buffer_pool_->Acquire(block, nullptr)) {
      LOG_WARN("缓冲池预填充失败: Acquire 第 %zu 个块时失败 (总计 %zu)", i, pool_block_count_);
      for (auto &owned : acquired) {
        buffer_pool_->Release(owned.ptr);
      }
      return false;
    }
    std::memcpy(block.ptr, pattern_buffer_.data(), init_param_.block_size);
    acquired.push_back(block);
  }

  for (auto &owned : acquired) {
    buffer_pool_->Release(owned.ptr);
  }

  return true;
}

bool CInputDataFromMemory::ShouldCopyIntoBlock(unsigned int data_length) const {
  if (!buffer_pool_) {
    return true;
  }
  if (!pool_prefilled_) {
    return true;
  }
  return data_length != init_param_.block_size;
}
