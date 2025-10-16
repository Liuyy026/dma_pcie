#ifndef _FILE_HANDLE_H_
#define _FILE_HANDLE_H_

#include <Windows.h>
#include <string>
#include <cstdint>

// 文件句柄的RAII封装
class FileHandle {
public:
  // 构造与析构
  FileHandle();
  ~FileHandle();

  // 禁用拷贝
  FileHandle(const FileHandle &) = delete;
  FileHandle &operator=(const FileHandle &) = delete;

  // 允许移动
  FileHandle(FileHandle &&other) noexcept;
  FileHandle &operator=(FileHandle &&other) noexcept;

  // 打开文件
  bool OpenFile(const std::string &file_path, DWORD access_flags = GENERIC_READ,
                DWORD share_mode = FILE_SHARE_READ,
                DWORD creation_disposition = OPEN_EXISTING,
                DWORD flags_and_attributes = FILE_FLAG_NO_BUFFERING |
                                             FILE_FLAG_OVERLAPPED |
                                             FILE_FLAG_SEQUENTIAL_SCAN);

  // 关闭文件句柄
  void Close();

  // 获取文件句柄
  HANDLE GetHandle() const { return file_handle_; }

  // 获取文件长度
  std::uint64_t GetFileLength() const { return file_length_; }

  // 获取文件路径
  const std::string &GetFilePath() const { return file_path_; }

private:
  // 计算文件长度
  std::uint64_t CalculateFileLength();

private:
  HANDLE file_handle_;           // 文件句柄
  std::uint64_t file_length_; // 文件长度
  std::string file_path_;        // 文件路径
};

#endif // _FILE_HANDLE_H_