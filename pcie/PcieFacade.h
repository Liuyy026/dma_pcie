#ifndef _PCIE_FACADE_H__
#define _PCIE_FACADE_H__

#include "PcieScanner.h"
#include "PcieTypes.h"
#include <memory>
#include <cstdint>

// #define DMA_ASYNC

#ifndef _WIN32
class AlignedBufferPool;
#endif

/**
 * @brief 解耦的DMA发送请求
 * 
 * 过渡设计：支持多种数据源
 * - 当前: direct_ptr 指向实际缓冲
 * - 未来: file_offset + mmap 映射
 * - 未来: iova 用户态DMA
 */
struct DmaSendRequest {
  // 当前模式：直接指针发送
  unsigned char* direct_ptr = nullptr;
  std::uint32_t direct_len = 0;
  
  // 过渡字段：缓冲源标识，用于后续生命周期管理
  void* buffer_owner = nullptr;  // 如缓冲池指针
  
  // 未来mmap模式预留
  // std::uint64_t file_offset = 0;
  // std::uint32_t mmap_len = 0;
  // void* mmap_handle = nullptr;
  
  // 未来UDMA预留
  // std::uint64_t iova = 0;
  // std::uint32_t iova_len = 0;
};

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
  // 旧接口（保留兼容）
  bool Send(unsigned char *data, unsigned int length);
  
  // 新接口（解耦缓冲生命周期）
  // 调用者需自行管理请求中缓冲的生命周期
  bool SendRequest(const DmaSendRequest& request);
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
