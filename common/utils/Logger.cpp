#include "Logger.h"
#ifdef _WIN32
#include <Windows.h>
#endif
#include <spdlog/sinks/daily_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <chrono>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <stdarg.h>
#include <system_error>

CLogger::CLogger()
    : m_bInitialized(false), m_level(LogLevel::Info),
      m_logFolderPath(), m_modeTag("general") {
  std::atomic_store(&m_file_logger, std::shared_ptr<spdlog::logger>());
}

CLogger::~CLogger() {
  if (m_bInitialized) {
    m_console_logger.reset();
    std::atomic_store(&m_file_logger, std::shared_ptr<spdlog::logger>());
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
    auto logger = std::atomic_exchange(&m_file_logger, std::shared_ptr<spdlog::logger>());
    if (logger) {
      logger->flush();
      spdlog::drop(logger->name());
    }
    spdlog::shutdown();
    m_bInitialized = false;
    m_logFolderPath.clear();
    m_modeTag = "general";
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

    if (!logFolderPath.empty()) {
      std::filesystem::path folder(logFolderPath);
      std::error_code ec;
      std::filesystem::create_directories(folder, ec);
      if (ec) {
        char szErrorMsg[1024] = {0};
        std::snprintf(szErrorMsg, sizeof(szErrorMsg),
                      "Logger failed to create directory %s: %s",
                      folder.string().c_str(), ec.message().c_str());
#ifdef _WIN32
        OutputDebugStringA(szErrorMsg);
#else
        std::fprintf(stderr, "%s\n", szErrorMsg);
#endif
        m_logFolderPath.clear();
      } else {
        m_logFolderPath = std::move(folder);
        pruneOldLogsLocked();
        configureFileLoggerLocked("general");
      }
    } else {
      m_logFolderPath.clear();
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

void CLogger::SetModeTag(const std::string &modeTag) {
  std::lock_guard<std::mutex> lock(m_mutex);
  if (!m_bInitialized)
    return;
  if (m_logFolderPath.empty())
    return;

  std::string sanitized = sanitizeModeTag(modeTag);
  if (sanitized == m_modeTag)
    return;

  configureFileLoggerLocked(sanitized);
  pruneOldLogsLocked();
  SetLevel(m_level);
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

  auto file_logger = std::atomic_load(&m_file_logger);
  if (file_logger)
    file_logger->set_level(spdlog_level);
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
  auto file_logger = std::atomic_load(&m_file_logger);
  if (file_logger)
    file_logger->trace(buffer);
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
  auto file_logger = std::atomic_load(&m_file_logger);
  if (file_logger)
    file_logger->debug(buffer);
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
  auto file_logger = std::atomic_load(&m_file_logger);
  if (file_logger)
    file_logger->info(buffer);
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
  auto file_logger = std::atomic_load(&m_file_logger);
  if (file_logger)
    file_logger->warn(buffer);
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
  auto file_logger = std::atomic_load(&m_file_logger);
  if (file_logger)
    file_logger->error(buffer);
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
  auto file_logger = std::atomic_load(&m_file_logger);
  if (file_logger)
    file_logger->critical(buffer);
}

void CLogger::configureFileLoggerLocked(const std::string &modeTag) {
  if (m_logFolderPath.empty())
    return;

  std::string sanitized = sanitizeModeTag(modeTag);
  const std::string baseName = "pcie_demo";
  std::string fileName = baseName;
  if (!sanitized.empty()) {
    fileName += "_" + sanitized;
  }
  fileName += ".log";

  std::filesystem::path fullPath = m_logFolderPath / fileName;
  auto daily_sink = std::make_shared<spdlog::sinks::daily_file_sink_mt>(
      fullPath.string(), 0, 0, false);

  auto oldLogger = std::atomic_load(&m_file_logger);
  if (oldLogger) {
    oldLogger->flush();
    spdlog::drop(oldLogger->name());
  }

  auto newLogger = std::make_shared<spdlog::logger>("file_logger", daily_sink);
  spdlog::register_logger(newLogger);
  spdlog::flush_every(std::chrono::seconds(5));
  newLogger->flush_on(spdlog::level::err);
  std::atomic_store(&m_file_logger, newLogger);
  m_modeTag = sanitized;
}

void CLogger::pruneOldLogsLocked() {
  if (m_logFolderPath.empty())
    return;

  namespace fs = std::filesystem;
  std::error_code ec;
  if (!fs::exists(m_logFolderPath, ec) || ec)
    return;

  auto now = std::chrono::system_clock::now();
  const auto maxAge = std::chrono::hours(24 * 7);

  for (fs::directory_iterator it(m_logFolderPath, ec);
       !ec && it != fs::directory_iterator(); ++it) {
    if (!it->is_regular_file(ec) || ec)
      continue;

    auto ftime = it->last_write_time(ec);
    if (ec)
      continue;

    auto sysTime = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
        ftime - decltype(ftime)::clock::now() + now);
    if (now - sysTime > maxAge) {
      fs::remove(it->path(), ec);
      if (!ec && m_console_logger) {
        m_console_logger->info("删除过期日志: {}", it->path().string());
      }
      ec.clear();
    }
  }
}

std::string CLogger::sanitizeModeTag(const std::string &modeTag) const {
  if (modeTag.empty())
    return "general";

  std::string cleaned;
  cleaned.reserve(modeTag.size());
  bool lastUnderscore = false;
  for (char ch : modeTag) {
    unsigned char uch = static_cast<unsigned char>(ch);
    if (std::isalnum(uch)) {
      cleaned.push_back(static_cast<char>(std::tolower(uch)));
      lastUnderscore = false;
    } else if (ch == '-' || ch == '_') {
      if (!lastUnderscore && !cleaned.empty()) {
        cleaned.push_back('_');
        lastUnderscore = true;
      }
    }
  }

  if (cleaned.empty())
    return "general";

  if (cleaned.back() == '_')
    cleaned.pop_back();

  if (cleaned.empty())
    return "general";

  return cleaned;
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
  auto file_logger = std::atomic_load(&m_file_logger);
  if (file_logger)
    file_logger->trace("[{}:{}] {}", file, line, buffer);
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
  auto file_logger = std::atomic_load(&m_file_logger);
  if (file_logger)
    file_logger->debug("[{}:{}] {}", file, line, buffer);
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
  auto file_logger = std::atomic_load(&m_file_logger);
  if (file_logger)
    file_logger->info("[{}:{}] {}", file, line, buffer);
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
  auto file_logger = std::atomic_load(&m_file_logger);
  if (file_logger)
    file_logger->warn("[{}:{}] {}", file, line, buffer);
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
  auto file_logger = std::atomic_load(&m_file_logger);
  if (file_logger)
    file_logger->error("[{}:{}] {}", file, line, buffer);
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
  auto file_logger = std::atomic_load(&m_file_logger);
  if (file_logger)
    file_logger->critical("[{}:{}] {}", file, line, buffer);
}
