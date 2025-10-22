#include "OrderedDataProcessor.h"
#include "../utils/Logger.h"
#include "../utils/ThreadAffinity.h"
#include <chrono>
#include <cstring>
#include <cstdint>
#include <cerrno>

COrderedDataProcessor::COrderedDataProcessor()
    : m_pMemoryBuffer(nullptr), m_uiBufferSize(0), m_uiBlockSize(0),
      m_uiIORequestNum(0), m_uiTotalSlots(0), m_uiNextOutputSlot(0),
      m_bStopThread(false), m_bThreadRunning(false),
      m_pInputDataListener(nullptr), use_external_memory_(false) {}

COrderedDataProcessor::~COrderedDataProcessor() { Destroy(); }

bool COrderedDataProcessor::Initialize(unsigned int io_request_num,
                                       unsigned int block_size, int cpu_id) {
  Destroy();

  m_uiIORequestNum = io_request_num;
  m_uiBlockSize = block_size;
  m_uiTotalSlots = MAX_DISORDER_GROUPS * m_uiIORequestNum;
  m_cpuId = cpu_id;

  m_uiBufferSize =
      static_cast<std::uint64_t>(m_uiTotalSlots) * m_uiBlockSize;
  try {
    if (!use_external_memory_) {
      m_pMemoryBuffer = std::make_unique<VirtualMemoryBuffer>(m_uiBufferSize);
      if (!m_pMemoryBuffer->isValid()) {
        LOG_ERROR("虚拟内存分配失败，需要分配 {} 字节", m_uiBufferSize);
        return false;
      }
    } else {
      slot_blocks_.clear();
      slot_blocks_.resize(m_uiTotalSlots);
    }

    m_pSlotFilled = std::make_unique<std::atomic<bool>[]>(m_uiTotalSlots);
    for (unsigned int i = 0; i < m_uiTotalSlots; ++i) {
      m_pSlotFilled[i].store(false, std::memory_order_relaxed);
    }

    m_pDataLength =
        std::make_unique<std::atomic<std::uint64_t>[]>(m_uiTotalSlots);
    for (unsigned int i = 0; i < m_uiTotalSlots; ++i) {
      m_pDataLength[i].store(0, std::memory_order_relaxed);
    }

    m_uiNextOutputSlot.store(0, std::memory_order_relaxed);

    m_bStopThread.store(false, std::memory_order_relaxed);
    m_bThreadRunning.store(false, std::memory_order_relaxed);

    return true;
  } catch (const std::bad_alloc &e) {
    LOG_ERROR("内存分配失败: {}, 需要分配 {} 字节", e.what(), m_uiBufferSize);
    return false;
  }
}

void COrderedDataProcessor::Destroy() {
  // 停止处理线程
  StopProcessing();

  // 无需加锁，单生产者单消费者场景下，调用Destroy时确保没有线程访问资源
  m_pMemoryBuffer.reset();
  m_pSlotFilled.reset();
  m_pDataLength.reset();
  m_uiBufferSize = 0;
  slot_blocks_.clear();
}

void COrderedDataProcessor::Reset() {
  // 无需加锁，单生产者单消费者场景下，调用Reset时确保没有线程访问资源
  // 重置槽位状态
  for (unsigned int i = 0; i < m_uiTotalSlots; ++i) {
    m_pSlotFilled[i].store(false, std::memory_order_relaxed);
    m_pDataLength[i].store(0, std::memory_order_relaxed);
    if (use_external_memory_ && i < slot_blocks_.size()) {
      slot_blocks_[i] = InputBlock{};
    }
  }

  // 重置槽位管理变量
  m_uiNextOutputSlot.store(0, std::memory_order_relaxed);
}

bool COrderedDataProcessor::StartProcessing() {
  if (m_bThreadRunning.load(std::memory_order_relaxed)) {
    return true;
  }

  if ((!use_external_memory_ && !m_pMemoryBuffer) || !m_pSlotFilled ||
      !m_pDataLength || (use_external_memory_ &&
                         slot_blocks_.size() != m_uiTotalSlots)) {
    // 输出更详细的诊断信息，便于定位初始化失败的原因
    LOG_ERROR("OrderedDataProcessor未正确初始化，无法启动处理线程");
    if (!use_external_memory_ && !m_pMemoryBuffer) {
      LOG_ERROR("缺少内部虚拟内存缓冲区，期望大小=%llu", (unsigned long long)m_uiBufferSize);
    }
    if (!m_pSlotFilled) {
      LOG_ERROR("槽位标记数组 m_pSlotFilled 未分配");
    }
    if (!m_pDataLength) {
      LOG_ERROR("槽位长度数组 m_pDataLength 未分配");
    }
    if (use_external_memory_ && slot_blocks_.size() != m_uiTotalSlots) {
      LOG_ERROR("外部模式下 slot_blocks_.size()=%zu, 期望=%u",
                slot_blocks_.size(), m_uiTotalSlots);
    }
    return false;
  }

  if (!m_pInputDataListener) {
    LOG_ERROR("未设置数据监听器，无法启动处理线程");
    return false;
  }

  // 设置停止标志为false
  m_bStopThread.store(false, std::memory_order_release);

  try {
    // 创建处理线程
    m_pOutputThread = std::make_unique<std::thread>(
        &COrderedDataProcessor::OutputThreadFunc, this);
    if (m_cpuId >= 0 &&
        !ThreadAffinity::GetInstance().SetThreadAffinity(
            m_pOutputThread->native_handle(), m_cpuId)) {
#ifdef _WIN32
  LOG_WARN("设置线程CPU亲和性失败，错误码: %d", GetLastError());
#else
  LOG_WARN("设置线程CPU亲和性失败，错误: %s", std::strerror(errno));
#endif
    }
    return true;
  } catch (const std::exception &e) {
    LOG_ERROR("创建处理线程失败: {}", e.what());
    return false;
  }
}

bool COrderedDataProcessor::StopProcessing() {
  // 设置停止标志
  m_bStopThread.store(true, std::memory_order_release);

  if (m_pOutputThread && m_pOutputThread->joinable()) {
    m_pOutputThread->join();
    m_pOutputThread.reset();
  }

  return true;
}

bool COrderedDataProcessor::ProcessData(const InputBlock &block) {
  unsigned int index = static_cast<unsigned int>(block.index %
                                                 m_uiTotalSlots);
  std::uint64_t data_length = static_cast<std::uint64_t>(block.data_length);

  // 检查槽位状态，只有空槽位才能接收数据 - 单生产者场景下无需原子操作
  if (m_pSlotFilled[index].load(std::memory_order_relaxed)) {
    LOG_ERROR("槽位{}已被使用", index);
    return false;
  }

  if (use_external_memory_) {
    if (!block.buffer) {
      LOG_ERROR("外部缓冲区为空，index: %u", index);
      return false;
    }
    if (block.buffer_capacity < block.data_length) {
      LOG_ERROR(
          "外部缓冲区容量不足，index: %u, data_length: %llu, capacity: %zu",
          index, static_cast<unsigned long long>(data_length),
          block.buffer_capacity);
      return false;
    }
    slot_blocks_[index] = block;
  } else {
    if (data_length > m_uiBlockSize) {
      LOG_ERROR(
          "数据长度超过块大小，index: %u, data_length: %llu, block_size: %u",
          index, static_cast<unsigned long long>(data_length),
          m_uiBlockSize);
      return false;
    }

    if (!block.buffer) {
      LOG_ERROR("数据缓冲区为空，index: %u", index);
      return false;
    }

    std::uint64_t offset =
        static_cast<std::uint64_t>(index) * m_uiBlockSize;
    std::memcpy(
        static_cast<unsigned char *>(m_pMemoryBuffer->get()) + offset,
        block.buffer, data_length);
  }

  // 保存数据长度 - 先设置长度再标记填充状态，确保消费者能看到正确的数据长度
  m_pDataLength[index].store(data_length, std::memory_order_relaxed);
  // 使用release内存序标记槽位已填充，确保之前的所有写入对消费者可见
  m_pSlotFilled[index].store(true, std::memory_order_release);

  return true;
}

void COrderedDataProcessor::OutputThreadFunc() {
  // 设置线程状态为运行中
  m_bThreadRunning.store(true, std::memory_order_release);

  while (!m_bStopThread.load(std::memory_order_relaxed)) {
    // 尝试批量处理槽位
    bool success = TryProcessNextSlots();

    // 如果处理失败，则退出处理线程
    if (!success)
      break;

    // 如果当前无数据可处理，则短暂睡眠
    if (!m_pSlotFilled[m_uiNextOutputSlot.load(std::memory_order_relaxed)].load(
            std::memory_order_acquire)) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  }

  m_bThreadRunning.store(false, std::memory_order_release);
}

bool COrderedDataProcessor::TryProcessNextSlots() {
  unsigned int current_slot =
      m_uiNextOutputSlot.load(std::memory_order_relaxed);

  while (!m_bStopThread.load(std::memory_order_relaxed)) {
    // 使用acquire内存序读取槽位状态，确保看到生产者的所有写入
    if (!m_pSlotFilled[current_slot].load(std::memory_order_acquire)) {
      // 当前槽位未填充，无法继续处理
      // LOG_INFO("当前槽位未填充，无法继续处理: index: %u", current_slot);
      break;
    }

    // 获取数据长度
    std::uint64_t data_length =
        m_pDataLength[current_slot].load(std::memory_order_relaxed);

    if (use_external_memory_) {
      InputBlock &slot_block = slot_blocks_[current_slot];
      if (!slot_block.buffer) {
        LOG_ERROR("槽位%u外部缓冲区为空", current_slot);
        m_pSlotFilled[current_slot].store(false, std::memory_order_release);
        unsigned int next_slot =
            (current_slot + 1) % m_uiTotalSlots;
        m_uiNextOutputSlot.store(next_slot, std::memory_order_release);
        current_slot = next_slot;
        continue;
      }

      if (m_pInputDataListener) {
        try {
          m_pInputDataListener->InputData(slot_block.buffer, data_length,
                                          slot_block.user_context);
        } catch (const std::exception &e) {
          LOG_ERROR("调用数据监听器时发生异常: {}", e.what());
          m_uiNextOutputSlot.store(current_slot, std::memory_order_release);
          return false;
        }
      }

      slot_block = InputBlock{};
    } else {
      std::uint64_t offset =
          static_cast<std::uint64_t>(current_slot) * m_uiBlockSize;

      if (m_pInputDataListener) {
        try {
          m_pInputDataListener->InputData(
              static_cast<unsigned char *>(m_pMemoryBuffer->get()) + offset,
              data_length);
        } catch (const std::exception &e) {
          LOG_ERROR("调用数据监听器时发生异常: {}", e.what());
          m_uiNextOutputSlot.store(current_slot, std::memory_order_release);
          return false;
        }
      }
    }

    // 标记槽位状态为空（重置）
    m_pSlotFilled[current_slot].store(false, std::memory_order_release);
    unsigned int next_slot = (current_slot + 1) % m_uiTotalSlots;

    // LOG_INFO("数据发送完成: index: %u, next_slot: %u", current_slot,
    // next_slot);

    // 更新下一个需要处理的槽位
    m_uiNextOutputSlot.store(next_slot, std::memory_order_release);

    current_slot = next_slot;
  }

  return true;
}
