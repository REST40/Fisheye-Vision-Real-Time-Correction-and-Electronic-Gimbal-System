#pragma once
#include "boardconnection.h"
#include "hostdecodecapacity.h"
#include "pspipelineratemeter.h"
#include <QThread>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>

// UI publishes/takes bounded mailboxes; network and codec never run on the UI thread.
class BoardVideoService final : public QThread {
    Q_OBJECT
  public:
    explicit BoardVideoService(QObject *parent = nullptr);
    ~BoardVideoService() override;
    void connectToBoard(const BoardEndpoints &endpoints = {});
    void disconnectFromBoard();
    bool isConnected() const;
    bool isConnecting() const;
    void requestPose(double pan, double tilt, double hfov);
    void setPaused(bool paused);
    bool enterQualityMode();
    void leaveQualityMode();
    bool qualityCanCapture() const;
    BoardEndpoints qualityEndpoints() const;
    void submit(const QImage &fullFrame, qint64 cameraTimestampMs = 0);
    void submitJpeg(const QByteArray &jpeg, qint64 capturedMs);
    void startHostPsRateTest();
    void stopHostPsRateTest();
    void startPsPipelineRateTest();
    void stopPsPipelineRateTest();
    void startPsHostRateTest();
    void stopPsHostRateTest();
    struct Display {
        QImage image;
        quint32 frame = 0, epoch = 0;
        double latencyMs = 0;
        qint64 renderedAtUs = 0;
        quint64 sampleId = 0;
        qint64 renderStartedUs = 0;
    };
    Display takeDisplay();
    QImage takeSphere();
    LatencyTracker &latencyTracker() {
        return m_latency;
    }
  signals:
    void qualitySuspended();
    void qualityResumed();
    void psFlowWindow(const QString &text);
    void psStageWaitWindow(quint32 samples, double decodeMs, double correctMs, double encodeMs);
    void uploadWaitWindow(double creditMs, double activeMs, double sendingMs, double sourceMs,
                          double readyMs, double longestCreditMs, int freeSlots, int totalSlots);
    void psWaitWindow(bool supported, quint32 samples, double readyMs, double pipelineMs,
                      double outQueueMs);
    void statusChanged(const QString &message);
    void connectedChanged(bool connected);
    void poseResolved(bool accepted, double pan, double tilt, double hfov, quint32 epoch,
                      int reason);
    void poseCommandDispatched(double pan, double tilt, double hfov, qint64 atUs);
    void poseCommandQueued(bool replaced);
    void poseAcceptedTimed(double pan, double tilt, double hfov, qint64 atUs);
    void poseReadyTimed(double pan, double tilt, double hfov, qint64 atUs);
    void poseLutProfile(double pan, double tilt, double hfov, quint32 wallUs, quint32 workUs,
                        quint32 anchorUs);
    void poseResolvedTimed(bool accepted, double pan, double tilt, double hfov, quint32 epoch,
                           int reason, qint64 atUs);
    void metricsChanged(double receiveFps, quint64 sent, quint64 complete);
    void pairEncodingRatesChanged(double fps, double averageMs, quint64 replacements);
    void pipelineRatesChanged(double uploadFps, double returnFps, double decodedFps);
    void psReceiveWindowChanged(double fps, double mbps, quint64 frames, quint32 incompleteDrop,
                                quint32 crcDrop, quint32 staleDrop, quint32 noSlotDrop);
    void psPipelineWindowChanged(const PsPipelineRateResult &result);
    void normalReturnWindowChanged(double psFps, double psMBps, double hostFps, quint64 psFrames,
                                   quint64 hostFrames, quint32 supersededDrop, quint32 sendDrop);
    void stageTimesChanged(quint32 decodeUs, quint32 correctUs, quint32 encodeUs);
    void hostPsRateTestRunningChanged(bool running);
    void hostPsRateTestProgress(int elapsedSeconds, int totalSeconds);
    void hostPsRateTestCompleted(double elapsedSeconds, double hostFps, double hostMBps,
                                 double psFps, double psMBps, quint64 hostFrames, quint64 psFrames,
                                 quint64 missingFrames, quint32 incompleteDrop, quint32 crcDrop,
                                 quint32 staleDrop, quint32 noSlotDrop, quint64 hostAttempts,
                                 quint64 hostTimeouts, quint64 hostWriteFailures,
                                 quint64 hostReplacements, double hostAverageUploadMs,
                                 quint64 hostMaxUploadMs, quint64 hostWriteStalls);
    void hostPsRateTestError(const QString &reason);
    void psPipelineRateTestRunningChanged(bool running);
    void psPipelineRateTestProgress(int elapsedSeconds, int totalSeconds);
    void psPipelineRateTestCompleted(const PsPipelineRateResult &result);
    void psPipelineRateTestError(const QString &reason);
    void psHostRateTestRunningChanged(bool running);
    void psHostRateTestProgress(int elapsedSeconds, int totalSeconds);
    void psHostRateTestCompleted(double elapsedSeconds, double hostFps, double hostMBps,
                                 double psFps, double psMBps, quint64 hostFrames, quint64 psFrames,
                                 quint64 missingFrames, quint64 partialFrames, quint64 badFrames,
                                 quint32 psSendFailures);
    void hostDecodeCapacityCompleted(double elapsedSeconds, double fps, double averageMs,
                                     quint64 attempts, quint64 completed, quint64 failures,
                                     quint64 sizeErrors);
    void psHostRateTestError(const QString &reason);

  protected:
    void run() override;

  private:
    bool m_quality = false, m_qualityEnter = false, m_qualityLeave = false;
    bool m_qualityRestoreConnection = false, m_qualityRestorePaused = false;
    std::atomic_bool m_encoderBusy{false}, m_codecBusy{false};
    LatencyTracker m_latency;
    quint64 m_rawSample = 0, m_encodedSample = 0;
    mutable std::mutex m_mutex;
    std::condition_variable m_wake;
    bool m_exit = false, m_codecExit = false, m_connected = false, m_connecting = false;
    bool m_connectRequest = false, m_disconnectRequest = false, m_poseDirty = false,
         m_pauseDirty = false, m_paused = false;
    bool m_hostPsStartRequest = false, m_hostPsStopRequest = false;
    qint64 m_hostPsRequestAt = 0;
    bool m_psPipelineStartRequest = false, m_psPipelineStopRequest = false;
    bool m_psHostStartRequest = false, m_psHostStopRequest = false;
    bool m_decodeCapacityPending = false, m_decodeCapacityFinishRequest = false;
    QByteArray m_decodeCapacityJpeg;
    std::atomic_bool m_decodeCapacityCancel{false};
    qint64 m_psPipelineRequestAt = 0;
    quint64 m_generation = 0;
    quint64 m_decoded = 0;
    quint64 m_encodedFrames = 0, m_encodeWorkUs = 0, m_encodeReplacements = 0;
    BoardEndpoints m_endpoints;
    double m_pan = 0, m_tilt = 0, m_hfov = 90;
    qint64 m_poseDue = 0, m_capture = 0, m_encodedCapture = 0;
    quint32 m_epoch = 0;
    quint32 m_cameraTimestamp = 0, m_encodedCameraTimestamp = 0;
    QImage m_raw, m_sphere;
    bool m_viewDirty = false;
    QByteArray m_encoded;
    struct Output {
        QByteArray jpeg;
        quint32 frame = 0, epoch = 0;
        double latency = 0;
        quint64 sampleId = 0;
    };
    Output m_decode;
    Display m_display;
    void codecLoop();
    void encodeLoop();
};
