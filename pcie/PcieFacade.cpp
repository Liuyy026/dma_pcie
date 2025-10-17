#include "PcieFacade.h"
#include "Logger.h"
#ifndef _WIN32
#include "AlignedBufferPool.h"
#endif

PcieFacade::PcieFacade()
    : m_scanner(std::make_unique<PcieScanner>()),
      m_device(std::make_unique<DeviceType>()),
      m_workMode(PcieDeviceWorkMode::SendMode) {}

PcieFacade::~PcieFacade() { CloseDevice(); }

bool PcieFacade::ScanDevice(unsigned int &deviceCount) {
  m_deviceList = m_scanner->ScanDevices();
  deviceCount = static_cast<unsigned int>(m_deviceList.size());
  return !m_deviceList.empty();
}

bool PcieFacade::GetDeviceInfo(unsigned int deviceIndex,
                               std::string &deviceInfo) const {
  if (deviceIndex >= m_deviceList.size()) {
    LOG_ERROR("设备索引超出范围: %u", deviceIndex);
    return false;
  }
  deviceInfo = m_deviceList[deviceIndex].deviceName + " (bus_number: " +
               std::to_string(m_deviceList[deviceIndex].busNumber) + ")";
  return true;
}

bool PcieFacade::OpenDevice(unsigned int deviceIndex,
                            PcieDeviceWorkMode workMode,
                            unsigned int sendDmaSize,
                            unsigned int sendBufferSize, unsigned int cpuId) {
  if (deviceIndex >= m_deviceList.size()) {
    LOG_ERROR("设备索引超出范围: %u", deviceIndex);
    return false;
  }

  m_workMode = workMode;

#ifdef _WIN32
#ifdef DMA_ASYNC
  if (!m_device->Open(m_deviceList[deviceIndex].devicePath, workMode, true)) {
#else
  if (!m_device->Open(m_deviceList[deviceIndex].devicePath, workMode)) {
#endif
#else
  if (!m_device->Open(m_deviceList[deviceIndex].devicePath, workMode)) {
#endif
      LOG_ERROR("打开设备失败: %s", m_deviceList[deviceIndex].devicePath.c_str());
      return false;
  }

  // 根据工作模式创建DMA通道
  if (m_workMode == PcieDeviceWorkMode::SendMode ||
      m_workMode == PcieDeviceWorkMode::SendAndReceiveMode) {
#ifdef _WIN32
#ifdef DMA_ASYNC
    m_dmaChannel =
        std::make_unique<ChannelType>(m_device->GetDeviceHandle(), cpuId);
#else
    m_dmaChannel =
        std::make_unique<ChannelType>(m_device->GetDeviceHandle(), cpuId);
#endif
#else
    int send_handle = m_device->GetSendHandle();
    if (send_handle < 0) {
      LOG_ERROR("无可用的Linux发送句柄");
      CloseDevice();
      return false;
    }
    m_dmaChannel =
        std::make_unique<ChannelType>(send_handle, cpuId);
#endif
      if (!m_dmaChannel->Initialize(sendDmaSize, sendBufferSize)) {
        LOG_ERROR("DMA 通道初始化失败");
        CloseDevice();
        return false;
      }
#ifndef _WIN32
    if (m_sendBufferPool) {
      m_dmaChannel->SetBufferPool(m_sendBufferPool);
    }
#endif
  }

  return true;
}

bool PcieFacade::CloseDevice() {
  if (m_dmaChannel) {
    m_dmaChannel->Stop();
    m_dmaChannel.reset();
  }

  if (m_device) {
    m_device->Close();
  }

  return true;
}

bool PcieFacade::Start() {
  if (!m_device->IsOpen()) {
    LOG_ERROR("设备未打开，无法启动");
    return false;
  }


  LOG_INFO("开始启动 DMA 通道");
  if (m_dmaChannel && !m_dmaChannel->Start()) {
    LOG_ERROR("DMA 通道启动失败");
    return false;
  }

  LOG_INFO("设备与 DMA 通道已启动");

  return true;
}

bool PcieFacade::Stop() {
  if (m_dmaChannel) {
    return m_dmaChannel->Stop();
  }
  return true;
}

bool PcieFacade::IsRunning() const {
  return m_dmaChannel && m_dmaChannel->IsRunning();
}

bool PcieFacade::Reset() {
  if (!m_device->Reset()) {
    return false;
  }

  if (m_dmaChannel) {
    m_dmaChannel->Reset();
  }

  return true;
}

void PcieFacade::HardwareSyncEnable(bool enable) {
  m_device->HardwareSyncEnable(enable);
}

void PcieFacade::HardwareSyncOpen(bool open) {
  m_device->HardwareSyncOpen(open);
}

void PcieFacade::SetSpeed(int index) { m_device->SetSpeed(index); }

bool PcieFacade::Send(unsigned char *data, unsigned int length) {
  if (!m_dmaChannel) {
    LOG_ERROR("DMA通道未初始化");
    return false;
  }


  LOG_INFO("PcieFacade::Send 请求: ptr=%p, len=%u", data, length);
  bool res = m_dmaChannel->Send(data, length);
  LOG_INFO("PcieFacade::Send 返回: %d", res ? 1 : 0);
  return res;
}

#ifndef _WIN32
void PcieFacade::SetBufferPool(std::shared_ptr<AlignedBufferPool> pool) {
  m_sendBufferPool = std::move(pool);
  if (m_dmaChannel) {
    m_dmaChannel->SetBufferPool(m_sendBufferPool);
  }
}
#endif

bool PcieFacade::IsDeviceOpen() const noexcept {
  return m_device && m_device->IsOpen();
}

DmaChannelStatus PcieFacade::GetStatus() const noexcept {
  return m_dmaChannel ? m_dmaChannel->GetStatus() : DmaChannelStatus();
}
