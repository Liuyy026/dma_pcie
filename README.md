# PCIe 数据发送客户端（Qt 版）

该项目是一个面向 Windows 平台的 PCIe 数据发送客户端，使用 Qt6 实现桌面图形界面，并结合自研的 PCIe 传输库完成高速数据下发。应用提供设备扫描、参数配置、文件分发、速率监控等功能，可协助验证板卡 DMA 通道吞吐能力或执行离线数据回放。

---

## 功能亮点

- **PCIe 设备管理**：自动扫描已安装的 XDMA 设备，支持打开/关闭、复位、速率切换及同步模式控制。
- **高速数据发送链路**：文件读取基于 Windows IOCP，数据按块排序后进入 DMA 队列，保障吞吐稳定且线程安全。
- **图形界面操作**：Qt Widgets 界面提供设备列表、参数输入、文件列表管理、实时速率曲线和状态栏信息。
- **日志与监控**：spdlog 输出控制台与文件双通道日志，界面实时显示 DMA 队列占用、速度、累计字节、读写状态等指标。
- **可配置运行参数**：`config/config.json` 中可调整应用名称、文件夹、块大小、线程数量、CPU 亲和性等关键参数。

---

## 目录结构

```
PCIE_QT_20251010/
├── CMakeLists.txt            # 顶层构建脚本
├── config/
│   └── config.json           # 运行时配置
├── common/
│   ├── utils/                # 日志与线程亲和性工具库
│   ├── InOutputData/         # 基于 IOCP 的文件读取流水线
│   └── spdlog/               # 第三方日志头文件
├── pcie/                     # PCIe 设备抽象、DMA 通道实现
├── src/                      # Qt 主程序与 UI 资源
└── build/                    # （可选）本地生成的构建目录
```

### 核心模块说明

- `src/`：Qt `MainWindow`、自定义列表组件 `MyListWidget`、速率图控件 `SpeedChartWidget`、资源文件与应用入口。
- `pcie/`：`PcieFacade` 封装设备扫描 (`PcieScanner`)、设备操作 (`PcieDevice`) 及 DMA 通道 (`DmaChannel` / `DmaChannelAsync`)，向上暴露统一接口。
- `common/InOutputData/`：实现多文件循环读取，利用 IOCP + 有序数据处理器 (`COrderedDataProcessor`) 维持读取结果按提交顺序输出。
- `common/utils/`：日志单例 `CLogger` 基于 spdlog，`ThreadAffinity` 支持跨处理器组的 CPU 亲和性绑定。

---

## 构建指南

### 环境要求

- Windows 10 或以上版本
- CMake ≥ 3.16
- Microsoft Visual Studio 2019/2022（建议 MSVC x64 工具链）或 Ninja + clang-cl
- Qt 6（需要组件：Core、Gui、Widgets、Charts、Concurrent；安装后需获取对应的 `CMAKE_PREFIX_PATH`）
- 已安装的 XDMA 驱动，以及匹配的 PCIe 设备

### 配置 Qt 环境变量

```powershell
$env:CMAKE_PREFIX_PATH = "C:/Qt/6.6.3/msvc2019_64"
```

或在 CMake 配置阶段通过 `-DCMAKE_PREFIX_PATH=` 指定。

### 使用 CMake 生成工程（VS 工具链示例）

```powershell
cd D:/code/PCIE/PCIE_QT_20251010
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 -DCMAKE_PREFIX_PATH="C:/Qt/6.6.3/msvc2022_64"
cmake --build build --config Release
```

### 使用 Ninja 构建（需 `ninja` 与 clang-cl 或 MSVC）

```powershell
cmake -S . -B build -G Ninja -DCMAKE_PREFIX_PATH="C:/Qt/6.6.3/msvc2022_64"
cmake --build build
```

> **提示**：首次构建会在 `build/` 下生成 `bin/`、`lib/` 等输出目录；Qt 相关自动生成文件由 CMake `AUTOMOC/AUTORCC/AUTOUIC` 处理，无需手动预处理。

---

## 运行与使用

1. 构建完成后，运行 `build/bin/PCIE_Demo_QT.exe`（或 `build/bin/Release/PCIE_Demo_QT.exe`）。
2. 首次启动自动创建 `logs/` 目录并写入日志文件 `pcie_demo.log`。
3. 在界面中：
   - 点击“刷新设备”扫描可用板卡；选定后点击“打开”。
   - 通过“DMA 大小”“缓冲区大小”输入框调整当前发送参数（单位分别为 KB / MB）。
   - “发送模式”支持单次列表发送与循环发送。
   - “同步控制”允许打开/关闭板卡硬件同步功能。
   - “待发送文件列表”区域点击“选择”指定数据目录，支持 `.dat` 文件自动列出、排序、删除、上下移动。
   - 点击“开始”进入发送流程，再次点击可停止；状态栏显示实时速率、累计字节、队列占用、读取状态等信息。

> **注意**：启动发送前需确保 PCIe 板卡已正确驱动，并且数据文件可被当前用户读取。

---

## 测试与调优建议（快速上手）

下面的参数为在测试高吞吐（例如接近板卡接口极限）时常用的推荐起点，适合在具备高速 PCIe 板卡与驱动的 Linux/Windows 环境下进行评估：

- 推荐默认值（用于快速验证零拷贝 + 高吞吐）
  - FileBlockSize：2048 （KB，2 MB）
  - IoRequestNum：32
  - DMA 大小（UI 中“DMA 大小(KB)”）：1024 （KB）
  - 缓冲区大小（UI 中“缓冲区大小(MB)”）：64 （MB）

- 调优要点
  - 尽量使用较大块（≥1MB）和较深的并发请求（IoRequestNum ≥ 16~32），可降低每次开销并提升吞吐。
  - 启用零拷贝路径：确保输入读入组件使用 `AlignedBufferPool` 分配的对齐内存，并在打开设备后通过 UI/代码将该池传递给 `PcieFacade::SetBufferPool()`，使 `DmaChannel` 能走描述符（descriptor）路径而不是内存拷贝。
  - 发送缓冲区需足够大以汇聚多个 DMA 请求（64MB 是常见起点），但不要无限增大以免占用过多系统内存。
  - 使用 CPU 亲和（PcieCpuId / DiskCpuId）将文件读取线程与 DMA 发送线程分配到不同核心，减少互相抢占带来的抖动。
  - 硬件/驱动限制：最终极限受板卡固件、驱动与主机平台（PCIe gen/link width）约束，软件参数调整只是优化的一部分。

如需快速测试吞吐，可在 UI 中先使用“内存生成（测试）”模式（若存在）或将 `InputDataFromMemory` 作为临时输入源，然后逐步切换到文件读入的零拷贝路径进行真实文件回放。


## 配置文件说明

`config/config.json` 控制默认运行参数，示例：

```json
{
  "AppName": "PCIe数据发送软件",
  "FolderPath": "D:/test/",
  "ServerUrl": "http://192.168.1.100:8080",
  "Server": false,
  "FileBlockSize": 512,
  "IoRequestNum": 16,
  "PcieCpuId": 1,
  "DiskCpuId": 2
}
```

- `AppName`：应用窗口标题，加载失败时使用默认值。
- `FolderPath`：数据文件默认目录，UI 启动时会读取该路径。
- `FileBlockSize`：单次 IOCP 读取块大小（单位 KB），会乘以 1024 传入，影响磁盘读取吞吐。
- `IoRequestNum`：并发 IO 请求数，决定 IOCP 管线深度。
- `PcieCpuId` / `DiskCpuId`：DMA 发送线程与文件读取线程绑定的逻辑 CPU 编号，用于提升实时性。
- `Server`、`ServerUrl`：当前版本保留字段，可用于扩展远程控制。

修改配置后需重新启动应用生效。

---

## 日志 & 故障排查

- 日志文件位于可执行文件所在目录下的 `logs/pcie_demo.log`，默认保留轮换历史。
- 关键出错路径：
  - 未找到 PCIe 设备：检查驱动安装及管理员权限。
  - 打开设备失败：确认板卡是否被占用、DMA 参数是否匹配固件。
  - 文件读取失败：确认路径、访问权限，以及配置的路径是否存在。
  - 发送卡顿：可调大 `FileBlockSize`、`IoRequestNum` 或增大发送缓冲区；检查 CPU 亲和设置是否合理。

---

## 开发者提示

- Qt GUI 代码在 `src/mainwindow.cpp` 中集中，信号槽对应 UI 控件，可参考 `.ui` 文件进行界面调整。
- 若需启用异步 DMA 通道，可在 `pcie/PcieFacade.h` 中取消注释 `#define DMA_ASYNC`，使用 `DmaChannelAsync`（基于 Overlapped I/O）。
- 项目遵循 C++17，日志与线程模块位于 `common/utils`，涉及 Windows API 的代码需保持 UNICODE 关闭（顶层 CMake 已 `-UUNICODE`）。
- 构建系统默认生成静态库 `pcie`、`InOutputData`、`utils` 并链接到主程序，方便后续拆分部署。

---

## 许可

本项目包含第三方组件 `spdlog`（MIT License）。其余源码版权归原作者/团队所有，分发与使用请遵循内部约定。
