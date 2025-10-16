#ifndef _SPSC_MEMORY_QUEUE_H__
#define _SPSC_MEMORY_QUEUE_H__

#include <algorithm>
#include <atomic>
#include <new>
#include <string.h>

#ifndef __hardware_constructive_interference_size__
#define __hardware_constructive_interference_size__
constexpr std::size_t hardware_constructive_interference_size = 64;
constexpr std::size_t hardware_destructive_interference_size = 64;
#endif

// 无锁单线程读/单线程写内存队列

class SPSCMemoryQueue
{
public:
    SPSCMemoryQueue();
    ~SPSCMemoryQueue();

    SPSCMemoryQueue(const SPSCMemoryQueue &) = delete;
    SPSCMemoryQueue &operator=(const SPSCMemoryQueue &) = delete;

    bool Init(uint32_t size);
    void Reset();

    uint32_t Put(unsigned char *data, uint32_t data_length);
    bool PutFixedLength(unsigned char *data, uint32_t data_length);
    uint32_t Get(unsigned char *data, uint32_t data_length);
    bool GetFixedLength(unsigned char *data, uint32_t data_length);

    // 判断队列是否为空
    bool IsEmpty() const;

    // 获取队列中的数据量
    uint32_t GetDataSize() const;

private:
    char padding0_[hardware_destructive_interference_size];
    uint32_t size_;
    unsigned char *buffer_;

    alignas(hardware_destructive_interference_size)
        std::atomic<unsigned int> get_index_;
    alignas(hardware_destructive_interference_size)
        std::atomic<unsigned int> put_index_;

    char padding1_[hardware_destructive_interference_size -
                   sizeof(std::atomic<unsigned int>)];
};

#endif // _SPSC_MEMORY_QUEUE_H__