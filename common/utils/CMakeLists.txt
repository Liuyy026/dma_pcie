cmake_minimum_required(VERSION 3.10)

# 添加库文件
add_library(utils
  AlignedBufferPool.cpp
  AlignedBufferPool.h
  ThreadAffinity.h
  Logger.cpp
  Logger.h
  ShmRing.cpp
  ShmRing.h
)

# 包含目录
target_include_directories(utils PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_CURRENT_SOURCE_DIR}/..
)

# 添加spdlog依赖
target_include_directories(utils PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/../spdlog/include
) 
