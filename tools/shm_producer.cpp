#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <iostream>
#include "../common/utils/ShmRing.h"

int main(int argc, char **argv) {
  const char *name = "/pcie_shm_ring";
  uint32_t slots = 32;
  uint32_t slot_size = 512 * 1024;

  // try create
  ShmRing ring;
  if (!ring.Open(name, slots, slot_size, true)) {
    std::cerr << "创建/打开 shm 失败\n";
    return 1;
  }

  // 简单循环写入若干次
  for (int i = 0; i < 100; ++i) {
    uint8_t *ptr = nullptr;
    uint32_t cap = 0;
    uint64_t idx = 0;
    while (!ring.ProducerAcquire(ptr, cap, idx)) {
      // full, wait
      usleep(1000);
    }
    // first 4 bytes reserved for length
    uint8_t *payload = ptr + sizeof(uint32_t);
    uint32_t payload_cap = cap - sizeof(uint32_t);
    uint32_t write_len = std::min<uint32_t>(payload_cap, 1024);
    for (uint32_t j = 0; j < write_len; ++j) payload[j] = static_cast<uint8_t>(j & 0xFF);
    ring.ProducerCommit(idx, write_len);
    usleep(1000);
  }
  std::cout << "producer done\n";
  return 0;
}
