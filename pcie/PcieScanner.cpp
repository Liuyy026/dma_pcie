#include "PcieScanner.h"

#include "Logger.h"

#ifdef _WIN32
#include "xdma_public.h"
#include <Devpkey.h>
#include <SetupAPI.h>
#include <windows.h>
#else
#include <filesystem>
#include <set>
#endif

std::vector<DeviceInfo> PcieScanner::ScanDevices() const {
#ifdef _WIN32
  std::vector<DeviceInfo> devices;
  GUID guid = GUID_DEVINTERFACE_XDMA;

  HDEVINFO hDeviceInfo = SetupDiGetClassDevs(
      (LPGUID)&guid, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);

  if (hDeviceInfo == INVALID_HANDLE_VALUE) {
    LOG_ERROR("无法获取设备信息集！错误码: %d", GetLastError());
    return devices;
  }

  SP_DEVICE_INTERFACE_DATA deviceInterfaceData;
  deviceInterfaceData.cbSize = sizeof(SP_DEVICE_INTERFACE_DATA);

  SP_DEVINFO_DATA devInfoData;
  devInfoData.cbSize = sizeof(SP_DEVINFO_DATA);

  for (unsigned int index = 0; SetupDiEnumDeviceInterfaces(
           hDeviceInfo, nullptr, &guid, index, &deviceInterfaceData);
       ++index) {
    // 获取所需的缓冲区大小
    DWORD detailLength = 0;
    if (!SetupDiGetDeviceInterfaceDetail(hDeviceInfo, &deviceInterfaceData,
                                         nullptr, 0, &detailLength, nullptr) &&
        GetLastError() != ERROR_INSUFFICIENT_BUFFER) {
      continue;
    }

    // 为设备接口详细信息分配空间
    PSP_DEVICE_INTERFACE_DETAIL_DATA detailData =
        (PSP_DEVICE_INTERFACE_DETAIL_DATA)HeapAlloc(
            GetProcessHeap(), HEAP_ZERO_MEMORY, detailLength);

    if (!detailData) {
      continue;
    }

    detailData->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA);

    // 获取设备接口详细信息
    if (!SetupDiGetDeviceInterfaceDetail(hDeviceInfo, &deviceInterfaceData,
                                         detailData, detailLength, nullptr,
                                         &devInfoData)) {
      HeapFree(GetProcessHeap(), 0, detailData);
      continue;
    }

    // 获取设备总线号、设备名称
    DWORD busNumber = 0;
    DEVPROPTYPE devPropType;
    if (!SetupDiGetDevicePropertyW(
            hDeviceInfo, &devInfoData, &DEVPKEY_Device_BusNumber, &devPropType,
            (PBYTE)&busNumber, sizeof(busNumber), NULL, 0)) {
      LOG_ERROR("无法获取设备总线号！错误码: %d", GetLastError());
    }
    unsigned char deviceName[1024] = {0};
    if (!SetupDiGetDevicePropertyW(
            hDeviceInfo, &devInfoData, &DEVPKEY_Device_DeviceDesc, &devPropType,
            (PBYTE)deviceName, sizeof(deviceName), NULL, 0)) {
      LOG_ERROR("无法获取设备名称！错误码: %d", GetLastError());
    }

    std::wstring deviceNameStr((wchar_t *)deviceName);
    std::string deviceNameStrA(deviceNameStr.begin(),
                               deviceNameStr.end()); // 不支持中文

    devices.emplace_back(index, detailData->DevicePath, busNumber,
                         deviceNameStrA);
    HeapFree(GetProcessHeap(), 0, detailData);
  }

  SetupDiDestroyDeviceInfoList(hDeviceInfo);

  if (devices.empty()) {
    LOG_ERROR("未找到PCIe设备！");
  }

  return devices;
#else
  std::vector<DeviceInfo> devices;
  std::set<std::string> device_roots;
  namespace fs = std::filesystem;
  const fs::path dev_dir("/dev");
  if (!fs::exists(dev_dir) || !fs::is_directory(dev_dir)) {
    LOG_ERROR("/dev 目录不存在，无法扫描XDMA设备");
    return devices;
  }

  for (const auto &entry : fs::directory_iterator(dev_dir)) {
    if (!entry.is_character_file() && !entry.is_block_file()) {
      continue;
    }
    const std::string name = entry.path().filename().string();
    if (name.rfind("xdma", 0) != 0) {
      continue;
    }
    auto underscore = name.find('_');
    if (underscore == std::string::npos) {
      continue;
    }
    std::string root = name.substr(0, underscore);
    device_roots.insert(root);
  }

  unsigned int index = 0;
  for (const auto &root : device_roots) {
    std::string base_path = "/dev/" + root;
    devices.emplace_back(index++, base_path, 0, root);
  }

  if (devices.empty()) {
    LOG_ERROR("未找到Linux XDMA设备");
  }

  return devices;
#endif
}
