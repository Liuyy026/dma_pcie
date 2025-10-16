set(PROJECT_SOURCES
    main.cpp
    mainwindow.cpp
    mainwindow.h
    mainwindow.ui
    mylistwidget.cpp
    mylistwidget.h
    speedchartwidget.cpp
    speedchartwidget.h
    resources.qrc
    app.rc
)

# 禁用UNICODE和_UNICODE宏
add_definitions(-UUNICODE -U_UNICODE)

# 创建可执行文件
add_executable(PCIE_Demo_QT
    ${PROJECT_SOURCES}
)

# 包含头文件目录
target_include_directories(PCIE_Demo_QT PRIVATE
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/pcie
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/common
    ${CMAKE_SOURCE_DIR}/common/spdlog/include
    ${CMAKE_SOURCE_DIR}/common/InOutputData
    ${CMAKE_SOURCE_DIR}/common/json_struct
)

# 链接库
target_link_libraries(PCIE_Demo_QT PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
    Qt6::Charts
    Qt6::Concurrent
    pcie
    InOutputData
    utils
)

# 设置Windows应用程序属性
set_target_properties(PCIE_Demo_QT PROPERTIES
    WIN32_EXECUTABLE TRUE
    MACOSX_BUNDLE TRUE
) 