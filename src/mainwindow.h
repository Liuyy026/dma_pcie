#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QAtomicInt>
#include <QFileDialog>
#include <QMainWindow>
#include <QMessageBox>
#include <QMutex>
#include <QSettings>
#include <QThread>
#include <QTimer>
#include <cstdint>
#include <vector>

#include "PcieFacade.h"
#include "json_struct.h"
#include "../common/InOutputData/InputDataFromMemory.h"
#include "../common/InOutputData/InputDataFromSHM.h"
#include "../common/utils/AlignedBufferPool.h"

#ifdef _WIN32
#include "InputDataFromFileUseIOCP.h"
using InputDataReader = CInputDataFromFileUseIOCP;
using InputDataReaderInitParam = InputDataFromFileUseIOCPInitParam;
#else
#include "InputDataFromFileUsePOSIX.h"
using InputDataReader = CInputDataFromFileUsePOSIX;
using InputDataReaderInitParam = InputDataFromFileUsePOSIXInitParam;
#endif

#define BUFFER_SIZE 8 * 1024 * 1024

// 配置结构体
struct Setting {
  std::string AppName;
  std::string FolderPath;
  std::string ServerUrl;
  bool Server;
  unsigned int FileBlockSize;
  unsigned int IoRequestNum;
  int PcieCpuId;
  int DiskCpuId;
  int DiskCpuIdSpan;
  int DiskProducerCpuId;
  unsigned int DiskPendingSlack;
  bool DiskUseStreamingReader;
  std::vector<int> DiskWorkerCpuIds;

  Setting() {
    AppName = "PCIe数据发送软件";
#ifdef _WIN32
    FolderPath = "D:/test/";
#else
    FolderPath = "/tmp/";
#endif
    ServerUrl = "http://192.168.1.100:8080";
    Server = false;
    // 默认较大的块和更多并发请求以便更容易达到高吞吐
    FileBlockSize = 4096; // 单位KB -> 4MB
    IoRequestNum = 64;
    PcieCpuId = 1;
    DiskCpuId = 2;
    DiskCpuIdSpan = 0;
    DiskProducerCpuId = -1;
    DiskPendingSlack = 0; // 0 表示自动按并发的25%计算
    DiskUseStreamingReader = false;
    DiskWorkerCpuIds.clear();
  }
};

JS_OBJ_EXT(Setting, AppName, FolderPath, ServerUrl, Server, FileBlockSize,
           IoRequestNum, PcieCpuId, DiskCpuId, DiskCpuIdSpan,
           DiskProducerCpuId, DiskPendingSlack, DiskUseStreamingReader,
           DiskWorkerCpuIds)

namespace Ui {
class MainWindow;
}

class MainWindow : public QMainWindow, public IInputDataListener {
  Q_OBJECT

public:
  explicit MainWindow(QWidget *parent = nullptr);
  ~MainWindow();

  // 显示错误信息
  void ShowErrorMessage(const std::string &sErrorInfo);

  // 输入数据接口实现
  virtual void InputData(unsigned char *pData, std::uint64_t uiDataLength,
                         void *pParam = NULL);

private slots:
  // 刷新设备列表
  void onRefreshDevices();

  // 当前设备变更
  void onDeviceChanged(int index);

  // 打开设备
  void onOpenDevice();

  // 关闭设备
  void onCloseDevice();

  // 开始传输
  void onStartTransfer();

  // 重置
  void onReset();

  // 硬件同步使能
  void onHardwareSyncEnable();

  // 硬件同步打开
  void onHardwareSyncOpen();

  // 选择文件夹
  void onSelectFolder();

  // 删除所有文件
  void onDeleteAllFiles();

  // 删除选中文件
  void onDeleteSelectedFile();

  // 上移选中文件
  void onMoveUpSelectedFile();

  // 下移选中文件
  void onMoveDownSelectedFile();

  // 定时更新
  void onTimerUpdate();

  void onSendModeChanged(); // 添加发送模式改变的槽函数
  void onInputSourceChanged();

private:
  Ui::MainWindow *ui;

  // 初始化界面
  void initUI();

  void updateInputSourceLabel();

  // 扫描设备并更新设备列表
  bool scanDevices();
  void updateDevicesList(unsigned int uiDeviceNum);

  // 更新文件列表
  void updateFileList();
  void findAllFiles(const QString &strDir, const QString &strFormat);
  void updateAllFileListWidget(const QString &strAdd);

  // 选择文件夹对话框
  bool selectFolderDialog(QString &folderPath);
  void getFilelist();

  // 开始/停止发送数据
  bool startSendData();
  void stopSendData();

private:
  // PCIe设备
  PcieFacade *m_pPcieDevice;
  int m_iDeviceIndex;
  std::vector<std::string> m_deviceInfoList; // 设备信息列表
  int m_iWorkMode;                           // 工作模式
  int m_iSendMode;                           // 发送模式

  // 发送相关
  unsigned int m_uiSendBufferSize;
  QAtomicInt m_bStopSendData;

  // 互斥量
  QMutex m_mutex;

  // 定时器
  QTimer *m_pTimer;

  // 路径相关
  QString m_sAppFolderPath;
  QString m_sFileFolderPath;
  int m_iFileCount;

  // 输入输出接口
  InputDataReader *m_pInputSendData;
  InputDataReaderInitParam m_InputDataFromFileUseIOCP_InitParam;
  // 内存生成输入（测试）
  CInputDataFromMemory *m_pMemoryInput;
  // 共享内存输入（SHM）
  CInputDataFromSHM *m_pShmInput;
  // 当前用于发送的缓冲池（若输入端提供）
  std::shared_ptr<AlignedBufferPool> m_pSendBufferPool;

  // 硬件同步
  bool m_bHardwareSyncEnable;
  bool m_bHardwareSyncOpen;

  // 配置
  Setting m_Setting;
  void loadSetting();
  void saveSetting();
};

#endif // MAINWINDOW_H
