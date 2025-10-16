#include "mainwindow.h"
#ifdef _WIN32
#include "FileHandleManager.h"
#include <cstdint>
#endif
#include "ui_mainwindow.h"
#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QFileInfo>
#include <QIcon>
#include <QVBoxLayout>
#include <algorithm>
#include <fstream>

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent), ui(new Ui::MainWindow), m_iWorkMode(0),
      m_iSendMode(0), m_uiSendBufferSize(64 * 1024 * 1024), m_bStopSendData(0),
      m_bHardwareSyncOpen(false), m_bHardwareSyncEnable(false) {
  ui->setupUi(this);

  // 设置应用程序图标
  setWindowIcon(QIcon(":/icons/pcie_demo.ico"));

  // 创建PCIe设备
  m_iDeviceIndex = 0;
  m_pPcieDevice = new PcieFacade();

  // 创建输入接口
  m_pInputSendData = new InputDataReader();
  m_pInputSendData->SetInputDataListener(this);

  // 创建内存生成输入（测试）
  m_pMemoryInput = new CInputDataFromMemory();
  m_pMemoryInput->SetInputDataListener(this);

  // 初始化参数

  // 获取应用程序路径
  m_sAppFolderPath = QCoreApplication::applicationDirPath() + "/";

  // 初始化界面
  initUI();
}

MainWindow::~MainWindow() {
  // 停止所有线程
  stopSendData();

  saveSetting();

  // 关闭设备
  if (m_pPcieDevice) {
    m_pPcieDevice->CloseDevice();
    delete m_pPcieDevice;
  }

  // 销毁输入接口
  if (m_pInputSendData) {
    delete m_pInputSendData;
  }
  if (m_pMemoryInput) {
    delete m_pMemoryInput;
  }

  delete ui;
}

void MainWindow::initUI() {
  // 设置列表控件行高
  ui->listFiles->setRowHeight(18);

  // 连接信号和槽
  connect(ui->btnRefreshDevices, &QPushButton::clicked, this,
          &MainWindow::onRefreshDevices);
  connect(ui->comboDevices, QOverload<int>::of(&QComboBox::currentIndexChanged),
          this, &MainWindow::onDeviceChanged);
  connect(ui->btnOpen, &QPushButton::clicked, this, &MainWindow::onOpenDevice);
  connect(ui->btnClose, &QPushButton::clicked, this,
          &MainWindow::onCloseDevice);
  connect(ui->btnStart, &QPushButton::clicked, this,
          &MainWindow::onStartTransfer);
  connect(ui->btnReset, &QPushButton::clicked, this, &MainWindow::onReset);
  connect(ui->btnHardwareSyncOpen, &QPushButton::clicked, this,
          &MainWindow::onHardwareSyncOpen);
  connect(ui->btnHardwareSyncEnable, &QPushButton::clicked, this,
          &MainWindow::onHardwareSyncEnable);
  connect(ui->btnFolderSelect, &QPushButton::clicked, this,
          &MainWindow::onSelectFolder);
  connect(ui->btnDeleteAllFiles, &QPushButton::clicked, this,
          &MainWindow::onDeleteAllFiles);
  connect(ui->btnDeleteSelectedFile, &QPushButton::clicked, this,
          &MainWindow::onDeleteSelectedFile);
  connect(ui->btnUpSelectFile, &QPushButton::clicked, this,
          &MainWindow::onMoveUpSelectedFile);
  connect(ui->btnDownSelectFile, &QPushButton::clicked, this,
          &MainWindow::onMoveDownSelectedFile);
  connect(ui->radioSendMode1, &QRadioButton::toggled, this,
          &MainWindow::onSendModeChanged);
  connect(ui->radioSendMode2, &QRadioButton::toggled, this,
          &MainWindow::onSendModeChanged);
  connect(ui->chkUseMemorySource, &QCheckBox::toggled, this,
    &MainWindow::onInputSourceChanged);

  loadSetting();

  // 设置初始窗口标题
  setWindowTitle(QString::fromStdString(m_Setting.AppName));

  // 设置定时器
  m_pTimer = new QTimer(this);
  connect(m_pTimer, &QTimer::timeout, this, &MainWindow::onTimerUpdate);
  m_pTimer->start(1000);

  // 默认选择发送模式1
  ui->radioSendMode1->setChecked(true);
  m_iSendMode = 0; // 模式1对应值为0

  // 初始化输入源标签
  updateInputSourceLabel();
  // 扫描设备并更新设备列表
  scanDevices();
}

void MainWindow::onInputSourceChanged() { updateInputSourceLabel(); }

void MainWindow::updateInputSourceLabel() {
  if (!ui) return;
  bool use_memory = ui->chkUseMemorySource->isChecked();
  if (use_memory) {
    ui->labelInputSource->setText(tr("输入源：内存"));
    statusBar()->showMessage(tr("输入源：内存生成（测试）"));
  } else {
    ui->labelInputSource->setText(tr("输入源：文件"));
    statusBar()->showMessage(tr("输入源：文件（从目录读取）"));
  }
}

bool MainWindow::scanDevices() {
  // 清空设备列表
  m_deviceInfoList.clear();
  ui->comboDevices->clear();

  // 扫描设备
  unsigned int uiDeviceNum = 0;
  if (!m_pPcieDevice->ScanDevice(uiDeviceNum)) {
    ShowErrorMessage(std::string("未找到PCIe设备！"));
    return false;
  }

  // 更新设备列表
  updateDevicesList(uiDeviceNum);

  return (uiDeviceNum > 0);
}

void MainWindow::updateDevicesList(unsigned int uiDeviceNum) {
  ui->comboDevices->clear();
  m_deviceInfoList.clear();

  if (uiDeviceNum == 0) {
    return;
  }

  // 添加设备信息到列表
  for (unsigned int i = 0; i < uiDeviceNum; ++i) {
    std::string deviceInfo;
    m_pPcieDevice->GetDeviceInfo(i, deviceInfo);
  m_deviceInfoList.push_back(deviceInfo);
  ui->comboDevices->addItem(
    QString("device %1: %2").arg(i).arg(QString::fromStdString(deviceInfo)));
  }

  // 默认选中第一个设备
  if (uiDeviceNum > 0 && ui->comboDevices->count() > 0) {
    ui->comboDevices->setCurrentIndex(0);
  }
}

void MainWindow::onRefreshDevices() { scanDevices(); }

void MainWindow::onDeviceChanged(int index) {
  if (index >= 0 && index < static_cast<int>(m_deviceInfoList.size())) {
    m_iDeviceIndex = index;
    // 更新窗口标题以显示当前设备
    setWindowTitle(QString::fromStdString(m_Setting.AppName) +
                   QString(" —— 板卡编号：%1").arg(m_iDeviceIndex));
  }
}

void MainWindow::updateFileList() {
  ui->listFiles->clear();
  m_iFileCount = 0;

  // 查找所有文件
  findAllFiles(m_sFileFolderPath, "*.dat");
}

void MainWindow::findAllFiles(const QString &strDir, const QString &strFormat) {
  QDir dir(strDir);
  QStringList fileList =
      dir.entryList(QStringList() << strFormat, QDir::Files, QDir::Name);

  for (const QString &filename : fileList) {
    updateAllFileListWidget(filename);
  }
}

void MainWindow::updateAllFileListWidget(const QString &strAdd) {
  ui->listFiles->addItem(strAdd);
  m_iFileCount++;
}

bool MainWindow::selectFolderDialog(QString &folderPath) {
  QString dir = QFileDialog::getExistingDirectory(
      this, tr("选择文件夹"), folderPath,
      QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks);
  if (dir.isEmpty()) {
    return false;
  }

  folderPath = dir + "/";
  return true;
}

bool MainWindow::startSendData() {
  // 根据 UI 选择决定使用文件输入还是内存测试输入
  bool use_memory = ui->chkUseMemorySource->isChecked();

  if (use_memory) {
    // 初始化内存输入参数
    InputDataFromMemoryInitParam memParam;
    memParam.loop = (m_iSendMode == 1);
    memParam.block_size = std::max(1u, m_Setting.FileBlockSize) * 1024;
    memParam.io_request_num = std::max(1u, m_Setting.IoRequestNum);
    memParam.cpu_id = static_cast<int>(m_Setting.DiskCpuId);

    if (!m_pMemoryInput->Init(&memParam)) {
      LOG_ERROR("内存输入 Init 失败");
      return false;
    }

#ifndef _WIN32
  auto pool = m_pMemoryInput->GetBufferPool();
  m_pSendBufferPool = pool;
  m_pPcieDevice->SetBufferPool(pool);
#endif

    if (!m_pMemoryInput->Start()) {
      LOG_ERROR("内存输入 Start 失败");
      return false;
    }

    return true;
  } else {
    InputDataReaderInitParam &param = m_InputDataFromFileUseIOCP_InitParam;

    // 初始化参数
    param.loop = (m_iSendMode == 1); // 根据发送模式设置是否循环
    param.block_size = std::max(1u, m_Setting.FileBlockSize) * 1024;
    param.io_request_num = std::max(1u, m_Setting.IoRequestNum);
    param.cpu_id = m_Setting.DiskCpuId;

    // 初始化输入数据接口
    if (!m_pInputSendData->Init(&param)) {
      // ShowErrorMessage(m_pInputSendData->GetErrorInfo());
      return false;
    }

#ifndef _WIN32
  auto pool = m_pInputSendData->GetBufferPool();
  m_pSendBufferPool = pool;
  m_pPcieDevice->SetBufferPool(pool);
#endif

    // 启动数据输入
    if (!m_pInputSendData->Start()) {
      // ShowErrorMessage(m_pInputSendData->GetErrorInfo());
      return false;
    }

    return true;
  }
}

void MainWindow::stopSendData() {
  m_bStopSendData = 1;
  // 停止两种输入源中的任意一个
  if (m_pInputSendData)
    m_pInputSendData->Stop();
  if (m_pMemoryInput)
    m_pMemoryInput->Stop();
  m_pPcieDevice->Stop();
#ifndef _WIN32
  m_pPcieDevice->SetBufferPool(nullptr);
  m_pSendBufferPool.reset();
#endif
}

void MainWindow::ShowErrorMessage(const std::string &sErrorInfo) {
  QMessageBox::warning(this, tr("错误"), QString::fromStdString(sErrorInfo));
  LOG_ERROR("%s", sErrorInfo.c_str());
}

void MainWindow::InputData(unsigned char *pData, std::uint64_t uiDataLength,
                           void *pParam) {
  // 直接发送数据
  while (!m_pPcieDevice->Send(pData, uiDataLength)) {
    if (m_bStopSendData)
      break;
  }

  // 如果发送成功并且数据来自缓冲池，则释放该块
  if (m_pSendBufferPool) {
    // AlignedBufferPool::Owns(ptr) 判断该指针是否属于池
    try {
      if (m_pSendBufferPool->Owns(pData)) {
        m_pSendBufferPool->Release(pData);
      }
    } catch (...) {
      // 不要让释放错误影响主流程，记录并继续
      LOG_WARN("释放发送缓冲失败或该缓冲不属于池");
    }
  }
}

void MainWindow::onOpenDevice() {
  // 获取参数
  unsigned int uiSendDMASize = ui->editSendDMASize->text().toUInt() * 1024;
  unsigned int uiSendBufferSize =
      ui->editSendBufferSize->text().toUInt() * 1024 * 1024;

  // 设置工作模式
  m_iWorkMode = 0; // 固定为发送模式
  PcieDeviceWorkMode workMode = PcieDeviceWorkMode::SendMode;

  // 打开设备
  if (!m_pPcieDevice->OpenDevice(m_iDeviceIndex, workMode, uiSendDMASize,
                                 uiSendBufferSize, m_Setting.PcieCpuId)) {
    ShowErrorMessage(std::string("打开设备失败！"));
    return;
  }

  // 设置速率
  m_pPcieDevice->SetSpeed(ui->comboSpeed->currentIndex());

  // 更新界面状态
  ui->comboDevices->setEnabled(false);
  ui->btnRefreshDevices->setEnabled(false);
  ui->btnOpen->setEnabled(false);
  ui->btnClose->setEnabled(true);
  ui->btnStart->setEnabled(true);
  ui->btnReset->setEnabled(true);
  ui->btnHardwareSyncEnable->setEnabled(true);
  ui->radioSendMode1->setEnabled(false);
  ui->radioSendMode2->setEnabled(false);
  ui->editSendDMASize->setEnabled(false);
  ui->editSendBufferSize->setEnabled(false);
  // 打开设备后禁止切换测试模式
  ui->chkUseMemorySource->setEnabled(false);
}

void MainWindow::onCloseDevice() {
  if (m_pPcieDevice) {
    // 停止发送
    stopSendData();

    // 关闭设备
    m_pPcieDevice->CloseDevice();
  }

  // 更新界面状态
  ui->comboDevices->setEnabled(true);
  ui->btnRefreshDevices->setEnabled(true);
  ui->btnOpen->setEnabled(true);
  ui->btnClose->setEnabled(false);
  ui->btnStart->setEnabled(false);
  ui->btnReset->setEnabled(false);
  ui->btnHardwareSyncEnable->setEnabled(false);
  ui->btnHardwareSyncOpen->setEnabled(false);
  ui->radioSendMode1->setEnabled(true);
  ui->radioSendMode2->setEnabled(true);
  ui->editSendDMASize->setEnabled(true);
  ui->btnStart->setText(tr("开始"));
  // 关闭设备后允许切换测试模式
  ui->chkUseMemorySource->setEnabled(true);
  // 清理发送缓冲池引用
  m_pSendBufferPool.reset();
}

void MainWindow::onStartTransfer() {
  if (!m_pPcieDevice) {
    return;
  }

  if (!m_pPcieDevice->IsRunning()) {
    bool use_memory = ui->chkUseMemorySource->isChecked();

    // 如果不是内存测试模式，要求文件列表非空并准备文件列表
    if (!use_memory) {
      if (ui->listFiles->count() == 0) {
        ShowErrorMessage("待发送文件列表为空！");
        return;
      }
      getFilelist();
    }

    // 重置设备
    m_pPcieDevice->Reset();
    // 重置可能的输入源
    m_pInputSendData->Reset();
    if (m_pMemoryInput)
      m_pMemoryInput->Reset();

    // 重置停止标志
    m_bStopSendData = 0;

    // 启动设备
    if (!m_pPcieDevice->Start()) {
      ShowErrorMessage("启动设备失败！");
      return;
    }

#ifndef PCIE_SPEED_TEST
    // 启动数据发送
    if (!startSendData()) {
      m_pPcieDevice->Stop();
      return;
    }
#endif

    // 更新按钮文本
    ui->btnStart->setText(tr("停止"));

    // 禁用复位按钮
    ui->btnReset->setEnabled(false);
  } else {
    ui->btnStart->setEnabled(false);

    // 停止设备和数据发送
    stopSendData();
  }
}

void MainWindow::onReset() {
  if (m_pPcieDevice) {
    m_pPcieDevice->Reset();
  }
}

void MainWindow::onHardwareSyncEnable() {
  if (!m_pPcieDevice) {
    return;
  }

  m_bHardwareSyncEnable = !m_bHardwareSyncEnable;
  m_pPcieDevice->HardwareSyncEnable(m_bHardwareSyncEnable);
  ui->btnHardwareSyncEnable->setText(
      m_bHardwareSyncEnable ? tr("退出同步模式") : tr("进入同步模式"));
  ui->btnHardwareSyncOpen->setEnabled(m_bHardwareSyncEnable);
}

void MainWindow::onHardwareSyncOpen() {
  if (!m_pPcieDevice) {
    return;
  }

  m_bHardwareSyncOpen = !m_bHardwareSyncOpen;
  m_pPcieDevice->HardwareSyncOpen(m_bHardwareSyncOpen);
  ui->btnHardwareSyncOpen->setText(m_bHardwareSyncOpen ? tr("关闭")
                                                       : tr("打开"));
}

void MainWindow::onSelectFolder() {
  if (!selectFolderDialog(m_sFileFolderPath)) {
    return;
  }

  // 更新文件列表
  updateFileList();
}

void MainWindow::onDeleteAllFiles() {
  ui->listFiles->clearAll();
  m_iFileCount = 0;
}

void MainWindow::onDeleteSelectedFile() { ui->listFiles->deleteSelectedItem(); }

void MainWindow::onMoveUpSelectedFile() { ui->listFiles->moveSelectedItemUp(); }

void MainWindow::onMoveDownSelectedFile() {
  ui->listFiles->moveSelectedItemDown();
}

void MainWindow::onTimerUpdate() {
  if (m_pPcieDevice && m_pPcieDevice->IsDeviceOpen()) {
    // 获取设备状态
    DmaChannelStatus status = m_pPcieDevice->GetStatus();

    // 构建状态栏信息
    QString statusMsg;

    // 添加工作状态
    switch (status.threadState) {
    case ThreadState::Running:
      statusMsg += tr("发送状态：运行中 | ");
      break;
    case ThreadState::Stopped:
      statusMsg += tr("发送状态：已停止 | ");
      break;
    case ThreadState::Error:
      statusMsg += tr("发送状态：错误 | ");
      break;
    default:
      statusMsg += tr("发送状态：未启动 | ");
    }

    // 添加发送速度
    float speedInMB =
        static_cast<float>(status.currentSpeed) / (1024.0f * 1024.0f);
    statusMsg += tr("速度：%1 MB/s | ").arg(speedInMB, 0, 'f', 2);

    // 更新速率图表数据
    ui->widgetOscope->updateSpeed(speedInMB);

    // 添加总发送量
    float totalInMB =
        static_cast<float>(status.totalBytesSent) / (1024.0f * 1024.0f);
    statusMsg += tr("总发送：%1 MB | ").arg(totalInMB, 0, 'f', 2);

    // 添加队列使用情况
    statusMsg +=
        tr("队列使用率：%1% | ").arg(status.queueUsagePercent, 0, 'f', 1);

    // 添加发送步骤
    statusMsg += tr("发送步骤：%1 | ").arg(status.step);

    // 添加读取状态
    statusMsg += tr("读取状态：%1 | ")
                     .arg(m_pInputSendData->IsRunning() ? "运行中" : "已停止");

    // 添加读取句柄数
#ifdef _WIN32
    statusMsg += tr("读取句柄数：%1 | ")
                     .arg(FileHandleManager::GetInstance().GetHandleCount());
#endif

    // 添加读取步骤
    statusMsg += tr("读取步骤：%1").arg(m_pInputSendData->GetStep());

    // 更新状态栏
    statusBar()->showMessage(statusMsg);

    if (m_bStopSendData && status.threadState == ThreadState::Stopped) {
      ui->btnStart->setEnabled(true);
      ui->btnStart->setText(tr("开始"));
      ui->btnReset->setEnabled(true);
    }

    // 单次发送模式下，如果发送状态为运行中，且读取文件数等于发送字节数，则自动停止
    if (m_iSendMode == 0 && status.threadState == ThreadState::Running &&
        !m_pInputSendData->IsRunning() && status.totalBytesSent > 0) {
      if (m_pInputSendData->GetInputDataNum() == status.totalBytesSent) {
        stopSendData();
      }
    }
  }
}

void MainWindow::getFilelist() {
  m_InputDataFromFileUseIOCP_InitParam.file_list.clear();

  for (int i = 0; i < ui->listFiles->count(); i++) {
    QListWidgetItem *item = ui->listFiles->item(i);
    QString file_path =
        QDir::toNativeSeparators(m_sFileFolderPath + item->text());
    m_InputDataFromFileUseIOCP_InitParam.file_list.push_back(
        file_path.toLocal8Bit().constData());
  }
}

void MainWindow::onSendModeChanged() {
  // 根据选中的单选按钮设置发送模式
  if (ui->radioSendMode1->isChecked()) {
    m_iSendMode = 0; // 单次发送模式
  } else if (ui->radioSendMode2->isChecked()) {
    m_iSendMode = 1; // 循环发送模式
  }
}

void MainWindow::loadSetting() {
  try {
    QString config_dir = QDir::cleanPath(m_sAppFolderPath + "config");
    QString setting_path = config_dir + QDir::separator() + "config.json";
    std::string setting_path_str = setting_path.toLocal8Bit().constData();
    std::ifstream ifs(setting_path_str);
    std::string content((std::istreambuf_iterator<char>(ifs)),
                        std::istreambuf_iterator<char>());
    JS::ParseContext js_context(content);
    JS::Error error = js_context.parseTo(m_Setting);
    m_sFileFolderPath = m_Setting.FolderPath.c_str();
    if (error != JS::Error::NoError) {
      ShowErrorMessage(std::string("加载配置文件失败！"));
      return;
    }
  } catch (const std::exception &e) {
    ShowErrorMessage(e.what());
  }
}

void MainWindow::saveSetting() {
  try {
    m_Setting.FolderPath = m_sFileFolderPath.toStdString();
    QString config_dir = QDir::cleanPath(m_sAppFolderPath + "config");
    QDir().mkpath(config_dir);
    QString setting_path = config_dir + QDir::separator() + "config.json";
    std::string setting_path_str = setting_path.toLocal8Bit().constData();
    std::string pretty_json = JS::serializeStruct(m_Setting);
    std::ofstream ofs(setting_path_str, std::ios::binary);
    ofs << pretty_json;
    ofs.flush();
    ofs.close();
  } catch (const std::exception &e) {
    ShowErrorMessage(e.what());
  }
}
