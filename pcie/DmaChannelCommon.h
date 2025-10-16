#pragma once

#include <cstddef>
#include <cstdint>

enum class ThreadState { NotStarted, Running, Stopping, Stopped, Error };

struct DmaChannelStatus {
  ThreadState threadState{ThreadState::NotStarted};
  std::size_t queueSize{0};
  std::size_t queueUsed{0};
  float queueUsagePercent{0.0f};
  std::uint64_t totalBytesSent{0};
  std::uint64_t currentSpeed{0};
  int step{0};
  // Diagnostic counters for write path timing
  std::uint64_t descWriteCount{0};       // number of descriptor-path writes
  std::uint64_t descWriteNsTotal{0};     // total nanoseconds spent in descriptor-path write()
  std::uint64_t copyWriteCount{0};       // number of copy-path writes
  std::uint64_t copyWriteNsTotal{0};     // total nanoseconds spent in copy-path write()
};
