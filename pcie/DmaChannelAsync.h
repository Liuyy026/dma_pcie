#pragma once

#include "DmaChannelCommon.h"
#include "SPSCMemoryQueue.h"
#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>
#include <windows.h>
#include <cstdint>

// #define PCIE_SPEED_TEST

class DmaChannelAsync {
public:
  DmaChannelAsync(HANDLE deviceHandle, unsigned int cpuId);
  ~DmaChannelAsync();

  // 禁止拷贝
  DmaChannelAsync(const DmaChannelAsync &) = delete;
  DmaChannelAsync &operator=(const DmaChannelAsync &) = delete;

  // 初始化和清理
  bool Initialize(unsigned int dmaSize, unsigned int queueSize);

  // 控制接口
  bool Start();
  bool Stop();
  bool IsRunning() const;
  bool Reset();

  // 数据传输
  bool Send(unsigned char *data, unsigned int length);

  // 状态监控接口
  DmaChannelStatus GetStatus() const;

private:
  static const int MAX_OVERLAPPED = 32; // 最大重叠I/O数量

  // 设备相关
  HANDLE m_deviceHandle;
  unsigned int m_cpuId;

  // DMA缓冲区
  std::vector<std::unique_ptr<unsigned char, void (*)(void *)>> m_dmaBuffers;
  unsigned int m_dmaSize;
  SYSTEM_INFO m_systemInfo;

  // 队列管理
  SPSCMemoryQueue m_sendQueue;
  unsigned int m_queueSize;

  // 线程控制
  std::atomic<bool> m_stopFlag;
  std::thread m_sendThread;
  std::atomic<ThreadState> m_threadState;

  // 条件变量
  std::condition_variable m_dataAvailableCV;
  std::mutex m_dataAvailableMutex;

  // 统计信息
  std::atomic<std::uint64_t> m_sendDataCount;
  std::atomic<std::uint64_t> m_lastSendDataCount;
  std::atomic<int> m_step;

  // 多重叠I/O相关
  OVERLAPPED m_overlapped[MAX_OVERLAPPED];
  HANDLE m_events[MAX_OVERLAPPED];

  // 状态监控相关
  mutable std::mutex m_statusMutex;
  DmaChannelStatus m_status;
  std::thread m_monitorThread;         // 独立的状态监控线程
  std::atomic<bool> m_monitorStopFlag; // 监控线程停止标志

  // 内部方法
  void Cleanup();
  void SendData();
  bool InitDMABuffer();
  void DestroyDMABuffer();
  bool InitOverlapped();
  void DestroyOverlapped();
  bool StartSendThread();
  void StopSendThread();
  void MonitorThread();
  void StartMonitorThread(); // 启动监控线程
  void StopMonitorThread();  // 停止监控线程
  void ResetMonitorStatus(); // 重置监控状态
};
