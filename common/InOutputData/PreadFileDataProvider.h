#ifndef PREAD_FILE_DATA_PROVIDER_H
#define PREAD_FILE_DATA_PROVIDER_H

#include "FileDataProvider.h"
#include "../utils/AlignedBufferPool.h"
#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>

/**
 * @brief 基于pread的文件数据提供者
 * 
 * 当前实现，读取逻辑与发送完全解耦
 * 为后续mmap迁移做准备（只需替换此类实现即可）
 */
class PreadFileDataProvider : public IFileDataProvider {
public:
  PreadFileDataProvider();
  ~PreadFileDataProvider();
  
  bool Initialize(const std::vector<std::string>& file_list,
                  std::size_t block_size) override;
  void Shutdown() override;
  bool GetNextBlock(FileDataRef& out_ref, bool blocking = true) override;
  void ReleaseBlock(const FileDataRef& ref) override;
  std::uint64_t GetBytesRead() const override;
  void Stop() override;

private:
  struct FileContext {
    int fd = -1;
    std::string path;
    std::uint64_t file_size = 0;
    bool use_odirect = false;
  };
  
  // 配置参数
  std::vector<std::string> file_list_;
  std::size_t block_size_ = 0;
  
  // 缓冲池管理
  std::shared_ptr<AlignedBufferPool> buffer_pool_;
  
  // 数据块队列：存储已读取但未发送的块
  std::deque<FileDataRef> block_queue_;
  std::mutex queue_mutex_;
  std::condition_variable queue_cv_;
  
  // 线程与状态
  std::unique_ptr<std::thread> reader_thread_;
  std::atomic<bool> stop_flag_{false};
  std::atomic<bool> running_{false};
  std::atomic<std::uint64_t> bytes_read_{0};
  
  // 内部方法
  void ReaderThreadFunc();
  bool OpenFileODirect(const std::string& path, FileContext& ctx);
  bool ReadBlockFromFile(const FileContext& ctx, std::uint64_t offset,
                         unsigned char*& out_ptr, std::size_t& out_len);
};

#endif // PREAD_FILE_DATA_PROVIDER_H
