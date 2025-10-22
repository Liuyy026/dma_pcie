#include "PreadFileDataProvider.h"
#include "../utils/Logger.h"
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cstring>
#include <chrono>

PreadFileDataProvider::PreadFileDataProvider()
    : block_size_(0), bytes_read_(0) {}

PreadFileDataProvider::~PreadFileDataProvider() { Shutdown(); }

bool PreadFileDataProvider::Initialize(const std::vector<std::string>& file_list,
                                       std::size_t block_size) {
  if (file_list.empty() || block_size == 0) {
    LOG_ERROR("PreadFileDataProvider: 文件列表为空或block_size=0");
    return false;
  }

  file_list_ = file_list;
  block_size_ = block_size;
  
  // 创建缓冲池：准备足够的块（预设16个并发块，4KB对齐）
  buffer_pool_ = std::make_shared<AlignedBufferPool>();
  if (!buffer_pool_->Initialize(16, block_size_, 4096)) {
    LOG_ERROR("PreadFileDataProvider: 缓冲池初始化失败");
    return false;
  }

  bytes_read_.store(0, std::memory_order_relaxed);
  stop_flag_.store(false, std::memory_order_relaxed);
  
  // 启动读取线程
  try {
    reader_thread_ = std::make_unique<std::thread>(
        &PreadFileDataProvider::ReaderThreadFunc, this);
    running_.store(true, std::memory_order_relaxed);
    LOG_INFO("PreadFileDataProvider 初始化成功");
    return true;
  } catch (const std::exception& ex) {
    LOG_ERROR("PreadFileDataProvider: 启动读取线程失败: %s", ex.what());
    return false;
  }
}

void PreadFileDataProvider::Shutdown() {
  Stop();
  if (reader_thread_ && reader_thread_->joinable()) {
    reader_thread_->join();
  }
  if (buffer_pool_) {
    buffer_pool_->Shutdown();
    buffer_pool_.reset();
  }
  running_.store(false, std::memory_order_relaxed);
  LOG_INFO("PreadFileDataProvider 已关闭");
}

bool PreadFileDataProvider::GetNextBlock(FileDataRef& out_ref, bool blocking) {
  std::unique_lock<std::mutex> lock(queue_mutex_);
  
  if (!blocking) {
    if (block_queue_.empty()) {
      return false;
    }
  } else {
    // 阻塞等待，直到有数据或停止
    while (block_queue_.empty() && !stop_flag_.load(std::memory_order_relaxed)) {
      queue_cv_.wait_for(lock, std::chrono::milliseconds(100));
    }
    if (block_queue_.empty()) {
      return false;
    }
  }
  
  out_ref = block_queue_.front();
  block_queue_.pop_front();
  return true;
}

void PreadFileDataProvider::ReleaseBlock(const FileDataRef& ref) {
  if (ref.actual_ptr && buffer_pool_) {
    buffer_pool_->Release(ref.actual_ptr);
  }
}

std::uint64_t PreadFileDataProvider::GetBytesRead() const {
  return bytes_read_.load(std::memory_order_relaxed);
}

void PreadFileDataProvider::Stop() {
  stop_flag_.store(true, std::memory_order_relaxed);
  queue_cv_.notify_all();
}

bool PreadFileDataProvider::OpenFileODirect(const std::string& path,
                                            FileContext& ctx) {
  ctx.fd = ::open(path.c_str(), O_RDONLY | O_DIRECT);
  ctx.use_odirect = true;
  
  if (ctx.fd < 0) {
    // O_DIRECT 不支持，回退到普通模式
    ctx.fd = ::open(path.c_str(), O_RDONLY);
    ctx.use_odirect = false;
  }
  
  if (ctx.fd < 0) {
    LOG_ERROR("打开文件失败: %s, 错误: %s", path.c_str(), std::strerror(errno));
    return false;
  }
  
  struct stat st;
  if (::fstat(ctx.fd, &st) != 0) {
    LOG_ERROR("获取文件大小失败: %s", path.c_str());
    ::close(ctx.fd);
    ctx.fd = -1;
    return false;
  }
  
  if (st.st_size <= 0) {
    LOG_WARN("文件大小为0: %s", path.c_str());
    ::close(ctx.fd);
    ctx.fd = -1;
    return false;
  }
  
  ctx.path = path;
  ctx.file_size = static_cast<std::uint64_t>(st.st_size);
  
#if defined(POSIX_FADV_SEQUENTIAL)
  if (!ctx.use_odirect) {
    ::posix_fadvise(ctx.fd, 0, 0, POSIX_FADV_SEQUENTIAL);
  }
#endif
  
  return true;
}

bool PreadFileDataProvider::ReadBlockFromFile(const FileContext& ctx,
                                              std::uint64_t offset,
                                              unsigned char*& out_ptr,
                                              std::size_t& out_len) {
  // 申请缓冲
  AlignedBufferPool::Block pool_block;
  if (!buffer_pool_->Acquire(pool_block)) {
    LOG_WARN("缓冲池获取超时");
    out_ptr = nullptr;
    out_len = 0;
    return false;
  }
  
  std::size_t read_len = std::min<std::uint64_t>(block_size_,
                                                  ctx.file_size - offset);
  if (read_len == 0) {
    buffer_pool_->Release(pool_block.ptr);
    out_ptr = nullptr;
    out_len = 0;
    return false;
  }
  
  ssize_t bytes = ::pread(ctx.fd, pool_block.ptr, read_len,
                          static_cast<off_t>(offset));
  
  if (bytes < 0) {
    LOG_ERROR("读文件失败: %s at offset %llu",
              ctx.path.c_str(), (unsigned long long)offset);
    buffer_pool_->Release(pool_block.ptr);
    out_ptr = nullptr;
    out_len = 0;
    return false;
  }
  
  if (bytes == 0) {
    buffer_pool_->Release(pool_block.ptr);
    out_ptr = nullptr;
    out_len = 0;
    return false;
  }
  
  out_ptr = pool_block.ptr;
  out_len = static_cast<std::size_t>(bytes);
  return true;
}

void PreadFileDataProvider::ReaderThreadFunc() {
  std::uint64_t global_block_id = 0;
  
  do {
    for (const auto& file_path : file_list_) {
      if (stop_flag_.load(std::memory_order_relaxed)) {
        break;
      }
      
      FileContext ctx;
      if (!OpenFileODirect(file_path, ctx)) {
        continue;
      }
      
      std::uint64_t file_offset = 0;
      while (file_offset < ctx.file_size &&
             !stop_flag_.load(std::memory_order_relaxed)) {
        
        unsigned char* ptr = nullptr;
        std::size_t len = 0;
        
        if (!ReadBlockFromFile(ctx, file_offset, ptr, len)) {
          break;
        }
        
        FileDataRef ref;
        ref.file_path = ctx.path;
        ref.file_offset = file_offset;
        ref.length = len;
        ref.block_id = global_block_id++;
        ref.actual_ptr = ptr;
        ref.actual_capacity = block_size_;
        
        // 投入队列
        {
          std::lock_guard<std::mutex> lock(queue_mutex_);
          block_queue_.push_back(ref);
        }
        queue_cv_.notify_one();
        
        // 更新统计
        bytes_read_.fetch_add(len, std::memory_order_relaxed);
        file_offset += len;
      }
      
      if (ctx.fd >= 0) {
        ::close(ctx.fd);
      }
    }
  } while (!stop_flag_.load(std::memory_order_relaxed));
  
  // 标记完成
  queue_cv_.notify_all();
}
