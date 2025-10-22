#include "speedchartwidget.h"
#include <QDateTime>
#include <QPainter>
#include <QVBoxLayout>

SpeedChartWidget::SpeedChartWidget(QWidget *parent)
    : QWidget(parent), m_dataPointCounter(0) {
  initChart();
}

SpeedChartWidget::~SpeedChartWidget() {
  // Qt父子对象系统会自动删除子对象
}

void SpeedChartWidget::initChart() {
  // 创建图表
  m_pChart = new QChart();
  //   m_pChart->setTitle("数据发送速率");
  m_pChart->setAnimationOptions(QChart::NoAnimation);
  // 去除图表边框空白的关键设置
  m_pChart->setMargins(QMargins(0, 0, 0, 0)); // 设置图表边距为0
  //   m_pChart->setBackgroundVisible(false);      // 可选：隐藏背景
  //   m_pChart->setPlotAreaBackgroundVisible(false); // 可选：隐藏绘图区域背景

  // 创建数据系列
  m_pSpeedSeries = new QLineSeries();
  //   m_pSpeedSeries->setName("发送速率 (MB/s)");
  m_pChart->addSeries(m_pSpeedSeries);

  // 创建X轴（时间）
  m_pAxisX = new QValueAxis();
  //   m_pAxisX->setTitleText("时间 (秒)");
  m_pAxisX->setRange(0, MAX_DATA_POINTS - 1);
  m_pAxisX->setTickCount(10); // 显示的刻度数量
  m_pAxisX->setLabelFormat("%d");

  // 创建Y轴（速率）
  m_pAxisY = new QValueAxis();
  //   m_pAxisY->setTitleText("速率 (MB/s)");
  m_pAxisY->setRange(0, 100); // 初始范围设为0-100MB/s，后续可动态调整
  m_pAxisY->setLabelFormat("%.1f");

  // 添加坐标轴到图表
  m_pChart->addAxis(m_pAxisX, Qt::AlignBottom);
  m_pChart->addAxis(m_pAxisY, Qt::AlignLeft);
  m_pSpeedSeries->attachAxis(m_pAxisX);
  m_pSpeedSeries->attachAxis(m_pAxisY);

  // 设置图表样式
  m_pChart->legend()->hide();
  m_pChart->legend()->setAlignment(Qt::AlignBottom);

  // 优化坐标轴，减少占用空间
  m_pAxisX->setLabelsVisible(false); // 隐藏X轴标签
  m_pAxisY->setLabelsVisible(true);  // 保留Y轴标签显示速率

  // 创建图表视图并添加到布局
  m_pChartView = new QChartView(m_pChart);
  m_pChartView->setRenderHint(QPainter::Antialiasing);
  m_pChartView->setContentsMargins(0, 0, 0, 0);

  // 添加到布局
  QVBoxLayout *layout = new QVBoxLayout(this);
  layout->setContentsMargins(0, 0, 0, 0);
  layout->addWidget(m_pChartView);
  setLayout(layout);

  // 初始化数据点
  m_speedData.clear();
  for (int i = 0; i < MAX_DATA_POINTS; ++i) {
    m_speedData.append(0.0f);
    m_pSpeedSeries->append(i, 0);
  }
}

void SpeedChartWidget::updateSpeed(float currentSpeed) {
  // 更新数据集合
  m_speedData.removeFirst();
  m_speedData.append(currentSpeed);

  // 更新图表数据
  m_pSpeedSeries->clear();

  // 动态调整Y轴范围
  float maxSpeed = 0.0f;
  for (int i = 0; i < m_speedData.size(); ++i) {
    m_pSpeedSeries->append(i, m_speedData[i]);
    if (m_speedData[i] > maxSpeed) {
      maxSpeed = m_speedData[i];
    }
  }

  // 设置Y轴最大值（比当前最大速率高20%，但最小不低于100）
  float yMax = qMax(100.0f, maxSpeed * 1.2f);
  if (m_pAxisY->max() != yMax) {
    m_pAxisY->setRange(0, yMax);
  }

  // 计数器递增
  m_dataPointCounter++;

  // 更新X轴标签（每5秒）
  //   if (m_dataPointCounter % 5 == 0) {
  //     QDateTime currentTime = QDateTime::currentDateTime();
  //     m_pAxisX->setTitleText(QString("时间（最近%1秒）- %2")
  //                                .arg(MAX_DATA_POINTS)
  //                                .arg(currentTime.toString("hh:mm:ss")));
  //   }
}