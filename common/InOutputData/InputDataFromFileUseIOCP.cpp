#include "InputDataFromFileUseIOCP.h"
#include "../utils/Logger.h"
#include "../utils/ThreadAffinity.h"
#include "FileHandleManager.h"
#include "InputBlock.h"
#include "VirtualMemoryBuffer.h"
#include <minwindef.h>
#include <cstdint>

CInputDataFromFileUseIOCP::CInputDataFromFileUseIOCP()
    : stop_read_data_(false), thread_running_(false), m_uiInputDataNum(0),
      m_uiOutputDataNum(0), current_file_id_(0),
      current_file_handle_(INVALID_HANDLE_VALUE), step(0),
      m_bFirstRequest(true) {}

CInputDataFromFileUseIOCP::~CInputDataFromFileUseIOCP() { Destroy(); }

bool CInputDataFromFileUseIOCP::Init(void *pInitParam) {
  if (!pInitParam) {
    LOG_ERROR("初始化参数为空");
    return false;
  }

  // 复制初始化参数
  m_InitParam = *(static_cast<InputDataFromFileUseIOCPInitParam *>(pInitParam));

  if (m_InitParam.file_list.empty()) {
    LOG_ERROR("文件列表为空");
    return false;
  }

  // 初始化有序数据处理器
  if (!m_OrderedDataProcessor.Initialize(m_InitParam.io_request_num,
                                         m_InitParam.block_size,
                                         m_InitParam.cpu_id + 1)) {
    LOG_ERROR("初始化有序数据处理器失败");
    return false;
  }

  // 创建完成端口
  if (!iocp_handle_.CreateCompletionPort(1)) {
    LOG_ERROR("创建完成端口失败");
    return false;
  }

  return true;
}

void CInputDataFromFileUseIOCP::Destroy() {
  Stop();
  Reset();

  // 销毁有序数据处理器
  m_OrderedDataProcessor.Destroy();

  // 关闭完成端口
  iocp_handle_.CloseCompletionPort();
}

void CInputDataFromFileUseIOCP::Reset() {
  // 重置所有计数器和状态
  m_uiInputDataNum = 0;
  m_uiOutputDataNum = 0;
  current_file_id_ = 0;
  current_file_handle_ = INVALID_HANDLE_VALUE;
  current_file_size_ = 0;
  step = 0;

  // 重置有序数据处理器
  m_OrderedDataProcessor.Reset();
}

bool CInputDataFromFileUseIOCP::Start() {
  // 如果已经在运行，直接返回
  if (IsRunning()) {
    LOG_ERROR("读取线程已经在运行");
    return true;
  }

  // 重置停止标志
  stop_read_data_.store(false);
  m_bFirstRequest = true;

  // 打开第一个文件
  current_file_id_ = OpenFile(m_InitParam.file_list[0]);
  if (current_file_id_ == 0) {
    LOG_ERROR("打开文件失败: %s", m_InitParam.file_list[0].c_str());
    return false;
  }

  // 启动有序数据处理
  if (!m_OrderedDataProcessor.StartProcessing()) {
    LOG_ERROR("启动有序数据处理器失败");
    return false;
  }

  // 创建并启动读取线程
  read_thread_ =
      std::make_unique<std::thread>(&CInputDataFromFileUseIOCP::ReadData, this);

  // 设置CPU亲和性
  if (m_InitParam.cpu_id >= 0) {
    if (!ThreadAffinity::GetInstance().SetThreadAffinity(
            read_thread_->native_handle(), m_InitParam.cpu_id)) {
      LOG_ERROR("设置线程CPU亲和性失败，错误码: %d", GetLastError());
    }
  }

  return true;
}

bool CInputDataFromFileUseIOCP::Stop() {
  // 设置停止标志
  stop_read_data_.store(true);

  // 如果读取线程存在，等待它结束
  if (read_thread_ && read_thread_->joinable()) {
    read_thread_->join();
    read_thread_.reset();
  }

  // 停止有序数据处理
  m_OrderedDataProcessor.StopProcessing();

  return true;
}

bool CInputDataFromFileUseIOCP::IsRunning() { return thread_running_.load(); }

void CInputDataFromFileUseIOCP::ReadData() {
  // 设置线程运行标志
  thread_running_.store(true);

  // 分配虚拟内存缓冲区
  VirtualMemoryBuffer virtual_memory(
      static_cast<size_t>(m_InitParam.block_size) * m_InitParam.io_request_num);
  if (!virtual_memory.isValid()) {
    LOG_ERROR("内存分配失败，大小: %zu 字节",
              static_cast<size_t>(m_InitParam.block_size) *
                  m_InitParam.io_request_num);
    thread_running_.store(false);
    return;
  }

  // 分配重叠结构数组
  std::unique_ptr<NEW_OVERLAPPED[]> overlapped_array(
      new NEW_OVERLAPPED[m_InitParam.io_request_num]);
  if (!overlapped_array) {
    LOG_ERROR("重叠结构数组分配失败");
    thread_running_.store(false);
    return;
  }

  // 初始化重叠结构数组
  memset(overlapped_array.get(), 0,
         sizeof(NEW_OVERLAPPED) * m_InitParam.io_request_num);
  for (unsigned int i = 0; i < m_InitParam.io_request_num; i++) {
    overlapped_array[i].buffer = reinterpret_cast<BYTE *>(
        static_cast<char *>(virtual_memory.get()) + i * m_InitParam.block_size);
  }

  // 分配OVERLAPPED_ENTRY数组，用于批量获取完成结果
  const ULONG max_entries = m_InitParam.io_request_num;
  std::unique_ptr<OVERLAPPED_ENTRY[]> entries(
      new OVERLAPPED_ENTRY[max_entries]);

  // 当前文件索引
  size_t current_file_index = 0;

  // 当前文件剩余字节数
  std::uint64_t remaining_bytes = current_file_size_;

  // 当前活跃请求数量
  unsigned int active_requests = 0;
  // 全局请求序号，用于生成不重复的index
  std::uint64_t global_request_counter = 0;

  // 发起初始I/O请求
  for (unsigned int i = 0;
       i < m_InitParam.io_request_num && remaining_bytes > 0; i++) {
    if (!SubmitNewReadRequest(overlapped_array.get() + i, remaining_bytes,
                              global_request_counter++, current_file_id_)) {
      LOG_ERROR("初始化读取请求失败");
      thread_running_.store(false);
      return;
    }
    active_requests++;
  }

  step = 0;

  // 主循环
  while (!stop_read_data_.load()) {
    ULONG entries_removed = 0;

    step = 1;
    // 批量获取I/O完成结果
    BOOL result = GetQueuedCompletionStatusEx(iocp_handle_.GetCompletionPort(),
                                              entries.get(), max_entries,
                                              &entries_removed,
                                              100,    // 超时100毫秒
                                              FALSE); // 非提醒模式

    // 检查停止信号
    if (stop_read_data_.load())
      break;

    // 如果没有完成的I/O，继续循环
    if (!result && GetLastError() == WAIT_TIMEOUT)
      continue;

    // 如果发生错误（非超时）
    if (!result) {
      LOG_ERROR("GetQueuedCompletionStatusEx失败，错误码: %d", GetLastError());
      break;
    }

    step = 2;

    // 处理完成的I/O
    bool has_error = false;
    for (ULONG i = 0; i < entries_removed; i++) {
      OVERLAPPED_ENTRY &entry = entries[i];
      LPOVERLAPPED overlapped_ptr = entry.lpOverlapped;
      DWORD bytes_transferred = entry.dwNumberOfBytesTransferred;

      if (overlapped_ptr) {
        NEW_OVERLAPPED *cur_overlapped =
            CONTAINING_RECORD(overlapped_ptr, NEW_OVERLAPPED, overlapped);

        // 更新活跃请求数量
        active_requests--;

        // 更新文件已完成请求计数
        FileHandleManager::GetInstance().UpdateCompletedRequests(
            cur_overlapped->file_id, 1);

        // 只有在有效的数据长度下才处理数据
        if (bytes_transferred == cur_overlapped->request_data_length) {
          InputBlock block;
          block.buffer =
              reinterpret_cast<std::uint8_t *>(cur_overlapped->buffer);
          block.buffer_capacity = m_InitParam.block_size;
          block.data_length = bytes_transferred;
          block.index = cur_overlapped->index;
          block.file_id = cur_overlapped->file_id;

          m_OrderedDataProcessor.ProcessData(block);

          // 更新输入数据计数
          m_uiInputDataNum += bytes_transferred;
        } else {
          LOG_ERROR("读取数据长度不正确，期望: %d, 实际: %d",
                    cur_overlapped->request_data_length, bytes_transferred);
          has_error = true;
          break;
        }

        // 提交新的读请求以维持io_request_num个请求在途
        if (remaining_bytes == 0) {
          // 切换到下一个文件
          current_file_index++;
          if (current_file_index >= m_InitParam.file_list.size()) {
            if (!m_InitParam.loop) {
              continue;
            }
            current_file_index = 0;
          }

          current_file_id_ =
              OpenFile(m_InitParam.file_list[current_file_index]);
          if (current_file_id_ == 0) {
            LOG_ERROR("切换到下一个文件失败");
            has_error = true;
            break;
          }
          remaining_bytes = current_file_size_;
        }

        if (remaining_bytes > 0) {
          if (SubmitNewReadRequest(cur_overlapped, remaining_bytes,
                                   global_request_counter++,
                                   current_file_id_)) {
            active_requests++;
          } else {
            has_error = true;
            break;
          }
        }
      }
    }

    if (has_error)
      break;

    // 如果所有文件都处理完毕并且没有活跃请求，退出循环
    if (remaining_bytes == 0 &&
        current_file_index >= m_InitParam.file_list.size() - 1 &&
        active_requests == 0) {
      break;
    }
  }

  step = 3;

  // 关闭所有文件句柄
  FileHandleManager::GetInstance().CloseAllHandles();

  // 等待所有进行中的I/O请求完成
  if (active_requests > 0) {
    // 使用GetQueuedCompletionStatusEx等待所有请求完成
    while (active_requests > 0) {
      step = 4;
      ULONG entries_removed = 0;
      BOOL result = GetQueuedCompletionStatusEx(
          iocp_handle_.GetCompletionPort(), entries.get(), max_entries,
          &entries_removed,
          100,    // 超时100毫秒
          FALSE); // 非提醒模式

      if (result) {
        // 处理完成的请求，只更新计数，不再提交新请求
        if (entries_removed > 0) {
          active_requests -= entries_removed;
        }
      }
    }
  }

  thread_running_.store(false);
}

// 打开文件并关联完成端口
std::uint64_t
CInputDataFromFileUseIOCP::OpenFile(const std::string &file_path) {
  // 使用FileHandleManager打开文件
  std::uint64_t file_id = FileHandleManager::GetInstance().OpenFile(
      file_path, m_InitParam.block_size);

  if (file_id == 0) {
    LOG_ERROR("无法打开文件: %s", file_path.c_str());
    return 0;
  }

  // 获取文件句柄
  auto file_handle = FileHandleManager::GetInstance().GetFileHandle(file_id);
  if (!file_handle) {
    LOG_ERROR("无法获取文件句柄: %s", file_path.c_str());
    FileHandleManager::GetInstance().CloseFileHandle(file_id);
    return 0;
  }

  current_file_handle_ = file_handle->GetHandle();
  current_file_size_ = file_handle->GetFileLength();

  // 将文件句柄关联到完成端口
  if (!iocp_handle_.AssociateHandleWithCompletionPort(current_file_handle_,
                                                      0)) {
    LOG_ERROR("关联文件到完成端口失败: %s", file_path.c_str());
    FileHandleManager::GetInstance().CloseFileHandle(file_id);
    return 0;
  }

  return file_id;
}

// 提交新的读请求
bool CInputDataFromFileUseIOCP::SubmitNewReadRequest(
    NEW_OVERLAPPED *overlapped, std::uint64_t &remaining_bytes,
    std::uint64_t request_counter, std::uint64_t file_id) {
  // 设置index，确保不超过MAX_DISORDER_GROUPS * io_request_num
  ULONGLONG index = static_cast<ULONGLONG>(
      request_counter % (MAX_DISORDER_GROUPS * m_InitParam.io_request_num));

  if (!m_bFirstRequest) {
    while (true) {
      ULONGLONG next_output_slot = m_OrderedDataProcessor.GetNextOutputSlot();

      if (index == next_output_slot) {
        // LOG_ERROR("读取请求的index与输出槽位相同，index: %llu, 输出槽位: %u",
        // index, next_output_slot);
        Sleep(1);
        continue;
      }
      break;
    }
  }
  m_bFirstRequest = false;

  // 重置重叠结构
  memset(&overlapped->overlapped, 0, sizeof(OVERLAPPED));

  // 计算当前请求的文件偏移
  std::uint64_t file_offset = current_file_size_ - remaining_bytes;
  overlapped->overlapped.Offset = static_cast<DWORD>(file_offset & 0xFFFFFFFF);
  overlapped->overlapped.OffsetHigh = static_cast<DWORD>(file_offset >> 32);

  // 计算读取长度
  DWORD read_length = m_InitParam.block_size;
  if (read_length > remaining_bytes) {
    read_length = static_cast<DWORD>(remaining_bytes);
  }
  overlapped->request_data_length = read_length;

  overlapped->index = index;
  overlapped->file_id = file_id;

  // LOG_INFO("提交请求: %llu, 偏移: %llu, 长度: %d，index: %llu, 文件大小:
  // %llu",
  //          file_id, file_offset, read_length, index, current_file_size_);

  // 发起异步读取
  BOOL read_ret =
      ReadFile(current_file_handle_, overlapped->buffer, m_InitParam.block_size,
               NULL, &overlapped->overlapped);

  if (!read_ret && GetLastError() != ERROR_IO_PENDING) {
    LOG_ERROR("ReadFile读取文件失败，错误码: %d", GetLastError());
    return false;
  }

  // 更新剩余字节数
  remaining_bytes -= read_length;
  return true;
}
