#ifndef THREAD_AFFINITY_H
#define THREAD_AFFINITY_H

#include <cstdint>
#include <string>
#include <vector>

#ifdef _WIN32
#include <Windows.h>
#include <winnt.h>
#elif defined(__linux__)
#include <pthread.h>
#include <sched.h>
#include <unistd.h>
#else
#include <pthread.h>
#include <unistd.h>
#endif

class ThreadAffinity {
public:
  static ThreadAffinity &GetInstance() {
    static ThreadAffinity instance;
    return instance;
  }

#ifdef _WIN32
  bool SetThreadAffinity(HANDLE thread_handle, DWORD processor_id) {
    if (processor_id >= processor_count_) {
      return false;
    }

    DWORD target_group = 0;
    DWORD processor_index = processor_id;
    for (DWORD i = 0; i < processor_group_count_; ++i) {
      if (processor_index < processors_in_group_[i]) {
        target_group = i;
        break;
      }
      processor_index -= processors_in_group_[i];
    }

    return SetThreadAffinityToGroup(thread_handle,
                                    static_cast<WORD>(target_group),
                                    1ull << (processor_index % 64));
  }

  bool SetThreadAffinityToGroup(HANDLE thread_handle, WORD group_id,
                                KAFFINITY affinity_mask) {
    if (group_id >= processor_group_count_) {
      return false;
    }

    GROUP_AFFINITY group_affinity = {};
    group_affinity.Group = group_id;
    group_affinity.Mask = affinity_mask;

    return SetThreadGroupAffinity(thread_handle, &group_affinity, nullptr)
               ? true
               : false;
  }

  DWORD GetProcessorGroupCount() const { return processor_group_count_; }
  DWORD GetProcessorCount() const { return processor_count_; }
#elif defined(__linux__)
  bool SetThreadAffinity(pthread_t thread_handle, int processor_id) {
    if (processor_id < 0 ||
        processor_id >= static_cast<int>(processor_count_)) {
      return false;
    }

    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(static_cast<std::size_t>(processor_id), &cpuset);

    return pthread_setaffinity_np(thread_handle, sizeof(cpu_set_t), &cpuset) ==
           0;
  }
#else
  bool SetThreadAffinity(pthread_t, int) { return false; }
#endif

  std::string GetProcessorInfo() const {
#ifdef _WIN32
    std::string info = "处理器信息:\n";
    info += "  总处理器数: " + std::to_string(processor_count_) + "\n";
    info += "  处理器组数: " + std::to_string(processor_group_count_) + "\n";
    for (std::size_t i = 0; i < processors_in_group_.size(); ++i) {
      info += "  组 " + std::to_string(i) + ": " +
              std::to_string(processors_in_group_[i]) + " 个处理器\n";
    }
    return info;
#else
    return "处理器信息: 使用pthread接口设置亲和性";
#endif
  }

private:
  ThreadAffinity() { Initialize(); }

  ThreadAffinity(const ThreadAffinity &) = delete;
  ThreadAffinity &operator=(const ThreadAffinity &) = delete;

  void Initialize() {
#ifdef _WIN32
    processor_group_count_ = GetActiveProcessorGroupCount();
    processors_in_group_.resize(processor_group_count_);
    processor_count_ = 0;
    for (WORD i = 0; i < processor_group_count_; ++i) {
      processors_in_group_[i] = GetActiveProcessorCount(i);
      processor_count_ += processors_in_group_[i];
    }
#else
    processor_count_ =
        static_cast<std::size_t>(sysconf(_SC_NPROCESSORS_ONLN));
#endif
  }

#ifdef _WIN32
  DWORD processor_count_ = 0;
  DWORD processor_group_count_ = 0;
  std::vector<DWORD> processors_in_group_;
#else
  std::size_t processor_count_ = 0;
#endif
};

#endif // THREAD_AFFINITY_H
