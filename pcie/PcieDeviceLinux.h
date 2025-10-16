#pragma once

#include "PcieTypes.h"
#include <cstdint>
#include <string>

class PcieDeviceLinux {
public:
  PcieDeviceLinux();
  ~PcieDeviceLinux();

  PcieDeviceLinux(const PcieDeviceLinux &) = delete;
  PcieDeviceLinux &operator=(const PcieDeviceLinux &) = delete;

  bool Open(const std::string &devicePath, PcieDeviceWorkMode workMode,
            bool async = false);
  bool Close();
  bool Reset();
  bool IsOpen() const noexcept;

  bool ReadBAR(long address, unsigned int size, void *buffer);
  bool WriteBAR(long address, unsigned int size, const void *buffer);

  void HardwareSyncEnable(bool enable);
  void HardwareSyncOpen(bool open);
  void SetSpeed(int index);

  int GetSendHandle() const { return send_fd_; }
  int GetUserBarHandle() const { return user_fd_; }

private:
  int send_fd_;
  int recv_fd_;
  int user_fd_;
  PcieDeviceWorkMode work_mode_;
};
