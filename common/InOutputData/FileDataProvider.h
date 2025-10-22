#ifndef FILE_DATA_PROVIDER_H
#define FILE_DATA_PROVIDER_H

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

/**
 * @brief 文件数据引用，描述一个数据块的来源
 * 
 * 过渡设计：目前记录文件路径+偏移+长度，
 * 为未来mmap/IOVA过渡做准备
 */
struct FileDataRef {
  std::string file_path;
  std::uint64_t file_offset = 0;
  std::size_t length = 0;
  std::uint64_t block_id = 0;  // 全局block编号，用于顺序追踪
  
  // 过渡字段：当前仍指向实际缓冲指针，mmap后可删除
  unsigned char* actual_ptr = nullptr;
  std::size_t actual_capacity = 0;
};

/**
 * @brief 文件数据源抽象接口
 * 
 * 定义统一的文件数据提供能力，支持：
 * 1. pread 同步读取（当前）
 * 2. mmap 映射（未来）
 * 3. O_DIRECT + 对齐内存（当前）
 */
class IFileDataProvider {
public:
  virtual ~IFileDataProvider() = default;
  
  /**
   * @brief 初始化数据源
   * @param file_list 要读取的文件路表
   * @param block_size 单个块的大小
   * @return 初始化成功
   */
  virtual bool Initialize(const std::vector<std::string>& file_list,
                          std::size_t block_size) = 0;
  
  /**
   * @brief 清理资源
   */
  virtual void Shutdown() = 0;
  
  /**
   * @brief 获取下一个数据块
   * 
   * 返回 FileDataRef，描述数据来源（当前: 指针+长度, 未来: 文件偏移+IOVA映射）
   * 若返回 false 表示无更多数据或停止
   */
  virtual bool GetNextBlock(FileDataRef& out_ref, bool blocking = true) = 0;
  
  /**
   * @brief 释放一个数据块（仅当需要时）
   * 
   * 当前mmap前可能需要释放缓冲，mmap后无需释放
   */
  virtual void ReleaseBlock(const FileDataRef& ref) = 0;
  
  /**
   * @brief 查询已读字节数
   */
  virtual std::uint64_t GetBytesRead() const = 0;
  
  /**
   * @brief 停止读取（用于优雅关闭）
   */
  virtual void Stop() = 0;
};

#endif // FILE_DATA_PROVIDER_H
