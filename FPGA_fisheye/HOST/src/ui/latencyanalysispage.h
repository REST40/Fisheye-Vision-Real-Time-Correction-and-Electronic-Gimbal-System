#pragma once
#include "fpga/network/latencytracker.h"
#include <QWidget>
class QLabel;
class QTableWidget;
class QComboBox;
class QPushButton;
class QCheckBox;
class LatencyPlot;
class LatencyAnalysisPage final : public QWidget {
    Q_OBJECT
  public:
    explicit LatencyAnalysisPage(LatencyTracker *tracker, QWidget *parent = nullptr);
    void refresh();
  signals:
    void backRequested();

  private:
    LatencyTracker *tracker;
    LatencySnapshot current;
    QLabel *cards[3];
    QLabel *status;
    QLabel *rates;
    QLabel *details;
    QTableWidget *tables[3];
    QComboBox *duration;
    QComboBox *metric;
    QComboBox *frame;
    QCheckBox *formal;
    QPushButton *startButton;
    QPushButton *stopButton;
    LatencyPlot *plot;
    LatencyPlot *timeline;
    void showFrame();
};
