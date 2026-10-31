#pragma once
#include "core/types.h"
#include <QComboBox>
#include <QDialog>
#include <QDoubleSpinBox>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSlider>
#include <QSpinBox>
#include <QTabWidget>
namespace df {
class SettingsDialog : public QDialog {
    Q_OBJECT
  public:
    explicit SettingsDialog(const QString &root, QWidget *parent = nullptr);
    View view() const;
    Alignment alignment() const;
    QJsonObject captureSettings() const;
    void setView(View value);
    void setSimulationEnabled(bool enabled);
    void setCaptureLocked(bool locked);
    void openPage(int index = -1);
    QTabWidget *tabs;
    QLineEdit *sourceDirectory, *datasetDirectory, *recordDirectory;
    QComboBox *sourceMode, *offlineQuality, *resolution, *encoder, *bitrate, *exposure, *iso, *shutter;
    QSpinBox *seconds, *blend;
    QDoubleSpinBox *ev;
    QSlider *yaw, *pitch, *hfov;
    QDoubleSpinBox *step, *sensitivity;
    std::array<QDoubleSpinBox *, 3> rear;
    QLabel *simulationStatus, *exposureActual;
    QPushButton *loadButton, *resetButton;
    std::array<QPushButton *, 9> directions;
  signals:
    void loadRequested();
    void viewChanged();
    void alignmentChanged();
    void directionRequested(int index);

  private:
    QString root;
    std::array<QLabel *, 3> angleLabels;
    QPushButton *recordBrowse, *dataBrowse, *sourceBrowse;
    bool captureLocked = false;
    void refreshAngles();
    void refreshExposure();
    void restore();
};
} // namespace df
