#ifndef _DEVICE_HANDLE_H__
#define _DEVICE_HANDLE_H__

#include <string>
#include <windows.h>

// 设备句柄RAII包装类
class DeviceHandle {
public:
  DeviceHandle() noexcept : m_handle(INVALID_HANDLE_VALUE) {}
  ~DeviceHandle() { close(); }

  // 禁止拷贝
  DeviceHandle(const DeviceHandle &) = delete;
  DeviceHandle &operator=(const DeviceHandle &) = delete;

  // 允许移动
  DeviceHandle(DeviceHandle &&other) noexcept : m_handle(other.m_handle) {
    other.m_handle = INVALID_HANDLE_VALUE;
  }

  DeviceHandle &operator=(DeviceHandle &&other) noexcept {
    if (this != &other) {
      close();
      m_handle = other.m_handle;
      other.m_handle = INVALID_HANDLE_VALUE;
    }
    return *this;
  }

  bool open(const std::string &path, DWORD access, DWORD share = 0,
            bool async = false) noexcept {
    close();
    if (async) {
      m_handle =
          CreateFile(path.c_str(), access, share, nullptr, OPEN_EXISTING,
                     FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED, nullptr);
    } else {
      m_handle = CreateFile(path.c_str(), access, share, nullptr, OPEN_EXISTING,
                            FILE_ATTRIBUTE_NORMAL, nullptr);
    }
    return isValid();
  }

  void close() noexcept {
    if (isValid()) {
      CloseHandle(m_handle);
      m_handle = INVALID_HANDLE_VALUE;
    }
  }

  bool isValid() const noexcept { return m_handle != INVALID_HANDLE_VALUE; }
  operator HANDLE() const noexcept { return m_handle; }

private:
  HANDLE m_handle;
};

#endif // _DEVICE_HANDLE_H__