#include "PcieDevice.h"
#include "Logger.h"
#include "xdma_public.h"
#include <system_error>

PcieDevice::PcieDevice() {}

PcieDevice::~PcieDevice() { Close(); }

bool PcieDevice::Open(const std::string &devicePath,
                      PcieDeviceWorkMode workMode, bool async) {
  DWORD access = GENERIC_READ | GENERIC_WRITE;
  std::string path = devicePath;
  if (workMode == PcieDeviceWorkMode::SendMode) {
    access = GENERIC_WRITE;
    path = devicePath + "\\h2c_0";
  } else if (workMode == PcieDeviceWorkMode::ReceiveMode) {
    access = GENERIC_READ;
    path = devicePath + "\\c2h_0";
  }

  // 打开设备句柄
  if (!m_deviceHandle.open(path, access, 0, async)) {
    std::error_code ec(GetLastError(), std::system_category());
    LOG_ERROR("打开设备失败: %s", ec.message().c_str());
    return false;
  }

  // 打开用户BAR空间
  std::string userBarPath = devicePath + "\\user";
  if (!m_userBarHandle.open(userBarPath, GENERIC_READ | GENERIC_WRITE, 0,
                            false)) {
    std::error_code ec(GetLastError(), std::system_category());
    LOG_ERROR("打开用户BAR空间失败: %s", ec.message().c_str());
    Close();
    return false;
  }

  return true;
}

bool PcieDevice::Close() {
  m_deviceHandle.close();
  m_userBarHandle.close();
  return true;
}

bool PcieDevice::Reset() {
  if (!IsOpen()) {
    LOG_ERROR("设备未打开，无法复位");
    return false;
  }

  DWORD value = 1;
  if (!WriteBAR(0x00, sizeof(DWORD), &value)) {
    LOG_ERROR("写入复位寄存器失败");
    return false;
  }

  Sleep(50);

  value = 0;
  if (!WriteBAR(0x00, sizeof(DWORD), &value)) {
    LOG_ERROR("清除复位寄存器失败");
    return false;
  }

  Sleep(100);

  return true;
}

bool PcieDevice::IsOpen() const noexcept {
  return m_deviceHandle.isValid() && m_userBarHandle.isValid();
}

bool PcieDevice::ReadBAR(long address, DWORD size, void *buffer) {
  if (!IsOpen()) {
    LOG_ERROR("设备未打开，无法读取BAR空间");
    return false;
  }

  if (INVALID_SET_FILE_POINTER ==
      SetFilePointer(m_userBarHandle, address, nullptr, FILE_BEGIN)) {
    LOG_ERROR("设置文件指针失败: %d", GetLastError());
    return false;
  }

  DWORD bytesRead = 0;
  if (!ReadFile(m_userBarHandle, buffer, size, &bytesRead, nullptr)) {
    LOG_ERROR("读取BAR空间失败: %d", GetLastError());
    return false;
  }

  if (bytesRead != size) {
    LOG_ERROR("读取的字节数不匹配，期望: %u, 实际: %u", size, bytesRead);
    return false;
  }

  return true;
}

bool PcieDevice::WriteBAR(long address, DWORD size, void *buffer) {
  if (!IsOpen()) {
    LOG_ERROR("设备未打开，无法写入BAR空间");
    return false;
  }

  if (INVALID_SET_FILE_POINTER ==
      SetFilePointer(m_userBarHandle, address, nullptr, FILE_BEGIN)) {
    LOG_ERROR("设置文件指针失败: %d", GetLastError());
    return false;
  }

  DWORD bytesWritten = 0;
  if (!WriteFile(m_userBarHandle, buffer, size, &bytesWritten, nullptr)) {
    LOG_ERROR("写入BAR空间失败: %d", GetLastError());
    return false;
  }

  if (bytesWritten != size) {
    LOG_ERROR("写入的字节数不匹配，期望: %u, 实际: %u", size, bytesWritten);
    return false;
  }

  return true;
}

void PcieDevice::HardwareSyncEnable(bool enable) {
  if (!IsOpen()) {
    LOG_ERROR("设备未打开，无法设置硬件同步");
    return;
  }

  DWORD value = 0;
  ReadBAR(0x28, sizeof(DWORD), &value);

  if (enable)
    value |= 0x00000001;
  else
    value &= 0xFFFFFFFE;

  WriteBAR(0x28, sizeof(DWORD), &value);
}

void PcieDevice::HardwareSyncOpen(bool open) {
  if (!IsOpen()) {
    LOG_ERROR("设备未打开，无法设置硬件同步模式");
    return;
  }

  DWORD value = 0;

  if (open)
    value |= 0x00000001;
  else
    value &= 0xFFFFFFFE;

  WriteBAR(0x08, sizeof(DWORD), &value);
}

void PcieDevice::SetSpeed(int index) {
  if (!IsOpen()) {
    LOG_ERROR("设备未打开，无法设置速度");
    return;
  }

  DWORD value = 0;
  switch (index) {
  case 0:
    value = 0;
    break;
  case 1:
    value = 1;
    break;
  case 2:
    value = 2;
    break;
  case 3:
    value = 3;
    break;
  case 4:
    value = 4;
    break;
  default:
    LOG_ERROR("无效的速度索引: %d", index);
    return;
  }

  WriteBAR(0x24, sizeof(DWORD), &value);
}