#ifndef SHM_RING_H
#define SHM_RING_H

#include <atomic>
#include <cstdint>
#include <string>

// 简单的 POSIX 共享内存环缓冲区，单生产者单消费者（SPSC）
// layout in shm:
// [Magic(8)][Version(4)][SlotCount(4)][SlotSize(4)][padding]
// [head(uint64_t)][tail(uint64_t)][slots...]

struct ShmRingHeader {
  uint64_t magic;
  uint32_t version;
  uint32_t slot_count;
  uint32_t slot_size;
  uint32_t reserved;
  alignas(64) std::atomic<uint64_t> head; // consumer reads from head
  alignas(64) std::atomic<uint64_t> tail; // producer writes to tail
};

class ShmRing {
public:
  ShmRing();
  ~ShmRing();

  // open existing or create if create_if_missing==true
  bool Open(const std::string &name, uint32_t slot_count, uint32_t slot_size, bool create_if_missing = false);
  void Close();

  // producer helpers (for test producer)
  bool ProducerAcquire(uint8_t *&ptr, uint32_t &capacity, uint64_t &index);
  bool ProducerCommit(uint64_t index, uint32_t len);

  // consumer helpers
  bool ConsumerPeek(uint8_t *&ptr, uint32_t &len, uint64_t &index);
  bool ConsumerAdvance(uint64_t index);

  // get metadata
  uint32_t SlotSize() const;
  uint32_t SlotCount() const;

private:
  int shm_fd_;
  void *map_addr_;
  size_t map_size_;
  ShmRingHeader *header_;
  uint8_t *slots_base_;
  std::string name_;

  bool CreateShm(const std::string &name, uint32_t slot_count, uint32_t slot_size);
  bool MapShm(int fd, size_t size);
};

#endif // SHM_RING_H
