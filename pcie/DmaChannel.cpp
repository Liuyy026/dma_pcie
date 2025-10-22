#include "DmaChannel.h"
#include "utils/ThreadAffinity.h"
#include <system_error>
#include <windows.h>
#include <cstdint>

DmaChannel::DmaChannel(HANDLE deviceHandle, unsigned int cpuId)
    : m_deviceHandle(deviceHandle), m_cpuId(cpuId), m_dmaSize(0),
      m_queueSize(0), m_stopFlag(false), m_threadState(ThreadState::NotStarted),
      m_sendDataCount(0), m_lastSendDataCount(0), m_step(0),
      m_monitorStopFlag(false) {
  GetSystemInfo(&m_systemInfo);
}

DmaChannel::~DmaChannel() { Cleanup(); }

bool DmaChannel::Initialize(unsigned int dmaSize, unsigned int queueSize) {
  if (m_threadState == ThreadState::Running) {
    LOG_ERROR("DMA通道正在运行，无法初始化");
    return false;
  }

  m_dmaSize = dmaSize;
  m_queueSize = queueSize;

  if (!InitDMABuffer()) {
    return false;
  }

  if (!m_sendQueue.Init(m_queueSize)) {
    LOG_ERROR("初始化发送队列失败");
    return false;
  }

  m_threadState = ThreadState::NotStarted;
  m_stopFlag = false;
  m_sendDataCount = 0;
  m_lastSendDataCount = 0;
  m_step = 0;

  ResetMonitorStatus();
  StartMonitorThread();

  return true;
}

void DmaChannel::Cleanup() {
  // 在析构函数中确保监控线程停止
  StopMonitorThread();

  Stop();
  DestroyDMABuffer();
}

bool DmaChannel::Start() {
  if (m_threadState == ThreadState::Running) {
    LOG_ERROR("DMA通道已经在运行");
    return false;
  }

  m_stopFlag = false;
  m_sendDataCount = 0;
  m_lastSendDataCount = 0;
  m_step = 0;

  {
    std::lock_guard<std::mutex> lock(m_statusMutex);
    m_firstDataTime = std::chrono::steady_clock::time_point{};
    m_status.elapsedNs = 0;
  }

  if (!StartSendThread()) {
    return false;
  }

  return true;
}

bool DmaChannel::Stop() {
  // 停止发送线程
  m_stopFlag = true;

  // 取消正在进行的同步I/O操作
  if (!CancelIoEx(m_deviceHandle, nullptr)) {
    DWORD lastError = GetLastError();
    if (lastError !=
        ERROR_NOT_FOUND) // ERROR_NOT_FOUND 表示没有待取消的I/O，这是正常的
    {
      LOG_ERROR("取消I/O操作失败，错误代码: %u", lastError);
    }
  }

  m_dataAvailableCV.notify_one();
  StopSendThread();

  return true;
}

bool DmaChannel::IsRunning() const {
  return m_threadState == ThreadState::Running;
}

bool DmaChannel::Reset() {
  m_sendQueue.Reset();
  m_sendDataCount.store(0, std::memory_order_relaxed);
  m_lastSendDataCount.store(0, std::memory_order_relaxed);
  m_step = 0;
  ResetMonitorStatus();
  return true;
}

bool DmaChannel::Send(unsigned char *data, unsigned int length) {
  if (!IsRunning()) {
    LOG_ERROR("DMA通道未运行，无法发送数据");
    return false;
  }

  if (!m_sendQueue.PutFixedLength(data, length)) {
    return false;
  }

  m_dataAvailableCV.notify_one();
  return true;
}

bool DmaChannel::InitDMABuffer() {
  DestroyDMABuffer();

  // 只需要一个DMA缓冲区
  void *buffer = _aligned_malloc(m_dmaSize, m_systemInfo.dwPageSize);
  if (!buffer) {
    LOG_ERROR("分配DMA缓冲区失败");
    return false;
  }
  m_dmaBuffers.emplace_back(static_cast<unsigned char *>(buffer),
                            _aligned_free);

  return true;
}

void DmaChannel::DestroyDMABuffer() { m_dmaBuffers.clear(); }

bool DmaChannel::StartSendThread() {
  try {
    m_sendThread = std::thread(&DmaChannel::SendData, this);
    if (!ThreadAffinity::GetInstance().SetThreadAffinity(
            m_sendThread.native_handle(), m_cpuId)) {
      LOG_ERROR("设置线程CPU亲和性失败，错误码: %d", GetLastError());
    }

    // 设置线程优先级
    SetThreadPriority(m_sendThread.native_handle(), THREAD_PRIORITY_HIGHEST);

    m_threadState = ThreadState::Running;
    return true;
  } catch (const std::system_error &e) {
    LOG_ERROR("创建发送线程失败: %s", e.what());
    m_threadState = ThreadState::Error;
    return false;
  }
}

void DmaChannel::StopSendThread() {
  if (m_sendThread.joinable()) {
    m_threadState = ThreadState::Stopping;
    m_sendThread.join();
    m_threadState = ThreadState::Stopped;
  }
}

void DmaChannel::SendData() {
  unsigned char *dmaBuffer = m_dmaBuffers[0].get();
  m_step = 0;

  try {
    while (!m_stopFlag) {
#ifndef PCIE_SPEED_TEST
      // 等待新数据
      {
        std::unique_lock<std::mutex> lock(m_dataAvailableMutex);
        if (m_sendQueue.IsEmpty()) {
          m_step = 1;
          m_dataAvailableCV.wait(
              lock, [this] { return !m_sendQueue.IsEmpty() || m_stopFlag; });
        }
      }
#endif

      m_step = 2;

      if (m_stopFlag)
        break;

#ifndef PCIE_SPEED_TEST
      // 从队列获取数据
      unsigned int dataSize = m_sendQueue.Get(dmaBuffer, m_dmaSize);
      if (dataSize == 0)
        continue;
#else
      unsigned int dataSize = m_dmaSize;
#endif

      // 同步写入数据
      DWORD bytesWritten = 0;
      m_step = 3;
      if (!WriteFile(m_deviceHandle, dmaBuffer, dataSize, &bytesWritten,
                     nullptr)) {
        DWORD lastError = GetLastError();
        if (lastError !=
            ERROR_OPERATION_ABORTED) // 忽略因Stop()调用而取消的操作
        {
          LOG_ERROR("DMA写入失败，错误代码: %u", lastError);
        }
        break;
      }

      m_step = 4;
      m_sendDataCount.fetch_add(bytesWritten, std::memory_order_relaxed);
    }
    m_step = 5;
  } catch (const std::exception &e) {
    m_step = 6;
    LOG_ERROR("SendData发生异常: %s", e.what());
  } catch (...) {
    m_step = 7;
    LOG_ERROR("SendData发生未知异常");
  }
}

void DmaChannel::ResetMonitorStatus() {
  std::lock_guard<std::mutex> lock(m_statusMutex);

  // 重置状态结构体
  m_status.currentSpeed = 0;
  m_status.totalBytesSent = 0;
  m_status.queueSize = m_queueSize;
  m_status.queueUsed = 0;
  m_status.queueUsagePercent = 0.0f;
  m_status.threadState = m_threadState;
  m_status.step = m_step;
  m_status.elapsedNs = 0;
  m_firstDataTime = std::chrono::steady_clock::time_point{};
}

void DmaChannel::StartMonitorThread() {
  try {
    m_monitorStopFlag = false;
    m_monitorThread = std::thread(&DmaChannel::MonitorThread, this);
  } catch (const std::system_error &e) {
    LOG_ERROR("创建状态监控线程失败: %s", e.what());
  }
}

void DmaChannel::StopMonitorThread() {
  m_monitorStopFlag = true;
  if (m_monitorThread.joinable()) {
    m_monitorThread.join();
  }
}

void DmaChannel::MonitorThread() {
  const auto updateInterval = std::chrono::milliseconds(1000);

  std::chrono::steady_clock::time_point lastSpeedUpdateTime =
      std::chrono::steady_clock::now();

  while (!m_monitorStopFlag) {
    // 更新状态
    {
      std::lock_guard<std::mutex> lock(m_statusMutex);

      // 更新基本状态
      m_status.threadState = m_threadState;
      m_status.step = m_step;

      // 更新队列状态
      m_status.queueSize = m_queueSize;
      m_status.queueUsed = m_sendQueue.GetDataSize();
      m_status.queueUsagePercent =
          m_queueSize > 0 ? (float)m_status.queueUsed / m_queueSize * 100.0f
                          : 0.0f;

      // 更新DMA传输状态
      std::uint64_t currentSendDataCount =
          m_sendDataCount.load(std::memory_order_relaxed);
      m_status.totalBytesSent = currentSendDataCount;

      // 计算当前速度
      auto now = std::chrono::steady_clock::now();
      if (currentSendDataCount > 0 && m_firstDataTime == std::chrono::steady_clock::time_point{}) {
        m_firstDataTime = now;
      }
      if (m_firstDataTime != std::chrono::steady_clock::time_point{}) {
        auto elapsed_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(now - m_firstDataTime).count();
        m_status.elapsedNs = elapsed_ns > 0 ? static_cast<std::uint64_t>(elapsed_ns) : 0;
      } else {
        m_status.elapsedNs = 0;
      }
      auto duration = std::chrono::duration_cast<std::chrono::seconds>(
                          now - lastSpeedUpdateTime)
                          .count();

      m_status.currentSpeed =
          (currentSendDataCount -
           m_lastSendDataCount.load(std::memory_order_relaxed)) /
          (duration ? duration : 1);
      m_lastSendDataCount.store(currentSendDataCount,
                                std::memory_order_relaxed);
      lastSpeedUpdateTime = now;
    }

    // 休眠一段时间
    std::this_thread::sleep_for(updateInterval);
  }
}

DmaChannelStatus DmaChannel::GetStatus() const {
  std::lock_guard<std::mutex> lock(m_statusMutex);
  return m_status;
}
