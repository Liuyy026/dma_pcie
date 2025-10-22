#include "InputDataFromSHM.h"
#include "../utils/ShmRing.h"
#include "../utils/Logger.h"
#include "../utils/ThreadAffinity.h"
#include <algorithm>
#include <chrono>
#include <cstring>
#include <stdexcept>

CInputDataFromSHM::CInputDataFromSHM()
    : running_(false), stop_requested_(false) {
  ResetDiagnostics();
}
CInputDataFromSHM::~CInputDataFromSHM() { Destroy(); }

bool CInputDataFromSHM::Init(void *pInitParam) {
  if (!pInitParam) {
    LOG_ERROR("SHM 输入初始化参数为空");
    return false;
  }
  init_param_ = *static_cast<InputDataFromSHMInitParam *>(pInitParam);

  // 初始化有序数据处理器
  if (!m_OrderedDataProcessor.Initialize(32, init_param_.slot_size, init_param_.cpu_id)) {
    LOG_ERROR("有序数据处理器初始化失败 (SHM)");
    return false;
  }

  shm_.reset(new ShmRing());
  if (!shm_->Open(init_param_.shm_name, init_param_.slot_count, init_param_.slot_size, init_param_.create_if_missing)) {
    LOG_ERROR("打开共享内存环失败: %s", init_param_.shm_name.c_str());
    return false;
  }

  LOG_INFO("SHM 输入初始化成功: name=%s, slots=%u, slot_size=%u",
           init_param_.shm_name.c_str(), init_param_.slot_count, init_param_.slot_size);
  return true;
}

void CInputDataFromSHM::Destroy() {
  Stop();
  if (shm_) {
    shm_->Close();
    shm_.reset();
  }
  m_OrderedDataProcessor.Destroy();
}

bool CInputDataFromSHM::Start() {
  if (IsRunning()) return true;
  stop_requested_.store(false, std::memory_order_relaxed);
  running_.store(true, std::memory_order_release);
  ResetDiagnostics();

  try {
    consumer_thread_.reset(new std::thread(&CInputDataFromSHM::ConsumerThreadFunc, this));
    if (init_param_.cpu_id >= 0) {
      ThreadAffinity::GetInstance().SetThreadAffinity(consumer_thread_->native_handle(), init_param_.cpu_id);
    }
  } catch (const std::exception &e) {
    LOG_ERROR("启动 SHM 消费线程失败: %s", e.what());
    running_.store(false, std::memory_order_release);
    return false;
  }

  LOG_INFO("SHM 输入 Start 成功");
  return true;
}

bool CInputDataFromSHM::Stop() {
  stop_requested_.store(true, std::memory_order_release);
  if (consumer_thread_ && consumer_thread_->joinable()) {
    consumer_thread_->join();
    consumer_thread_.reset();
  }
  running_.store(false, std::memory_order_release);
  m_OrderedDataProcessor.StopProcessing();
  return true;
}

void CInputDataFromSHM::ConsumerThreadFunc() {
  using clock = std::chrono::steady_clock;
  uint8_t *data_ptr = nullptr;
  uint32_t data_len = 0;
  uint64_t index = 0;
  bool waiting_for_data = false;
  clock::time_point wait_start{};

  while (!stop_requested_.load(std::memory_order_relaxed)) {
    auto loop_start = clock::now();
    if (shm_->ConsumerPeek(data_ptr, data_len, index)) {
      if (waiting_for_data) {
        auto wait_ns =
            std::chrono::duration_cast<std::chrono::nanoseconds>(loop_start -
                                                                 wait_start);
        if (wait_ns >= std::chrono::milliseconds(10)) {
          LogAcquireWait(wait_ns, index);
        }
        waiting_for_data = false;
      }

      // 构造 InputBlock 并提交到有序处理器
      InputBlock block;
      block.index = index;
      block.file_id = 0;
      block.buffer = reinterpret_cast<std::uint8_t *>(data_ptr);
      block.buffer_capacity = static_cast<std::size_t>(data_len);
      block.data_length = static_cast<std::size_t>(data_len);
      block.user_context = nullptr;

      if (!m_OrderedDataProcessor.ProcessData(block)) {
        LOG_ERROR("SHM 数据提交到有序处理器失败");
        auto loop_end = clock::now();
        auto loop_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(loop_end - loop_start);
        if (loop_ns >= std::chrono::milliseconds(100)) {
          LogSlowLoop(loop_ns, index);
        }
        // 性能优化：减少休眠时间从1ms降到100us，提高响应速度
        std::this_thread::sleep_for(std::chrono::microseconds(100));
        continue;
      }

      // 等待有序处理器消费后，advance
      // 在当前简单设计中，我们假设有序处理器通过 listener 那端释放或处理后会调用回调释放 buffer。
      // 这里直接 advance，Producer/Consumer 需要遵守协议（producer 提交后，consumer 读取并 advance）
      if (!shm_->ConsumerAdvance(index)) {
        LOG_WARN("SHM ConsumerAdvance 失败: index=%llu", (unsigned long long)index);
      }

      auto loop_end = clock::now();
      auto loop_ns =
          std::chrono::duration_cast<std::chrono::nanoseconds>(loop_end -
                                                               loop_start);
      if (loop_ns >= std::chrono::milliseconds(100)) {
        LogSlowLoop(loop_ns, index);
      }
    } else {
      if (!waiting_for_data) {
        waiting_for_data = true;
        wait_start = loop_start;
      }
      // 空或未就绪，短暂等待
      // 性能优化：减少轮询延迟从1ms降到100us，提高响应速度和吞吐量
      std::this_thread::sleep_for(std::chrono::microseconds(100));
    }
  }
}

void CInputDataFromSHM::ResetDiagnostics() {
  std::lock_guard<std::mutex> lock(diagnostics_mutex_);
  diagnostics_ = ShmDiagnostics{};
  last_metrics_log_ = std::chrono::steady_clock::now();
}

void CInputDataFromSHM::LogAcquireWait(std::chrono::nanoseconds wait_ns,
                                       std::uint64_t slot_index) {
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
      "SHM 输入等待数据 %.2f ms (slot=%llu, 总等待 %llu 次, 均值 %.2f ms, 最大 %.2f ms)",
      static_cast<double>(wait_ms.count()),
      static_cast<unsigned long long>(slot_index),
      static_cast<unsigned long long>(events), avg_ms, max_ms);
}

void CInputDataFromSHM::LogSlowLoop(std::chrono::nanoseconds loop_ns,
                                    std::uint64_t slot_index) {
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
      "SHM 输入单轮耗时 %.2f ms (slot=%llu, 总慢环 %llu 次, 均值 %.2f ms, 最大 %.2f ms)",
      static_cast<double>(loop_ms.count()),
      static_cast<unsigned long long>(slot_index),
      static_cast<unsigned long long>(events), avg_ms, max_ms);
}
