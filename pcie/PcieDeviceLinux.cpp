#include "PcieDeviceLinux.h"
#include "Logger.h"
#include <chrono>
#include <fcntl.h>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>

namespace {
std::string BuildNodePath(const std::string &base, const char *suffix) {
  if (base.find('_') != std::string::npos) {
    return base + suffix;
  }
  return base + suffix;
}
} // namespace

PcieDeviceLinux::PcieDeviceLinux()
    : send_fd_(-1), recv_fd_(-1), user_fd_(-1),
      work_mode_(PcieDeviceWorkMode::SendMode) {}

PcieDeviceLinux::~PcieDeviceLinux() { Close(); }

bool PcieDeviceLinux::Open(const std::string &devicePath,
                           PcieDeviceWorkMode workMode, bool) {
  work_mode_ = workMode;

  std::string base = devicePath;
  if (base.find("/dev/") != 0) {
    base = "/dev/" + base;
  }

  auto open_node = [](const std::string &path, int flags) -> int {
    int fd = ::open(path.c_str(), flags);
    if (fd < 0) {
      LOG_ERROR("无法打开设备节点: %s", path.c_str());
    }
    return fd;
  };

  if (work_mode_ == PcieDeviceWorkMode::SendMode ||
      work_mode_ == PcieDeviceWorkMode::SendAndReceiveMode) {
    std::string h2c_path = BuildNodePath(base, "_h2c_0");
    send_fd_ = open_node(h2c_path, O_WRONLY);
    if (send_fd_ < 0) {
      return false;
    }
  }

  if (work_mode_ == PcieDeviceWorkMode::ReceiveMode ||
      work_mode_ == PcieDeviceWorkMode::SendAndReceiveMode) {
    std::string c2h_path = BuildNodePath(base, "_c2h_0");
    recv_fd_ = open_node(c2h_path, O_RDONLY);
    if (recv_fd_ < 0) {
      Close();
      return false;
    }
  }

  std::string user_path = BuildNodePath(base, "_user");
  user_fd_ = open_node(user_path, O_RDWR);
  if (user_fd_ < 0) {
    Close();
    return false;
  }

  return true;
}

bool PcieDeviceLinux::Close() {
  if (send_fd_ >= 0) {
    ::close(send_fd_);
    send_fd_ = -1;
  }
  if (recv_fd_ >= 0) {
    ::close(recv_fd_);
    recv_fd_ = -1;
  }
  if (user_fd_ >= 0) {
    ::close(user_fd_);
    user_fd_ = -1;
  }
  return true;
}

bool PcieDeviceLinux::Reset() {
  if (!IsOpen()) {
    LOG_ERROR("设备未打开，无法复位");
    return false;
  }

  std::uint32_t value = 1;
  if (!WriteBAR(0x00, sizeof(value), &value)) {
    LOG_ERROR("写入复位寄存器失败");
    return false;
  }

  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  value = 0;
  if (!WriteBAR(0x00, sizeof(value), &value)) {
    LOG_ERROR("清除复位寄存器失败");
    return false;
  }

  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  return true;
}

bool PcieDeviceLinux::IsOpen() const noexcept {
  if (work_mode_ == PcieDeviceWorkMode::SendMode) {
    return send_fd_ >= 0 && user_fd_ >= 0;
  }
  if (work_mode_ == PcieDeviceWorkMode::ReceiveMode) {
    return recv_fd_ >= 0 && user_fd_ >= 0;
  }
  return send_fd_ >= 0 && recv_fd_ >= 0 && user_fd_ >= 0;
}

bool PcieDeviceLinux::ReadBAR(long address, unsigned int size, void *buffer) {
  if (user_fd_ < 0) {
    LOG_ERROR("用户BAR未打开");
    return false;
  }

  ssize_t bytes =
      ::pread(user_fd_, buffer, static_cast<size_t>(size), static_cast<off_t>(address));
  if (bytes < 0 || static_cast<unsigned int>(bytes) != size) {
    LOG_ERROR("读取用户BAR失败");
    return false;
  }
  return true;
}

bool PcieDeviceLinux::WriteBAR(long address, unsigned int size,
                               const void *buffer) {
  if (user_fd_ < 0) {
    LOG_ERROR("用户BAR未打开");
    return false;
  }

  ssize_t bytes =
      ::pwrite(user_fd_, buffer, static_cast<size_t>(size), static_cast<off_t>(address));
  if (bytes < 0 || static_cast<unsigned int>(bytes) != size) {
    LOG_ERROR("写入用户BAR失败");
    return false;
  }
  return true;
}

void PcieDeviceLinux::HardwareSyncEnable(bool enable) {
  if (!IsOpen()) {
    LOG_ERROR("设备未打开，无法设置硬件同步");
    return;
  }

  std::uint32_t value = 0;
  if (!ReadBAR(0x28, sizeof(value), &value)) {
    LOG_ERROR("读取硬件同步寄存器失败");
    return;
  }

  if (enable) {
    value |= 0x00000001;
  } else {
    value &= 0xFFFFFFFE;
  }

  WriteBAR(0x28, sizeof(value), &value);
}

void PcieDeviceLinux::HardwareSyncOpen(bool open) {
  if (!IsOpen()) {
    LOG_ERROR("设备未打开，无法设置硬件同步模式");
    return;
  }

  std::uint32_t value = open ? 0x00000001 : 0x00000000;
  WriteBAR(0x08, sizeof(value), &value);
}

void PcieDeviceLinux::SetSpeed(int index) {
  if (!IsOpen()) {
    LOG_ERROR("设备未打开，无法设置速度");
    return;
  }

  std::uint32_t value = 0;
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
    LOG_WARN("无效的速度索引: %d", index);
    return;
  }

  WriteBAR(0x24, sizeof(value), &value);
}
