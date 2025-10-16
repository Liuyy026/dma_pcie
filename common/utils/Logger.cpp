#include "Logger.h"
#ifdef _WIN32
#include <Windows.h>
#endif
#include <spdlog/sinks/daily_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <chrono>
#include <stdarg.h>
#include <cstdio>

CLogger::CLogger() : m_bInitialized(false), m_level(LogLevel::Info) {}

CLogger::~CLogger() {
  if (m_bInitialized) {
    m_console_logger.reset();
    m_file_logger.reset();
  }
}

// 使用 Meyer's Singleton 实现
CLogger &CLogger::GetInstance() {
  static CLogger instance;
  return instance;
}

void CLogger::Cleanup() {
  std::lock_guard<std::mutex> lock(m_mutex);
  if (m_bInitialized) {
    m_console_logger.reset();
    m_file_logger.reset();
    spdlog::shutdown();
    m_bInitialized = false;
  }
}

bool CLogger::Initialize(const std::string &logFolderPath, LogLevel level) {
  std::lock_guard<std::mutex> lock(m_mutex); // 使用实例成员互斥锁保护初始化
  if (m_bInitialized)
    return true;

  try {
    // 设置日志格式
    // [时间] [日志级别] [消息内容]
    spdlog::set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] %v");

    // 创建控制台日志记录器
    m_console_logger = spdlog::stdout_color_mt("console");

    // 如果提供了日志文件路径，则创建文件日志记录器
    if (!logFolderPath.empty()) {
      // 根据当前日期创建新的日志文件，每天一个文件，追加模式
      std::string baseName = "pcie_demo";

      std::string logPath = logFolderPath + baseName + ".log";

      // 使用daily_file_sink，每天创建一个新日志文件
      // 参数：文件路径, 小时(0-23), 分钟(0-59), 是否截断
      auto daily_sink = std::make_shared<spdlog::sinks::daily_file_sink_mt>(
          logPath, 0, 0, false);

      // 创建logger，添加daily_sink
      m_file_logger =
          std::make_shared<spdlog::logger>("file_logger", daily_sink);
      spdlog::register_logger(m_file_logger);

      // 设置刷新策略，每5秒刷新一次
      spdlog::flush_every(std::chrono::seconds(5));
      m_file_logger->flush_on(spdlog::level::err);
    }

    // 设置日志级别
    SetLevel(level);

    m_bInitialized = true;
    return true;
  } catch (const spdlog::spdlog_ex &ex) {
    // 初始化失败
    char szErrorMsg[1024] = {0};
    std::snprintf(szErrorMsg, sizeof(szErrorMsg),
                  "Logger initialization failed: %s", ex.what());
#ifdef _WIN32
    OutputDebugStringA(szErrorMsg);
#else
    std::fprintf(stderr, "%s\n", szErrorMsg);
#endif
    return false;
  }
}

void CLogger::SetLevel(LogLevel level) {
  m_level = level;
  spdlog::level::level_enum spdlog_level;

  switch (level) {
  case LogLevel::Trace:
    spdlog_level = spdlog::level::trace;
    break;
  case LogLevel::Debug:
    spdlog_level = spdlog::level::debug;
    break;
  case LogLevel::Info:
    spdlog_level = spdlog::level::info;
    break;
  case LogLevel::Warn:
    spdlog_level = spdlog::level::warn;
    break;
  case LogLevel::Error:
    spdlog_level = spdlog::level::err;
    break;
  case LogLevel::Critical:
    spdlog_level = spdlog::level::critical;
    break;
  case LogLevel::Off:
    spdlog_level = spdlog::level::off;
    break;
  default:
    spdlog_level = spdlog::level::info;
  }

  spdlog::set_level(spdlog_level);

  if (m_console_logger)
    m_console_logger->set_level(spdlog_level);

  if (m_file_logger)
    m_file_logger->set_level(spdlog_level);
}

// 辅助函数，处理格式化字符串和变参
void FormatLogMessage(char *buffer, size_t buffer_size, const char *fmt,
                      va_list args) {
  vsnprintf(buffer, buffer_size, fmt, args);
}

// 日志记录函数实现
void CLogger::Trace(const char *fmt, ...) {
  if (!m_bInitialized || m_level > LogLevel::Trace)
    return;

  char buffer[4096] = {0};
  va_list args;
  va_start(args, fmt);
  FormatLogMessage(buffer, sizeof(buffer), fmt, args);
  va_end(args);

  m_console_logger->trace(buffer);
  if (m_file_logger)
    m_file_logger->trace(buffer);
}

void CLogger::Debug(const char *fmt, ...) {
  if (!m_bInitialized || m_level > LogLevel::Debug)
    return;

  char buffer[4096] = {0};
  va_list args;
  va_start(args, fmt);
  FormatLogMessage(buffer, sizeof(buffer), fmt, args);
  va_end(args);

  m_console_logger->debug(buffer);
  if (m_file_logger)
    m_file_logger->debug(buffer);
}

void CLogger::Info(const char *fmt, ...) {
  if (!m_bInitialized || m_level > LogLevel::Info)
    return;

  char buffer[4096] = {0};
  va_list args;
  va_start(args, fmt);
  FormatLogMessage(buffer, sizeof(buffer), fmt, args);
  va_end(args);

  m_console_logger->info(buffer);
  if (m_file_logger)
    m_file_logger->info(buffer);
}

void CLogger::Warn(const char *fmt, ...) {
  if (!m_bInitialized || m_level > LogLevel::Warn)
    return;

  char buffer[4096] = {0};
  va_list args;
  va_start(args, fmt);
  FormatLogMessage(buffer, sizeof(buffer), fmt, args);
  va_end(args);

  m_console_logger->warn(buffer);
  if (m_file_logger)
    m_file_logger->warn(buffer);
}

void CLogger::Error(const char *fmt, ...) {
  if (!m_bInitialized || m_level > LogLevel::Error)
    return;

  char buffer[4096] = {0};
  va_list args;
  va_start(args, fmt);
  FormatLogMessage(buffer, sizeof(buffer), fmt, args);
  va_end(args);

  m_console_logger->error(buffer);
  if (m_file_logger)
    m_file_logger->error(buffer);
}

void CLogger::Critical(const char *fmt, ...) {
  if (!m_bInitialized || m_level > LogLevel::Critical)
    return;

  char buffer[4096] = {0};
  va_list args;
  va_start(args, fmt);
  FormatLogMessage(buffer, sizeof(buffer), fmt, args);
  va_end(args);

  m_console_logger->critical(buffer);
  if (m_file_logger)
    m_file_logger->critical(buffer);
}

// 带文件和行号的日志记录函数实现
void CLogger::TraceEx(const char *file, int line, const char *fmt, ...) {
  if (!m_bInitialized || m_level > LogLevel::Trace)
    return;

  char buffer[4096] = {0};
  va_list args;
  va_start(args, fmt);
  FormatLogMessage(buffer, sizeof(buffer), fmt, args);
  va_end(args);

  m_console_logger->trace("[{}:{}] {}", file, line, buffer);
  if (m_file_logger)
    m_file_logger->trace("[{}:{}] {}", file, line, buffer);
}

void CLogger::DebugEx(const char *file, int line, const char *fmt, ...) {
  if (!m_bInitialized || m_level > LogLevel::Debug)
    return;

  char buffer[4096] = {0};
  va_list args;
  va_start(args, fmt);
  FormatLogMessage(buffer, sizeof(buffer), fmt, args);
  va_end(args);

  m_console_logger->debug("[{}:{}] {}", file, line, buffer);
  if (m_file_logger)
    m_file_logger->debug("[{}:{}] {}", file, line, buffer);
}

void CLogger::InfoEx(const char *file, int line, const char *fmt, ...) {
  if (!m_bInitialized || m_level > LogLevel::Info)
    return;

  char buffer[4096] = {0};
  va_list args;
  va_start(args, fmt);
  FormatLogMessage(buffer, sizeof(buffer), fmt, args);
  va_end(args);

  m_console_logger->info("[{}:{}] {}", file, line, buffer);
  if (m_file_logger)
    m_file_logger->info("[{}:{}] {}", file, line, buffer);
}

void CLogger::WarnEx(const char *file, int line, const char *fmt, ...) {
  if (!m_bInitialized || m_level > LogLevel::Warn)
    return;

  char buffer[4096] = {0};
  va_list args;
  va_start(args, fmt);
  FormatLogMessage(buffer, sizeof(buffer), fmt, args);
  va_end(args);

  m_console_logger->warn("[{}:{}] {}", file, line, buffer);
  if (m_file_logger)
    m_file_logger->warn("[{}:{}] {}", file, line, buffer);
}

void CLogger::ErrorEx(const char *file, int line, const char *fmt, ...) {
  if (!m_bInitialized || m_level > LogLevel::Error)
    return;

  char buffer[4096] = {0};
  va_list args;
  va_start(args, fmt);
  FormatLogMessage(buffer, sizeof(buffer), fmt, args);
  va_end(args);

  m_console_logger->error("[{}:{}] {}", file, line, buffer);
  if (m_file_logger)
    m_file_logger->error("[{}:{}] {}", file, line, buffer);
}

void CLogger::CriticalEx(const char *file, int line, const char *fmt, ...) {
  if (!m_bInitialized || m_level > LogLevel::Critical)
    return;

  char buffer[4096] = {0};
  va_list args;
  va_start(args, fmt);
  FormatLogMessage(buffer, sizeof(buffer), fmt, args);
  va_end(args);

  m_console_logger->critical("[{}:{}] {}", file, line, buffer);
  if (m_file_logger)
    m_file_logger->critical("[{}:{}] {}", file, line, buffer);
}
