#ifndef _PCIE_DEVICE_H__
#define _PCIE_DEVICE_H__

#include "DeviceHandle.h"
#include "Logger.h"
#include "PcieTypes.h"
#include <memory>
#include <string>

class PcieDevice {
public:
  PcieDevice();
  ~PcieDevice();

  // 禁止拷贝
  PcieDevice(const PcieDevice &) = delete;
  PcieDevice &operator=(const PcieDevice &) = delete;

  // 设备操作
  bool Open(const std::string &devicePath, PcieDeviceWorkMode workMode,
            bool async = false);
  bool Close();
  bool Reset();
  bool IsOpen() const noexcept;

  // BAR空间读写
  bool ReadBAR(long address, DWORD size, void *buffer);
  bool WriteBAR(long address, DWORD size, void *buffer);

  // 硬件特定功能
  void HardwareSyncEnable(bool enable);
  void HardwareSyncOpen(bool open);
  void SetSpeed(int index);

  // 获取设备句柄（供DmaChannel使用）
  HANDLE GetDeviceHandle() const { return m_deviceHandle; }
  HANDLE GetUserBarHandle() const { return m_userBarHandle; }

private:
  DeviceHandle m_deviceHandle;  // 设备句柄
  DeviceHandle m_userBarHandle; // 用户BAR空间句柄
};

#endif // _PCIE_DEVICE_H__
