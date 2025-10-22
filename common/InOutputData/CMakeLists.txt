# 设置公共源文件
set(INOUTPUTDATA_SOURCES
  OrderedDataProcessor.cpp
  OrderedDataProcessor.h
  VirtualMemoryBuffer.h
  InputBlock.h
  InputDataFromMemory.cpp
  InputDataFromMemory.h
  InputDataFromSHM.cpp
  InputDataFromSHM.h
  FileDataProvider.h
)

if (WIN32)
  list(APPEND INOUTPUTDATA_SOURCES
    InputDataFromFileUseIOCP.cpp
    InputDataFromFileUseIOCP.h
    IOCPHandle.cpp
    IOCPHandle.h
    FileHandle.cpp
    FileHandle.h
    FileHandleManager.cpp
    FileHandleManager.h
    OverlappedQueue.cpp
    OverlappedQueue.h
  )
else()
  list(APPEND INOUTPUTDATA_SOURCES
    InputDataFromFileUsePOSIX.cpp
    InputDataFromFileUsePOSIX.h
    PreadFileDataProvider.cpp
    PreadFileDataProvider.h
  )
endif()

add_library(InOutputData ${INOUTPUTDATA_SOURCES})

# 包含目录
target_include_directories(InOutputData PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_CURRENT_SOURCE_DIR}/..
)

# 添加需要的链接库
target_link_libraries(InOutputData PUBLIC
  utils
) 
