set(PCIE_SOURCES
    SPSCMemoryQueue.cpp
    SPSCMemoryQueue.h
    SPSCDescriptorQueue.cpp
    SPSCDescriptorQueue.h
    PcieScanner.cpp
    PcieScanner.h
    PcieFacade.cpp
    PcieFacade.h
    PcieTypes.h
    DmaChannelCommon.h
)

if (WIN32)
    list(APPEND PCIE_SOURCES
        xdma_public.h
        PcieDevice.cpp
        PcieDevice.h
        DeviceHandle.h
        DmaChannel.cpp
        DmaChannel.h
        DmaChannelAsync.cpp
        DmaChannelAsync.h
    )
else()
    list(APPEND PCIE_SOURCES
        PcieDeviceLinux.cpp
        PcieDeviceLinux.h
        DmaChannelLinux.cpp
        DmaChannelLinux.h
    )
endif()

# 创建静态库
add_library(pcie STATIC ${PCIE_SOURCES})

# 设置C++17标准
target_compile_features(pcie PUBLIC cxx_std_17)
set_target_properties(pcie PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
)

# 添加编译器警告
if(MSVC)
    target_compile_options(pcie PRIVATE /W4 /wd4100)
else()
    target_compile_options(pcie PRIVATE -Wall -Wextra -Wpedantic)
endif()

# 包含头文件目录
target_include_directories(pcie PUBLIC 
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/common
    ${CMAKE_SOURCE_DIR}/common/utils
    ${CMAKE_SOURCE_DIR}/common/spdlog/include
)

# 添加外部库依赖
target_link_libraries(pcie PUBLIC utils)

if (WIN32)
    target_compile_definitions(pcie PRIVATE -UUNICODE -U_UNICODE)
    target_link_libraries(pcie PUBLIC setupapi.lib)
else()
    find_package(Threads REQUIRED)
    target_link_libraries(pcie PUBLIC Threads::Threads)
endif()
