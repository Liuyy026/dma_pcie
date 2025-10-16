#ifndef _FILE_HANDLE_MANAGER_H_
#define _FILE_HANDLE_MANAGER_H_

#include "FileHandle.h"
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <cstdint>

// 文件句柄管理器，管理多个文件句柄，根据完成请求数自动销毁
class FileHandleManager {
public:
  // 获取单例实例
  static FileHandleManager &GetInstance();

  // 禁用拷贝和移动
  FileHandleManager(const FileHandleManager &) = delete;
  FileHandleManager &operator=(const FileHandleManager &) = delete;
  FileHandleManager(FileHandleManager &&) = delete;
  FileHandleManager &operator=(FileHandleManager &&) = delete;

  // 打开文件并返回文件句柄ID
  std::uint64_t OpenFile(const std::string &file_path, DWORD block_size);

  // 获取文件句柄
  std::shared_ptr<FileHandle> GetFileHandle(std::uint64_t file_id);

  // 获取文件路径
  std::string GetFilePath(std::uint64_t file_id);

  // 更新文件的已完成请求数，当所有请求都完成时自动销毁句柄
  bool UpdateCompletedRequests(std::uint64_t file_id,
                               std::uint64_t completed_count = 1);

  // 关闭指定的文件句柄
  bool CloseFileHandle(std::uint64_t file_id);

  // 关闭所有文件句柄
  void CloseAllHandles();

  // 获取当前管理的文件句柄数量
  size_t GetHandleCount() const;

  // 获取自增ID
  std::uint64_t GetNextFileId();

  // 计算文件总请求数
  std::uint64_t GetTotalRequestCount(std::uint64_t file_size,
                                        DWORD block_size);

private:
  // 私有构造和析构
  FileHandleManager();
  ~FileHandleManager();

private:
  // 文件句柄信息结构体
  struct FileHandleInfo {
    std::shared_ptr<FileHandle> handle;  // 文件句柄
    std::string file_path;               // 文件路径
    std::uint64_t total_requests;     // 总请求数
    std::uint64_t completed_requests; // 已完成请求数

    // 默认构造函数
    FileHandleInfo() : total_requests(0), completed_requests(0) {}

    // 完整构造函数
    FileHandleInfo(std::shared_ptr<FileHandle> h, std::string path,
                   std::uint64_t total)
        : handle(std::move(h)), file_path(std::move(path)),
          total_requests(total), completed_requests(0) {}
  };

  std::unordered_map<std::uint64_t, FileHandleInfo>
      file_handles_;         // 文件ID到句柄的映射
  mutable std::mutex mutex_; // 保护文件句柄映射的互斥锁
  std::atomic<std::uint64_t> next_file_id_; // 下一个文件ID
};

#endif // _FILE_HANDLE_MANAGER_H_