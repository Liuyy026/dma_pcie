#ifndef _PCIE_SCANNER_H__
#define _PCIE_SCANNER_H__

#ifdef _WIN32
#include <Windows.h>
#endif
#include <cstdint>
#include <string>
#include <vector>

// 设备信息结构体
struct DeviceInfo {
  unsigned int index;
  std::string devicePath;
  std::uint32_t busNumber;
  std::string deviceName;

  DeviceInfo(unsigned int idx, const std::string &path, std::uint32_t bus_num,
             const std::string &name)
      : index(idx), devicePath(path), busNumber(bus_num), deviceName(name) {}
};

class PcieScanner {
public:
  // 扫描设备并返回找到的设备列表
  std::vector<DeviceInfo> ScanDevices() const;
};

#endif // _PCIE_SCANNER_H__
