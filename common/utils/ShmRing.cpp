#include "ShmRing.h"
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cstring>
#include <cerrno>
#include <iostream>

static constexpr uint64_t SHM_MAGIC = 0x53484D52494E4759ULL; // "SHMRINGY"
static constexpr uint32_t SHM_VERSION = 1;

ShmRing::ShmRing() : shm_fd_(-1), map_addr_(nullptr), map_size_(0), header_(nullptr), slots_base_(nullptr) {}

ShmRing::~ShmRing() { Close(); }

bool ShmRing::CreateShm(const std::string &name, uint32_t slot_count, uint32_t slot_size) {
  int fd = shm_open(name.c_str(), O_CREAT | O_EXCL | O_RDWR, 0666);
  if (fd < 0) {
    if (errno == EEXIST) return false;
    return false;
  }

  size_t header_sz = sizeof(ShmRingHeader);
  size_t slots_sz = static_cast<size_t>(slot_count) * slot_size;
  size_t total = header_sz + slots_sz;

  if (ftruncate(fd, static_cast<off_t>(total)) != 0) {
    close(fd);
    shm_unlink(name.c_str());
    return false;
  }

  void *addr = mmap(nullptr, total, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (addr == MAP_FAILED) {
    close(fd);
    shm_unlink(name.c_str());
    return false;
  }

  // initialize header
  ShmRingHeader *hdr = reinterpret_cast<ShmRingHeader *>(addr);
  hdr->magic = SHM_MAGIC;
  hdr->version = SHM_VERSION;
  hdr->slot_count = slot_count;
  hdr->slot_size = slot_size;
  hdr->reserved = 0;
  hdr->head.store(0);
  hdr->tail.store(0);

  munmap(addr, total);
  close(fd);
  return true;
}

bool ShmRing::MapShm(int fd, size_t size) {
  void *addr = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (addr == MAP_FAILED) return false;

  map_addr_ = addr;
  map_size_ = size;
  header_ = reinterpret_cast<ShmRingHeader *>(map_addr_);
  slots_base_ = reinterpret_cast<uint8_t *>(map_addr_) + sizeof(ShmRingHeader);
  return true;
}

bool ShmRing::Open(const std::string &name, uint32_t slot_count, uint32_t slot_size, bool create_if_missing) {
  name_ = name;
  std::string shm_name = name;
  if (shm_name.empty() || shm_name[0] != '/') shm_name = std::string("/") + name;

  // try open existing
  int fd = shm_open(shm_name.c_str(), O_RDWR, 0);
  if (fd < 0) {
    if (errno == ENOENT && create_if_missing) {
      if (!CreateShm(shm_name, slot_count, slot_size)) return false;
      fd = shm_open(shm_name.c_str(), O_RDWR, 0);
      if (fd < 0) return false;
    } else {
      return false;
    }
  }

  // get size
  struct stat st;
  if (fstat(fd, &st) != 0) {
    close(fd);
    return false;
  }

  size_t total = static_cast<size_t>(st.st_size);
  if (!MapShm(fd, total)) {
    close(fd);
    return false;
  }

  // validate header
  if (header_->magic != SHM_MAGIC || header_->version != SHM_VERSION) {
    // mismatched
    munmap(map_addr_, map_size_);
    close(fd);
    header_ = nullptr;
    map_addr_ = nullptr;
    return false;
  }

  shm_fd_ = fd;
  return true;
}

void ShmRing::Close() {
  if (map_addr_) {
    munmap(map_addr_, map_size_);
    map_addr_ = nullptr;
    header_ = nullptr;
    slots_base_ = nullptr;
  }
  if (shm_fd_ >= 0) {
    close(shm_fd_);
    shm_fd_ = -1;
  }
}

uint32_t ShmRing::SlotSize() const { return header_ ? header_->slot_size : 0; }
uint32_t ShmRing::SlotCount() const { return header_ ? header_->slot_count : 0; }

bool ShmRing::ProducerAcquire(uint8_t *&ptr, uint32_t &capacity, uint64_t &index) {
  if (!header_) return false;
  uint64_t tail = header_->tail.load(std::memory_order_relaxed);
  uint64_t head = header_->head.load(std::memory_order_acquire);
  uint64_t next = tail + 1;
  uint64_t count = header_->slot_count;
  if ((next - head) > count) {
    // full
    return false;
  }
  uint64_t slot = tail % count;
  ptr = slots_base_ + slot * header_->slot_size;
  capacity = header_->slot_size;
  index = tail;
  // caller will fill and then Commit(index, len)
  return true;
}

bool ShmRing::ProducerCommit(uint64_t index, uint32_t len) {
  if (!header_) return false;
  // store length at start of slot (first 4 bytes)
  uint64_t slot = index % header_->slot_count;
  uint8_t *p = slots_base_ + slot * header_->slot_size;
  uint32_t stored_len = len;
  std::memcpy(p, &stored_len, sizeof(uint32_t));
  // advance tail
  header_->tail.fetch_add(1, std::memory_order_release);
  return true;
}

bool ShmRing::ConsumerPeek(uint8_t *&ptr, uint32_t &len, uint64_t &index) {
  if (!header_) return false;
  uint64_t head = header_->head.load(std::memory_order_relaxed);
  uint64_t tail = header_->tail.load(std::memory_order_acquire);
  if (head >= tail) return false; // empty
  uint64_t slot = head % header_->slot_count;
  uint8_t *p = slots_base_ + slot * header_->slot_size;
  uint32_t stored_len = 0;
  std::memcpy(&stored_len, p, sizeof(uint32_t));
  if (stored_len == 0 || stored_len > header_->slot_size - sizeof(uint32_t)) {
    // not yet committed or corrupted
    return false;
  }
  ptr = p + sizeof(uint32_t);
  len = stored_len;
  index = head;
  return true;
}

bool ShmRing::ConsumerAdvance(uint64_t index) {
  if (!header_) return false;
  uint64_t expected = index;
  uint64_t head = header_->head.load(std::memory_order_relaxed);
  if (head != expected) return false;
  header_->head.fetch_add(1, std::memory_order_release);
  return true;
}
