#pragma once

#include "fpga/network/pspipelineratemeter.h"
#include <QWidget>
#include <array>

class QLabel;
class QTimer;
class QPushButton;

// Live observations only. A stage's per-frame latency is not its throughput capacity.
class FrameRatePage final : public QWidget {
    Q_OBJECT
  public:
    explicit FrameRatePage(QWidget *parent = nullptr);
    void setCameraRates(double inputFps, double outputFps, double transcodeMs, quint64 dropped);
    void setPsFlow(const QString &text);
    void setPsStageWait(quint32 samples, double decodeMs, double correctMs, double encodeMs);
    void setUploadWait(double creditMs, double activeMs, double sendingMs, double sourceMs,
                       double readyMs, double longestCreditMs, int freeSlots, int totalSlots);
    void setPsWait(bool supported, quint32 samples, double readyMs, double pipelineMs,
                   double outQueueMs);
    void setX5InputRate(double fps, double deliveredFps = -1);
    void setPairEncodingRate(double fps, double ms, quint64 replaced);
    void setBoardRates(double uploadFps, double returnFps, double decodedFps);
    void setPsReceiveWindow(double fps, double mbps, quint64 frames, quint32 incompleteDrop,
                            quint32 crcDrop, quint32 staleDrop, quint32 noSlotDrop);
    void setPsPipelineWindow(const PsPipelineRateResult &result);
    void setNormalReturnWindow(double psFps, double psMBps, double hostFps, quint64 psFrames,
                               quint64 hostFrames, quint32 supersededDrop, quint32 sendDrop);
    void setBoardStages(quint32 decodeUs, quint32 correctUs, quint32 encodeUs);
    void resetCamera();
    void resetBoard();
    void setCameraTestRunning(bool running);
    void setCameraInputSample(quint64 frames, double mediaSeconds, double timestampFps,
                              double deliveryFps);
    void setTranscodeCapacityCompleted(double elapsedSeconds, double fps, quint64 attempts,
                                       quint64 completed, quint64 failures, int workers);
    void setCameraTestError(const QString &reason);
    void setHostPsTestRunning(bool running);
    void setHostPsTestProgress(int elapsedSeconds, int totalSeconds);
    void setHostPsTestCompleted(double elapsedSeconds, double hostFps, double hostMBps,
                                double psFps, double psMBps, quint64 hostFrames, quint64 psFrames,
                                quint64 missingFrames, quint32 incompleteDrop, quint32 crcDrop,
                                quint32 staleDrop, quint32 noSlotDrop, quint64 hostAttempts,
                                quint64 hostTimeouts, quint64 hostWriteFailures,
                                quint64 hostReplacements, double hostAverageUploadMs,
                                quint64 hostMaxUploadMs, quint64 hostWriteStalls);
    void setHostPsTestError(const QString &reason);
    void setPsPipelineTestRunning(bool running);
    void setPsPipelineTestProgress(int elapsedSeconds, int totalSeconds);
    void setPsPipelineTestCompleted(const PsPipelineRateResult &result);
    void setPsPipelineTestError(const QString &reason);
    void setPsHostTestRunning(bool running);
    void setPsHostTestProgress(int elapsedSeconds, int totalSeconds);
    void setPsHostTestCompleted(double elapsedSeconds, double hostFps, double hostMBps,
                                double psFps, double psMBps, quint64 hostFrames, quint64 psFrames,
                                quint64 missingFrames, quint64 partialFrames, quint64 badFrames,
                                quint32 psSendFailures);
    void setHostDecodeCapacityCompleted(double elapsedSeconds, double fps, double averageMs,
                                        quint64 attempts, quint64 completed, quint64 failures,
                                        quint64 sizeErrors);
    void setPsHostTestError(const QString &reason);
  signals:
    void backRequested();
    void cameraTestRequested(bool running);
    void hostPsTestRequested(bool running);
    void psPipelineTestRequested(bool running);
    void psHostTestRequested(bool running);

  private:
    struct RateWindow {
        std::array<double, 3> values{};
        int count = 0, next = 0;
        double add(double value) {
            values[static_cast<size_t>(next)] = value;
            next = (next + 1) % 3;
            if (count < 3)
                ++count;
            double total = 0.0;
            for (int i = 0; i < count; ++i)
                total += values[static_cast<size_t>(i)];
            return total / double(count);
        }
        void clear() {
            values = {};
            count = 0;
            next = 0;
        }
    };
    QPushButton *m_cameraTestButton = nullptr;
    QPushButton *m_hostPsTestButton = nullptr;
    QPushButton *m_psPipelineTestButton = nullptr;
    QPushButton *m_psHostTestButton = nullptr;
    QLabel *m_flow = nullptr;
    QLabel *m_uploadWait = nullptr, *m_psWait = nullptr, *m_psStageWait = nullptr;
    QLabel *m_camera = nullptr;
    QLabel *m_cameraReference = nullptr;
    QLabel *m_cameraStatus = nullptr;
    QLabel *m_transcode = nullptr;
    QLabel *m_transcodeReference = nullptr;
    QLabel *m_transcodeStatus = nullptr;
    QLabel *m_upload = nullptr;
    QLabel *m_uploadReference = nullptr;
    QLabel *m_uploadStatus = nullptr;
    QLabel *m_psReceive = nullptr;
    QLabel *m_psReceiveReference = nullptr;
    QLabel *m_psReceiveStatus = nullptr;
    QLabel *m_psDecode = nullptr;
    QLabel *m_psDecodeReference = nullptr;
    QLabel *m_psDecodeStatus = nullptr;
    QLabel *m_psCorrect = nullptr;
    QLabel *m_psCorrectReference = nullptr;
    QLabel *m_psCorrectStatus = nullptr;
    QLabel *m_psEncode = nullptr;
    QLabel *m_psEncodeReference = nullptr;
    QLabel *m_psEncodeStatus = nullptr;
    QLabel *m_return = nullptr;
    QLabel *m_returnReference = nullptr;
    QLabel *m_returnStatus = nullptr;
    QLabel *m_decode = nullptr;
    QLabel *m_decodeReference = nullptr;
    QLabel *m_decodeStatus = nullptr;
    QLabel *m_summary = nullptr;
    QTimer *m_stageTimer = nullptr;
    double m_cameraFps = -1, m_transcodeFps = -1, m_uploadFps = -1, m_psReceiveFps = -1;
    double m_psDecodeFps = -1, m_psCorrectFps = -1, m_psEncodeFps = -1;
    double m_returnFps = -1, m_decodeFps = -1;
    RateWindow m_cameraWindow, m_transcodeWindow, m_uploadWindow, m_psReceiveWindow;
    RateWindow m_psDecodeWindow, m_psCorrectWindow, m_psEncodeWindow, m_returnWindow,
        m_decodeWindow;
    bool m_haveNormalReturnWindow = false, m_havePsPipelineWindow = false;
    bool m_cameraTestRunning = false;
    bool m_cameraTestHasSample = false;
    bool m_cameraTestHasResult = false;
    bool m_transcodeCapacityHasResult = false;
    bool m_hostPsTestRunning = false;
    bool m_hostPsTestHasResult = false;
    bool m_hostPsTestFailed = false;
    bool m_psPipelineTestRunning = false;
    bool m_psPipelineTestHasResult = false;
    bool m_psPipelineTestFailed = false;
    bool m_psHostTestRunning = false;
    bool m_psHostTestHasResult = false;
    void updateSummary();
};
