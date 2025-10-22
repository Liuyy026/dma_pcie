#include "../common/utils/Logger.h"
#include "mainwindow.h"
#include <QApplication>
#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QLocale>
#include <QTranslator>

int main(int argc, char *argv[]) {
  QApplication app(argc, argv);

  // 设置应用名称和组织信息
  QApplication::setApplicationName("PCIe数据发送软件");
  QApplication::setOrganizationName("GKWX");

  // 初始化日志系统，统一使用程序目录下的 logs 文件夹
  QString logDir = QDir::cleanPath(QCoreApplication::applicationDirPath() + "/logs");
  CLogger::GetInstance().Initialize(logDir.toStdString(), LogLevel::Debug);
  CLogger::GetInstance().SetModeTag("general");

  // 确保在程序退出时清理日志系统
  QObject::connect(&app, &QApplication::aboutToQuit,
                   []() { CLogger::GetInstance().Cleanup(); });

  // 加载中文翻译
  QTranslator translator;
  if (translator.load(QLocale::system(), "pcie_demo_qt", "_",
                      QDir::currentPath() + "/translations")) {
    app.installTranslator(&translator);
  }

  // 创建并显示主窗口
  MainWindow mainWindow;
  mainWindow.show();

  return app.exec();
}