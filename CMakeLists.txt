cmake_minimum_required(VERSION 3.16)
project(PCIE_Demo_QT VERSION 1.0.0 LANGUAGES CXX)

# 设置C++标准
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if (WIN32)
  add_definitions(-DPLATFORM_WINDOWS -UUNICODE -U_UNICODE)
else()
  add_definitions(-DPLATFORM_LINUX)
endif()

# 设置自动生成moc文件
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

# 查找Qt6包
find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets Charts Concurrent)

# 设置输出目录
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

# Ensure runtime config is available in the binary output directory so the
# application can find config/config.json when run from the build tree.
set(RUNTIME_CONFIG_DIR ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/config)
file(MAKE_DIRECTORY ${RUNTIME_CONFIG_DIR})
configure_file(${CMAKE_SOURCE_DIR}/config/config.json ${RUNTIME_CONFIG_DIR}/config.json COPYONLY)

# 添加子目录
add_subdirectory(common/utils)
add_subdirectory(common/InOutputData)
add_subdirectory(pcie)
add_subdirectory(src)

