#include "DmaChannelAsync.h"
#include "utils/Logger.h"
#include "utils/ThreadAffinity.h"
#include <windows.h>
#include <cstdint>

DmaChannelAsync::DmaChannelAsync(HANDLE deviceHandle, unsigned int cpuId)
    : m_deviceHandle(deviceHandle), m_cpuId(cpuId), m_dmaSize(0),
      m_queueSize(0), m_stopFlag(false), m_threadState(ThreadState::NotStarted),
      m_sendDataCount(0), m_lastSendDataCount(0), m_step(0),
      m_monitorStopFlag(false) {
  for (int i = 0; i < MAX_OVERLAPPED; ++i) {
    m_overlapped[i] = {0, 0};
    m_events[i] = NULL;
  }
  GetSystemInfo(&m_systemInfo);
}

DmaChannelAsync::~DmaChannelAsync() { Cleanup(); }

bool DmaChannelAsync::Initialize(unsigned int dmaSize, unsigned int queueSize) {
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

  // 初始化重叠结构
  if (!InitOverlapped()) {
    return false;
  }

  ResetMonitorStatus();
  StartMonitorThread();

  return true;
}

void DmaChannelAsync::Cleanup() {
  // 在析构函数中确保监控线程停止
  StopMonitorThread();

  Stop();
  DestroyDMABuffer();
  DestroyOverlapped();
}

bool DmaChannelAsync::Start() {
  if (m_threadState == ThreadState::Running) {
    LOG_ERROR("DMA通道已经在运行");
    return false;
  }

  m_stopFlag = false;
  m_sendDataCount = 0;
  m_lastSendDataCount = 0;

  if (!StartSendThread()) {
    return false;
  }

  return true;
}

bool DmaChannelAsync::Stop() {
  // 停止发送线程
  m_stopFlag = true;
  m_dataAvailableCV.notify_one();
  StopSendThread();

  return true;
}

bool DmaChannelAsync::IsRunning() const {
  return m_threadState == ThreadState::Running;
}

bool DmaChannelAsync::Reset() {
  m_sendQueue.Reset();
  m_sendDataCount.store(0, std::memory_order_relaxed);
  m_lastSendDataCount.store(0, std::memory_order_relaxed);
  m_step = 0;
  ResetMonitorStatus();
  return true;
}

bool DmaChannelAsync::Send(unsigned char *data, unsigned int length) {
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

bool DmaChannelAsync::InitDMABuffer() {
  DestroyDMABuffer();

  m_dmaBuffers.reserve(MAX_OVERLAPPED);
  for (int i = 0; i < MAX_OVERLAPPED; ++i) {
    void *buffer = _aligned_malloc(m_dmaSize, m_systemInfo.dwPageSize);
    if (!buffer) {
      LOG_ERROR("分配第 %d 个DMA缓冲区失败", i);
      DestroyDMABuffer(); // 清理已分配的缓冲区
      return false;
    }
    m_dmaBuffers.emplace_back(static_cast<unsigned char *>(buffer),
                              _aligned_free);
  }

  return true;
}

void DmaChannelAsync::DestroyDMABuffer() { m_dmaBuffers.clear(); }

bool DmaChannelAsync::InitOverlapped() {
  DestroyOverlapped();

  for (int i = 0; i < MAX_OVERLAPPED; ++i) {
    m_overlapped[i] = {0, 0};
    m_events[i] = CreateEvent(nullptr, TRUE, FALSE, nullptr);
    if (!m_events[i]) {
      LOG_ERROR("创建第 %d 个重叠事件对象失败", i);
      DestroyOverlapped();
      return false;
    }
    m_overlapped[i].hEvent = m_events[i];
  }

  return true;
}

void DmaChannelAsync::DestroyOverlapped() {
  for (int i = 0; i < MAX_OVERLAPPED; ++i) {
    if (m_events[i]) {
      CloseHandle(m_events[i]);
      m_events[i] = NULL;
    }
  }
}

bool DmaChannelAsync::StartSendThread() {
  try {
    m_sendThread = std::thread(&DmaChannelAsync::SendData, this);
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

void DmaChannelAsync::StopSendThread() {
  if (m_sendThread.joinable()) {
    m_threadState = ThreadState::Stopping;
    m_sendThread.join();
    m_threadState = ThreadState::Stopped;
  }
}

void DmaChannelAsync::SendData() {
  int currentOverlapped = 0;
  bool pendingIO[MAX_OVERLAPPED] = {false};
  m_step = 0;

  try {
    while (!m_stopFlag) {
      // 检查之前的异步操作是否完成
      bool has_error = false;
      for (int i = 0; i < MAX_OVERLAPPED; ++i) {
        if (pendingIO[i]) {
          DWORD bytesWritten = 0;
          if (GetOverlappedResult(m_deviceHandle, &m_overlapped[i],
                                  &bytesWritten, FALSE)) {
            m_sendDataCount.fetch_add(bytesWritten, std::memory_order_relaxed);
            pendingIO[i] = false;
            ResetEvent(m_events[i]);
          } else if (GetLastError() != ERROR_IO_INCOMPLETE) {
            LOG_ERROR("DMA写入失败，错误代码: %u", GetLastError());
            has_error = true;
            break;
          }
        }
      }

      if (has_error || m_stopFlag)
        break;

      // 如果所有重叠结构都在使用中，等待一个完成，但设置超时以便检查停止标志
      if (std::all_of(pendingIO, pendingIO + MAX_OVERLAPPED,
                      [](bool b) { return b; })) {
        m_step = 1;
        // 使用10ms作为超时值，而不是INFINITE
        DWORD waitResult =
            WaitForMultipleObjects(MAX_OVERLAPPED, m_events, FALSE, 10);
        if (waitResult == WAIT_FAILED) {
          has_error = true;
          LOG_ERROR("等待I/O事件失败，错误代码: %u", GetLastError());
          break;
        }

        continue;
      }

#ifndef PCIE_SPEED_TEST
      // 获取新数据
      {
        std::unique_lock<std::mutex> lock(m_dataAvailableMutex);
        if (m_sendQueue.IsEmpty()) {
          m_step = 2;
          m_dataAvailableCV.wait(
              lock, [this] { return !m_sendQueue.IsEmpty() || m_stopFlag; });
        }
      }
#endif

      m_step = 3;

      if (m_stopFlag)
        break;

      // 找到一个空闲的重叠结构
      while (pendingIO[currentOverlapped])
        currentOverlapped = (currentOverlapped + 1) % MAX_OVERLAPPED;

      unsigned char *currentDmaBuffer = m_dmaBuffers[currentOverlapped].get();

#ifndef PCIE_SPEED_TEST
      unsigned int dataSize = m_sendQueue.Get(currentDmaBuffer, m_dmaSize);
      if (dataSize == 0)
        continue;
#else
      unsigned int dataSize = m_dmaSize;
#endif

      DWORD bytesWritten = 0;
      if (!WriteFile(m_deviceHandle, currentDmaBuffer, dataSize, &bytesWritten,
                     &m_overlapped[currentOverlapped])) {
        DWORD lastError = GetLastError();
        if (lastError == ERROR_IO_PENDING) {
          pendingIO[currentOverlapped] = true;
          currentOverlapped = (currentOverlapped + 1) % MAX_OVERLAPPED;
        } else {
          LOG_ERROR("DMA写入失败，错误代码: %u", lastError);
          break;
        }
      } else {
        m_step = 4;
        m_sendDataCount.fetch_add(bytesWritten, std::memory_order_relaxed);
      }
    }
    m_step = 5;
  } catch (const std::exception &e) {
    m_step = 6;
    LOG_ERROR("SendData发生异常: %s", e.what());
  } catch (...) {
    m_step = 7;
    LOG_ERROR("SendData发生未知异常");
  }

  // 等待所有未完成的I/O操作，设置1秒超时
  const DWORD CLEANUP_TIMEOUT_MS = 500; // 500ms超时
  HANDLE pendingEvents[MAX_OVERLAPPED];
  int pendingCount = 0;

  // 收集所有未完成的I/O操作的事件句柄
  for (int i = 0; i < MAX_OVERLAPPED; ++i) {
    if (pendingIO[i]) {
      pendingEvents[pendingCount++] = m_events[i];
    }
  }

  // 如果有未完成的I/O操作，等待它们完成或超时
  if (pendingCount > 0) {
    DWORD waitResult = WaitForMultipleObjects(pendingCount, pendingEvents, TRUE,
                                              CLEANUP_TIMEOUT_MS);
    if (waitResult == WAIT_TIMEOUT) {
      LOG_ERROR("等待I/O操作完成超时，强制取消剩余操作");
      // 超时后强制取消所有未完成的I/O操作
      for (int i = 0; i < MAX_OVERLAPPED; ++i) {
        if (pendingIO[i]) {
          CancelIoEx(m_deviceHandle, &m_overlapped[i]);
        }
      }
    }
  }
}

void DmaChannelAsync::ResetMonitorStatus() {
  std::lock_guard<std::mutex> lock(m_statusMutex);

  // 重置状态结构体
  m_status.currentSpeed = 0;
  m_status.totalBytesSent = 0;
  m_status.queueSize = m_queueSize;
  m_status.queueUsed = 0;
  m_status.queueUsagePercent = 0.0f;
  m_status.threadState = m_threadState;
  m_status.step = m_step;
}

void DmaChannelAsync::StartMonitorThread() {
  try {
    m_monitorStopFlag = false;
    m_monitorThread = std::thread(&DmaChannelAsync::MonitorThread, this);
  } catch (const std::system_error &e) {
    LOG_ERROR("创建状态监控线程失败: %s", e.what());
  }
}

void DmaChannelAsync::StopMonitorThread() {
  m_monitorStopFlag = true;
  if (m_monitorThread.joinable()) {
    m_monitorThread.join();
  }
}

void DmaChannelAsync::MonitorThread() {
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

DmaChannelStatus DmaChannelAsync::GetStatus() const {
  std::lock_guard<std::mutex> lock(m_statusMutex);
  return m_status;
}
