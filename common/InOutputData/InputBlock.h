#ifndef INPUT_BLOCK_H
#define INPUT_BLOCK_H

#include <cstddef>
#include <cstdint>

#ifdef _WIN32
#include <Windows.h>
#endif

struct InputBlock {
#ifdef _WIN32
  OVERLAPPED overlapped{};
#endif
  std::uint8_t *buffer{nullptr};
  std::size_t buffer_capacity{0};
  std::size_t data_length{0};
  std::uint64_t index{0};
  std::uint64_t file_id{0};
  void *user_context{nullptr};
#ifndef _WIN32
  std::int64_t file_offset{0};
#endif
};

#endif // INPUT_BLOCK_H
