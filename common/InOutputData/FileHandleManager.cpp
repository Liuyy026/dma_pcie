#include "FileHandleManager.h"
#include "../utils/Logger.h"
#include <utility>
#include <cstdint>

FileHandleManager &FileHandleManager::GetInstance() {
  static FileHandleManager instance;
  return instance;
}

FileHandleManager::FileHandleManager() : next_file_id_(1) {}

FileHandleManager::~FileHandleManager() { CloseAllHandles(); }

std::uint64_t FileHandleManager::GetNextFileId() { return next_file_id_++; }

std::uint64_t
FileHandleManager::GetTotalRequestCount(std::uint64_t file_size,
                                        DWORD block_size) {
  // 计算总的请求数，向上取整
  return static_cast<std::uint64_t>((file_size + block_size - 1) /
                                       block_size);
}

std::uint64_t FileHandleManager::OpenFile(const std::string &file_path,
                                             DWORD block_size) {
  std::lock_guard<std::mutex> lock(mutex_);

  // 创建新的文件句柄
  auto new_handle = std::make_shared<FileHandle>();
  if (!new_handle->OpenFile(file_path)) {
    LOG_ERROR("无法打开文件: %s", file_path.c_str());
    return 0; // 返回0表示失败
  }

  // 计算总请求数
  std::uint64_t file_size = new_handle->GetFileLength();
  std::uint64_t total_requests = GetTotalRequestCount(file_size, block_size);

  // 获取新的文件ID
  std::uint64_t file_id = GetNextFileId();

  // 使用构造函数直接创建并插入FileHandleInfo
  file_handles_.emplace(file_id,
                        FileHandleInfo(new_handle, file_path, total_requests));

  return file_id;
}

std::shared_ptr<FileHandle>
FileHandleManager::GetFileHandle(std::uint64_t file_id) {
  std::lock_guard<std::mutex> lock(mutex_);

  auto it = file_handles_.find(file_id);
  if (it != file_handles_.end()) {
    return it->second.handle;
  }

  LOG_ERROR("未找到文件ID: %llu 对应的句柄",
            static_cast<unsigned long long>(file_id));
  return nullptr;
}

std::string FileHandleManager::GetFilePath(std::uint64_t file_id) {
  std::lock_guard<std::mutex> lock(mutex_);

  auto it = file_handles_.find(file_id);
  if (it != file_handles_.end()) {
    return it->second.file_path;
  }

  return "";
}

bool FileHandleManager::UpdateCompletedRequests(
    std::uint64_t file_id, std::uint64_t completed_count) {
  std::lock_guard<std::mutex> lock(mutex_);

  auto it = file_handles_.find(file_id);
  if (it != file_handles_.end()) {
    // 更新已完成请求数
    it->second.completed_requests += completed_count;

    // 检查是否所有请求都已完成
    if (it->second.completed_requests >= it->second.total_requests) {
      file_handles_.erase(it);
      return true; // 返回true表示文件已被自动关闭
    }

    return false; // 文件尚未完成所有请求
  }

  LOG_ERROR("尝试更新未找到的文件ID: %llu",
            static_cast<unsigned long long>(file_id));
  return false;
}

bool FileHandleManager::CloseFileHandle(std::uint64_t file_id) {
  std::lock_guard<std::mutex> lock(mutex_);

  auto it = file_handles_.find(file_id);
  if (it != file_handles_.end()) {
    it->second.handle->Close();
    file_handles_.erase(it);
    return true;
  }

  LOG_ERROR("尝试关闭未找到的文件ID: %llu",
            static_cast<unsigned long long>(file_id));
  return false;
}

void FileHandleManager::CloseAllHandles() {
  std::lock_guard<std::mutex> lock(mutex_);

  LOG_INFO("关闭所有文件句柄，当前数量: %zu", file_handles_.size());
  file_handles_.clear();
}

size_t FileHandleManager::GetHandleCount() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return file_handles_.size();
}
