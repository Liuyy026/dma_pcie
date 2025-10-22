#ifndef _InputDataFromFileUseIOCP_
#define _InputDataFromFileUseIOCP_

#include "IOCPHandle.h"
#include "OrderedDataProcessor.h"
#include "OverlappedQueue.h"
#include <Windows.h>
#include <atomic>
#include <memory>
#include <string>
#include <thread>
#include <vector>
#include <cstdint>

typedef struct _InputDataFromFileUseIOCPInitParam_ {
  std::vector<std::string> file_list;
  unsigned int block_size;
  unsigned int io_request_num;
  unsigned int pending_slack;
  bool loop;
  int cpu_id;

  _InputDataFromFileUseIOCPInitParam_() {
    block_size = 512 * 1024;
    io_request_num = 16;
    pending_slack = 0;
    loop = false;
    cpu_id = -1;
  }
} InputDataFromFileUseIOCPInitParam;

// OVERLAPPED资源的智能指针别名
using OverlappedPtr = std::unique_ptr<NEW_OVERLAPPED[]>;

class CInputDataFromFileUseIOCP {
public:
  CInputDataFromFileUseIOCP();
  ~CInputDataFromFileUseIOCP();

protected:
  InputDataFromFileUseIOCPInitParam m_InitParam;

  // 添加有序数据处理器
  COrderedDataProcessor m_OrderedDataProcessor;

  void ReadData();

  // 打开文件并关联完成端口
  std::uint64_t OpenFile(const std::string &file_path);

  // 提交新的读请求
  bool m_bFirstRequest;
  bool SubmitNewReadRequest(NEW_OVERLAPPED *overlapped,
                            std::uint64_t &remaining_bytes,
                            std::uint64_t request_counter,
                            std::uint64_t file_id);

  // 使用资源管理器
  IOCPHandle iocp_handle_;             // 完成端口管理
  std::uint64_t current_file_id_;   // 当前文件ID
  HANDLE current_file_handle_;         // 当前文件句柄
  std::uint64_t current_file_size_; // 当前文件大小

  std::unique_ptr<std::thread> read_thread_; // 使用std::thread替代HANDLE
  std::atomic<bool> stop_read_data_;         // 原子变量控制线程停止
  std::atomic<bool> thread_running_; // 原子变量标记线程是否在运行

  // 保留计数器和监听器
  std::uint64_t m_uiInputDataNum;
  std::uint64_t m_uiOutputDataNum;

  unsigned int step;

public:
  // 添加设置监听器的方法
  void SetInputDataListener(IInputDataListener *pInputDataListener) {
    m_OrderedDataProcessor.SetInputDataListener(pInputDataListener);
  }

  bool Init(void *pInitParam);
  void Destroy();
  void Reset();

  bool Start();
  bool Stop();

  bool IsRunning();

  // 添加数据计数获取方法
  std::uint64_t GetInputDataNum(void *pParam = nullptr) {
    return m_uiInputDataNum;
  }
  std::uint64_t GetOutputDataNum(void *pParam = nullptr) {
    return m_uiOutputDataNum;
  }

  unsigned int GetStep() { return step; }
};

#endif
