#ifndef _IOCP_HANDLE_H_
#define _IOCP_HANDLE_H_

#include <Windows.h>

// IOCP句柄的RAII封装，专注于完成端口资源管理
class IOCPHandle {
public:
  // 构造与析构
  IOCPHandle();
  ~IOCPHandle();

  // 禁用拷贝
  IOCPHandle(const IOCPHandle &) = delete;
  IOCPHandle &operator=(const IOCPHandle &) = delete;

  // 允许移动
  IOCPHandle(IOCPHandle &&other) noexcept;
  IOCPHandle &operator=(IOCPHandle &&other) noexcept;

  // 创建完成端口
  bool CreateCompletionPort(DWORD number_of_concurrent_threads = 1);

  // 将句柄关联到完成端口
  bool AssociateHandleWithCompletionPort(HANDLE file_handle,
                                         ULONG_PTR completion_key);

  // 关闭完成端口
  void CloseCompletionPort();

  // 获取完成端口
  HANDLE GetCompletionPort() const { return completion_port_; }

private:
  HANDLE completion_port_; // 完成端口句柄
};

#endif // _IOCP_HANDLE_H_