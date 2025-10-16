#include "FileHandle.h"
#include "../utils/Logger.h"
#include <cstdint>

FileHandle::FileHandle()
    : file_handle_(INVALID_HANDLE_VALUE), file_length_(0) {}

FileHandle::~FileHandle() { Close(); }

FileHandle::FileHandle(FileHandle &&other) noexcept
    : file_handle_(other.file_handle_), file_length_(other.file_length_),
      file_path_(std::move(other.file_path_)) {
  other.file_handle_ = INVALID_HANDLE_VALUE;
  other.file_length_ = 0;
}

FileHandle &FileHandle::operator=(FileHandle &&other) noexcept {
  if (this != &other) {
    Close();
    file_handle_ = other.file_handle_;
    file_length_ = other.file_length_;
    file_path_ = std::move(other.file_path_);
    other.file_handle_ = INVALID_HANDLE_VALUE;
    other.file_length_ = 0;
  }
  return *this;
}

bool FileHandle::OpenFile(const std::string &file_path, DWORD access_flags,
                          DWORD share_mode, DWORD creation_disposition,
                          DWORD flags_and_attributes) {
  // 关闭之前打开的文件
  Close();

  // 保存文件路径
  file_path_ = file_path;

  // 创建文件
  file_handle_ = CreateFile(file_path.c_str(), access_flags, share_mode, NULL,
                            creation_disposition, flags_and_attributes, NULL);

  if (file_handle_ == INVALID_HANDLE_VALUE) {
    DWORD error_code = GetLastError();
    LOG_ERROR("%s 文件打开失败！错误码: %u", file_path.c_str(), error_code);
    return false;
  }

  // 计算文件大小
  file_length_ = CalculateFileLength();
  if (file_length_ < 1) {
    Close();
    LOG_ERROR("%s 文件大小为0！", file_path.c_str());
    return false;
  }

  return true;
}

void FileHandle::Close() {
  if (file_handle_ != INVALID_HANDLE_VALUE) {
    CloseHandle(file_handle_);
    file_handle_ = INVALID_HANDLE_VALUE;
    file_length_ = 0;
  }
}

std::uint64_t FileHandle::CalculateFileLength() {
  ULARGE_INTEGER file_size;
  file_size.LowPart = GetFileSize(file_handle_, &file_size.HighPart);

  if (file_size.LowPart == INVALID_FILE_SIZE && GetLastError() != NO_ERROR) {
    LOG_ERROR("计算文件大小失败！错误码: %u", GetLastError());
    return 0;
  }

  return static_cast<std::uint64_t>(file_size.QuadPart);
}
