#include "IOCPHandle.h"
#include "../utils/Logger.h"

IOCPHandle::IOCPHandle() : completion_port_(NULL) {}

IOCPHandle::~IOCPHandle() { CloseCompletionPort(); }

IOCPHandle::IOCPHandle(IOCPHandle &&other) noexcept
    : completion_port_(other.completion_port_) {
  other.completion_port_ = NULL;
}

IOCPHandle &IOCPHandle::operator=(IOCPHandle &&other) noexcept {
  if (this != &other) {
    CloseCompletionPort();
    completion_port_ = other.completion_port_;
    other.completion_port_ = NULL;
  }
  return *this;
}

void IOCPHandle::CloseCompletionPort() {
  if (completion_port_ != NULL) {
    CloseHandle(completion_port_);
    completion_port_ = NULL;
  }
}

bool IOCPHandle::CreateCompletionPort(DWORD number_of_concurrent_threads) {
  // 关闭旧的完成端口（如果存在）
  CloseCompletionPort();

  // 创建新的完成端口
  completion_port_ = CreateIoCompletionPort(
      INVALID_HANDLE_VALUE, // 不关联文件句柄
      NULL,                 // 创建新的完成端口
      0,                    // 完成键，不需要
      number_of_concurrent_threads); // 并发线程数，0表示默认值（处理器核心数）

  if (completion_port_ == NULL) {
    LOG_ERROR("创建完成端口失败！错误码: %u", GetLastError());
    return false;
  }

  return true;
}

bool IOCPHandle::AssociateHandleWithCompletionPort(HANDLE file_handle,
                                                   ULONG_PTR completion_key) {
  HANDLE result = CreateIoCompletionPort(file_handle, // 要关联的句柄
                                         completion_port_, // 已存在的完成端口
                                         completion_key, // 完成键
                                         0); // 忽略，使用已有完成端口的设置

  if (result == NULL) {
    LOG_ERROR("关联句柄到完成端口失败！错误码: %u", GetLastError());
    return false;
  }

  return true;
}