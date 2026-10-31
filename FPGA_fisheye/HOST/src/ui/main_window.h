#pragma once
#include "core/types.h"
#include <QElapsedTimer>
#include <QLabel>
#include <QListWidget>
#include <QMainWindow>
#include <QPushButton>
#include <QSlider>
#include <QStackedWidget>
#include <QTimer>
#include <deque>
#include <optional>
class QThread;
class BoardVideoService;
class FrameRatePage;
class ImageQualityDialog;
class QDialog;
class QGridLayout;
class QSpinBox;
namespace df {
class SettingsDialog;
class ViewPanel;
class SimulationService;
class CaptureService;
class FpgaLink;
class MainWindow : public QMainWindow {
    Q_OBJECT
  public:
    explicit MainWindow(QString root, bool autoplay = true, QWidget *parent = nullptr);
    ~MainWindow() override;
    SettingsDialog *settings;
    std::array<ViewPanel *, 4> panels;
    void loadSimulation();
    void loadOfflineVideo();
    void exitSimulation();
    void captureEvent(const QJsonObject &data);
    bool simulationActive() const {
        return simActive;
    }
    std::optional<FramePacket> displayedFrame() const {
        return displayed;
    }
    void saveScreenshot(const QString &path);

  protected:
    void closeEvent(QCloseEvent *) override;

  private:
    ImageQualityDialog *qualityDialog = nullptr;
    bool qualityActive = false;
    bool qualityOfflineWasPlaying = false;
    bool qualityHostPsBusy = false, qualityPipelineBusy = false, qualityReturnBusy = false;
    QPushButton *qualityButton;
    QString root, lastSaved, sessionPath, phase = "idle", pending;
    QString lastSnapshot, lastClip, lastDataset, operationMessage;
    bool offlineBoard = false;
    double boardReturnFps = 0;
    bool simActive = false, closing = false, snapshotPending = false, lensRequested = false,
         exportPending = false;
    quint64 generation = 0;
    QJsonObject live;
    std::optional<FramePacket> displayed;
    std::deque<double> frameTimes;
    SimulationService *simulation;
    CaptureService *capture;
    BoardVideoService *board;
    QThread *verifier = nullptr;
    QTimer timer, cameraPoll;
    QElapsedTimer clock, operationClock, snapshotClock, lensClock;
    QStackedWidget *pages;
    QListWidget *library;
    QPushButton *linkAnalysisButton, *latencyAnalysisButton;
    QDialog *linkDialog, *latencyDialog;
    FrameRatePage *linkPage;
    QElapsedTimer cameraRateClock, cameraSampleClock;
    quint64 cameraDelivered = 0, lastCameraSequence = 0, cameraSampleFirst = 0,
            cameraSampleLast = 0;
    qint64 cameraSampleFirstTimestamp = 0, cameraSampleLastTimestamp = 0;
    bool cameraSampleActive = false;
    void setupAnalysis();
    void cameraInputTest(bool running);
    QLabel *libraryStatus = nullptr;
    std::array<QLabel *, 3> metrics, captions;
    QPushButton *connectButton, *recordButton, *snapshotButton, *datasetButton, *pauseButton,
        *exitButton, *exportButton;
    QSlider *seek;
    void buildUi();
    void setPresentationStep(int step);
    void advancePresentation(int delta);
    QGridLayout *viewGrid = nullptr;
    QWidget *presentationControls = nullptr;
    QWidget *statisticsBar = nullptr;
    QLabel *presentationTitle = nullptr;
    QPushButton *presentationButton = nullptr, *presentationPrevious = nullptr,
                *presentationNext = nullptr, *presentationAuto = nullptr;
    QSpinBox *presentationSeconds = nullptr;
    QTimer presentationTimer;
    int presentationStep = -1;
    void loadSource(bool boardInput);
    void tick();
    void pollCameraRaw();
    void refresh();
    void message(const QString &text, bool error = false);
    void render(const FramePacket &packet);
    void viewChanged();
    void drag(double dx, double dy);
    void direction(int index);
    void connectCamera();
    void record();
    void snapshot();
    void dataset();
    void send(const QString &command);
    void loadLibrary();
    void openSelected(const QString &name);
    void showLogs();
    void verifySelected();
    void showParameters(const QString &directory = {});
};
} // namespace df
