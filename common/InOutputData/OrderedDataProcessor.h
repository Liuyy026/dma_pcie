#ifndef _ORDERED_DATA_PROCESSOR_H_
#define _ORDERED_DATA_PROCESSOR_H_

#include "InputBlock.h"
#include "VirtualMemoryBuffer.h"
#include <atomic>
#include <cstdint>
#include <memory>
#include <thread>
#include <vector>

// 定义最大乱序支持范围
#define MAX_DISORDER_GROUPS 8
// 定义缓存行大小，用于对齐，避免伪共享
#define CACHE_LINE_SIZE 64

class IInputDataListener {
public:
  virtual ~IInputDataListener(){};
  virtual void InputData(unsigned char *pData, std::uint64_t uiDataLength,
                         void *pParam = NULL) = 0;
};

// 定义数据槽的状态
enum class SlotStatus : uint8_t {
  EMPTY = 0,      // 空槽位
  FILLED = 1,     // 已填充数据
  PROCESSING = 2, // 正在处理
  PROCESSED = 3   // 已处理完成
};

class COrderedDataProcessor {
public:
  COrderedDataProcessor();
  ~COrderedDataProcessor();

  // 初始化函数，传入必要的参数
  bool Initialize(unsigned int io_request_num, unsigned int block_size,
                  int cpu_id);

  // 销毁和重置
  void Destroy();
  void Reset();

  // 处理返回的数据
  bool ProcessData(const InputBlock &block);

  // 设置数据监听器
  void SetInputDataListener(IInputDataListener *pListener) {
    m_pInputDataListener = pListener;
  }

  void EnableExternalMemoryMode(bool enable) {
    use_external_memory_ = enable;
  }

  bool IsExternalMemoryMode() const { return use_external_memory_; }

  // 获取当前下一个输出槽位的值
  unsigned int GetNextOutputSlot() const {
    return m_uiNextOutputSlot.load(std::memory_order_relaxed);
  }

  // 启动和停止数据处理线程
  bool StartProcessing();
  bool StopProcessing();

  // 判断处理线程是否正在运行
  bool IsProcessing() const {
    return m_bThreadRunning.load(std::memory_order_relaxed);
  }

private:
  // 输出线程函数
  void OutputThreadFunc();

  // 尝试批量处理槽位的数据
  bool TryProcessNextSlots();

  // 内存管理
  std::unique_ptr<VirtualMemoryBuffer> m_pMemoryBuffer;
  std::uint64_t m_uiBufferSize;

  // 配置参数
  unsigned int m_uiBlockSize;
  unsigned int m_uiIORequestNum;
  unsigned int m_uiTotalSlots;
  int m_cpuId;

  // 槽位管理（使用缓存行对齐避免伪共享）
  alignas(CACHE_LINE_SIZE)
      std::atomic<unsigned int> m_uiNextOutputSlot; // 下一个要输出的槽位

  // 简化的槽位状态管理（用于单生产者单消费者场景）
  std::unique_ptr<std::atomic<bool>[]> m_pSlotFilled; // 生产者设置，消费者清除
  std::unique_ptr<std::atomic<std::uint64_t>[]>
      m_pDataLength; // 每个槽位的数据长度

  bool use_external_memory_;
  std::vector<InputBlock> slot_blocks_;

  // 线程相关
  std::unique_ptr<std::thread> m_pOutputThread;
  std::atomic<bool> m_bStopThread;
  std::atomic<bool> m_bThreadRunning;

  // 数据监听器
  IInputDataListener *m_pInputDataListener;
};

#endif // _ORDERED_DATA_PROCESSOR_H_
