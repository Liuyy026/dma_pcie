#include "InputDataFromSHM.h"
#include "../utils/ShmRing.h"
#include "../utils/Logger.h"
#include "../utils/ThreadAffinity.h"
#include <chrono>
#include <cstring>
#include <stdexcept>

CInputDataFromSHM::CInputDataFromSHM() : running_(false), stop_requested_(false) {}
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
  uint8_t *data_ptr = nullptr;
  uint32_t data_len = 0;
  uint64_t index = 0;

  while (!stop_requested_.load(std::memory_order_relaxed)) {
    if (shm_->ConsumerPeek(data_ptr, data_len, index)) {
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
        // 尝试休眠，防止紧循环
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        continue;
      }

      // 等待有序处理器消费后，advance
      // 在当前简单设计中，我们假设有序处理器通过 listener 那端释放或处理后会调用回调释放 buffer。
      // 这里直接 advance，Producer/Consumer 需要遵守协议（producer 提交后，consumer 读取并 advance）
      if (!shm_->ConsumerAdvance(index)) {
        LOG_WARN("SHM ConsumerAdvance 失败: index=%llu", (unsigned long long)index);
      }
    } else {
      // 空或未就绪，短暂等待
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  }
}
