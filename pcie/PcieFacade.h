#ifndef _PCIE_FACADE_H__
#define _PCIE_FACADE_H__

#include "PcieScanner.h"
#include "PcieTypes.h"
#include <memory>

// #define DMA_ASYNC

#ifndef _WIN32
class AlignedBufferPool;
#endif

#ifdef _WIN32
#include "PcieDevice.h"
#ifdef DMA_ASYNC
#include "DmaChannelAsync.h"
#else
#include "DmaChannel.h"
#endif
#else
#include "DmaChannelLinux.h"
#include "PcieDeviceLinux.h"
#endif

class PcieFacade {
public:
  PcieFacade();
  ~PcieFacade();

  // 禁止拷贝
  PcieFacade(const PcieFacade &) = delete;
  PcieFacade &operator=(const PcieFacade &) = delete;

  // 设备扫描与信息
  bool ScanDevice(unsigned int &deviceCount);
  bool GetDeviceInfo(unsigned int deviceIndex, std::string &deviceInfo) const;

  // 设备操作
  bool OpenDevice(unsigned int deviceIndex, PcieDeviceWorkMode workMode,
                  unsigned int sendDmaSize, unsigned int sendBufferSize,
                  unsigned int cpuId);
  bool CloseDevice();

  // 设备控制
  bool Start();
  bool Stop();
  bool IsRunning() const;
  bool Reset();

  // 硬件配置
  void HardwareSyncEnable(bool enable);
  void HardwareSyncOpen(bool open);
  void SetSpeed(int index);

  // 数据传输
  bool Send(unsigned char *data, unsigned int length);
#ifndef _WIN32
  void SetBufferPool(std::shared_ptr<AlignedBufferPool> pool);
#endif

  // 状态查询
  bool IsDeviceOpen() const noexcept;
  DmaChannelStatus GetStatus() const noexcept;

private:
  std::unique_ptr<PcieScanner> m_scanner;
#ifdef _WIN32
  using DeviceType = PcieDevice;
#ifdef DMA_ASYNC
  using ChannelType = DmaChannelAsync;
#else
  using ChannelType = DmaChannel;
#endif
#else
  using DeviceType = PcieDeviceLinux;
  using ChannelType = DmaChannelLinux;
#endif
  std::unique_ptr<DeviceType> m_device;
  std::unique_ptr<ChannelType> m_dmaChannel;

  std::vector<DeviceInfo> m_deviceList;
  PcieDeviceWorkMode m_workMode;
#ifndef _WIN32
  std::shared_ptr<AlignedBufferPool> m_sendBufferPool;
#endif
};

#endif // _PCIE_FACADE_H__
