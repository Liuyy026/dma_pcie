#ifndef _LOGGER_H_
#define _LOGGER_H_

#include <atomic>
#include <filesystem>
#include <mutex>
#include <spdlog/spdlog.h>
#include <string>

// 日志级别枚举
enum class LogLevel {
  Trace = 0,
  Debug = 1,
  Info = 2,
  Warn = 3,
  Error = 4,
  Critical = 5,
  Off = 6
};

class CLogger {
public:
  // 获取单例实例
  static CLogger &GetInstance();

  // 删除拷贝和移动操作
  CLogger(const CLogger &) = delete;
  CLogger &operator=(const CLogger &) = delete;
  CLogger(CLogger &&) = delete;
  CLogger &operator=(CLogger &&) = delete;

  // 初始化日志系统
  bool Initialize(const std::string &logFolderPath = "",
                  LogLevel level = LogLevel::Info);

  // 调整当前文件日志的模式后缀（如 file/memory/shm 等）
  void SetModeTag(const std::string &modeTag);

  // 清理日志系统资源
  void Cleanup();

  // 设置日志级别
  void SetLevel(LogLevel level);

  // 日志记录函数
  void Trace(const char *fmt, ...);
  void Debug(const char *fmt, ...);
  void Info(const char *fmt, ...);
  void Warn(const char *fmt, ...);
  void Error(const char *fmt, ...);
  void Critical(const char *fmt, ...);

  // 带源文件和行号的日志记录函数
  void TraceEx(const char *file, int line, const char *fmt, ...);
  void DebugEx(const char *file, int line, const char *fmt, ...);
  void InfoEx(const char *file, int line, const char *fmt, ...);
  void WarnEx(const char *file, int line, const char *fmt, ...);
  void ErrorEx(const char *file, int line, const char *fmt, ...);
  void CriticalEx(const char *file, int line, const char *fmt, ...);

private:
  CLogger();  // 私有构造函数
  ~CLogger(); // 私有析构函数

  std::shared_ptr<spdlog::logger> m_console_logger;
  std::shared_ptr<spdlog::logger> m_file_logger;
  std::mutex m_mutex;
  bool m_bInitialized;
  LogLevel m_level;
  std::filesystem::path m_logFolderPath;
  std::string m_modeTag;

  void configureFileLoggerLocked(const std::string &modeTag);
  void pruneOldLogsLocked();
  std::string sanitizeModeTag(const std::string &modeTag) const;
};

// 全局日志宏定义，方便使用
#define LOG_TRACE(...) CLogger::GetInstance().Trace(__VA_ARGS__)
#define LOG_DEBUG(...) CLogger::GetInstance().Debug(__VA_ARGS__)
#define LOG_INFO(...) CLogger::GetInstance().Info(__VA_ARGS__)
#define LOG_WARN(...) CLogger::GetInstance().Warn(__VA_ARGS__)
#define LOG_ERROR(...) CLogger::GetInstance().Error(__VA_ARGS__)
#define LOG_CRITICAL(...) CLogger::GetInstance().Critical(__VA_ARGS__)

// 带文件和行号的日志宏
#define LOG_TRACE_EX(...)                                                      \
  CLogger::GetInstance().TraceEx(__FILE__, __LINE__, __VA_ARGS__)
#define LOG_DEBUG_EX(...)                                                      \
  CLogger::GetInstance().DebugEx(__FILE__, __LINE__, __VA_ARGS__)
#define LOG_INFO_EX(...)                                                       \
  CLogger::GetInstance().InfoEx(__FILE__, __LINE__, __VA_ARGS__)
#define LOG_WARN_EX(...)                                                       \
  CLogger::GetInstance().WarnEx(__FILE__, __LINE__, __VA_ARGS__)
#define LOG_ERROR_EX(...)                                                      \
  CLogger::GetInstance().ErrorEx(__FILE__, __LINE__, __VA_ARGS__)
#define LOG_CRITICAL_EX(...)                                                   \
  CLogger::GetInstance().CriticalEx(__FILE__, __LINE__, __VA_ARGS__)

#endif // _LOGGER_H_