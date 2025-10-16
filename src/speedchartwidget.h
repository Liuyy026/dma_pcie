#ifndef SPEEDCHARTWIDGET_H
#define SPEEDCHARTWIDGET_H

#include <QDateTime>
#include <QList>
#include <QWidget>
#include <QtCharts/QChart>
#include <QtCharts/QChartView>
#include <QtCharts/QLineSeries>
#include <QtCharts/QValueAxis>

#define MAX_DATA_POINTS 60 // 保存1分钟的数据点

/**
 * @brief 实时速率曲线图表控件
 * 用于显示数据发送速率的曲线图表
 */
class SpeedChartWidget : public QWidget {
  Q_OBJECT
public:
  /**
   * @brief 构造函数
   * @param parent 父控件
   */
  explicit SpeedChartWidget(QWidget *parent = nullptr);

  /**
   * @brief 析构函数
   */
  ~SpeedChartWidget();

  /**
   * @brief 更新速率数据
   * @param currentSpeed 当前速率(MB/s)
   */
  void updateSpeed(float currentSpeed);

private:
  /**
   * @brief 初始化图表
   */
  void initChart();

private:
  QChartView *m_pChartView;    // 图表视图
  QChart *m_pChart;            // 图表
  QLineSeries *m_pSpeedSeries; // 速率曲线
  QValueAxis *m_pAxisX;        // X轴
  QValueAxis *m_pAxisY;        // Y轴
  QList<float> m_speedData;    // 速率数据列表
  int m_dataPointCounter;      // 数据点计数器
};

#endif // SPEEDCHARTWIDGET_H