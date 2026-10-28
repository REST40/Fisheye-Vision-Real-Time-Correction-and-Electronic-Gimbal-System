#pragma once
#include "flowdiagnostics.h"
#include "latencytracker.h"
#include "mediawire.h"
#include "pspipelineratemeter.h"
#include "uploadwaitmeter.h"
#include <QElapsedTimer>
#include <QHostAddress>
#include <QList>
#include <QObject>
#include <QTimer>
#include <QUdpSocket>

struct BoardEndpoints {
    QHostAddress localAddress{QStringLiteral("192.168.10.100")};
    QHostAddress boardAddress{QStringLiteral("192.168.10.10")};
    quint16 controlPort = 5500, inputPort = 5501, outputPort = 5502;
    quint16 boardControlPort = 5500;
    quint16 boardInputPort = 5501, boardOutputPort = 5502;
    int helloTimeoutMs = 3000, idleTimeoutMs = 3500, retryIntervalMs = 500;
    int commandTimeoutMs = 10000;
    // Bounded UDP bursts, yielding between batches for PS RX/media scheduling.
    int uploadBatchPackets = 16;
    QString diagnosticsDirectory = QStringLiteral("output/diagnostics");
};

class BoardConnection final : public QObject {
    Q_OBJECT
  public:
    explicit BoardConnection(QObject *parent = nullptr);
    void connectToBoard(const BoardEndpoints &endpoints = {});
    void disconnectFromBoard();
    bool isConnected() const {
        return m_connected;
    }
    bool isConnecting() const {
        return m_connecting;
    }
    quint32 session() const {
        return m_session;
    }
    quint32 epoch() const {
        return m_epoch;
    }
    void requestPose(double pan, double tilt, double hfov);
    void setPaused(bool paused);
    void submitJpeg(const QByteArray &jpeg, qint64 capturedMs, quint32 cameraTimestampMs = 0,
                    quint64 sampleId = 0);
    void setLatencyTracker(LatencyTracker *tracker) {
        m_latency = tracker;
    }
    void stopAndDisconnect();
    void startPsHostRateTest();
    void stopPsHostRateTest();
    void finishPsHostDecodeCapacityTest();
    void startHostPsRateTest();
    void stopHostPsRateTest();
    void startPsPipelineRateTest();
    void stopPsPipelineRateTest();
  signals:
    void psFlowWindow(const QString &text);
    void psStageWaitWindow(quint32 samples, double decodeMs, double correctMs, double encodeMs);
    void uploadWaitWindow(double creditMs, double activeMs, double sendingMs, double sourceMs,
                          double readyMs, double longestCreditMs, int freeSlots, int totalSlots);
    void psWaitWindow(bool supported, quint32 samples, double readyMs, double pipelineMs,
                      double outQueueMs);
    void statusChanged(const QString &message);
    void connectedChanged(bool connected);
    void poseCommandSent(double pan, double tilt, double hfov);
    void poseCommandQueued(bool replaced);
    void poseAccepted(double pan, double tilt, double hfov);
    void poseReady(double pan, double tilt, double hfov);
    void poseLutProfile(double pan, double tilt, double hfov, quint32 wallUs, quint32 workUs,
                        quint32 anchorUs);
    void poseResolved(bool accepted, double pan, double tilt, double hfov, quint32 epoch,
                      int reason);
    void jpegReady(const QByteArray &jpeg, quint32 frame, quint32 epoch, double latencyMs);
    void frameSent(quint32 frame);
    void frameUploaded(quint32 frame, quint32 jpegBytes);
    void hostUploadCountersChanged(quint64 attempts, quint64 completeFrames, quint64 completeBytes,
                                   quint64 timeouts, quint64 writeStalls, quint64 writeFailures,
                                   quint64 replacements, quint64 durationMsTotal,
                                   quint64 lastDurationMs);
    void psReceiveCountersChanged(quint32 completeFrames, quint32 completeBytes,
                                  quint32 incompleteDrop, quint32 crcDrop, quint32 staleDrop,
                                  quint32 noSlotDrop);
    void psPipelineCountersChanged(quint32 decodeStarted, quint32 decodeCompleted,
                                   quint32 decodeFailed, quint32 decodeTimeout,
                                   quint32 correctStarted, quint32 correctCompleted,
                                   quint32 correctFailed, quint32 correctTimeout,
                                   quint32 encodeStarted, quint32 encodeCompleted,
                                   quint32 encodeFailed, quint32 encodeTimeout);
    void psOutputCountersChanged(quint32 completeFrames, quint32 completeBytes,
                                 quint32 supersededDrop, quint32 sendDrop);
    void stageTimesChanged(quint32 decodeUs, quint32 correctUs, quint32 encodeUs);
    void streamChanged(bool running, bool paused);
    void psHostRateTestRunningChanged(bool running);
    void psHostRateTestProgress(int elapsedSeconds, int totalSeconds);
    void psHostRateTestCompleted(double elapsedSeconds, double hostFps, double hostMBps,
                                 double psFps, double psMBps, quint64 hostFrames, quint64 psFrames,
                                 quint64 missingFrames, quint64 partialFrames, quint64 badFrames,
                                 quint32 psSendFailures);
    void psHostCapacityJpegReady(const QByteArray &jpeg);
    void psHostRateTestError(const QString &reason);
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

  private:
    friend class BoardConnectionTests; // fault injection without adding runtime/test-only APIs
    bool m_connected = false, m_connecting = false;
    quint32 m_session = 0;
    quint32 m_helloTxn = 0;
    BoardEndpoints m_endpoints;
    QUdpSocket m_control, m_input, m_output;
    QTimer m_timer;
    QElapsedTimer m_elapsed, m_lastRx;
    qint64 m_lastHelloMs = 0;
    void sendHello();
    void receiveControl();
    void tick();
    void fail(const QString &message);
    void closeSockets();
    quint32 m_epoch = 0;
    struct Command {
        quint8 type = 0;
        quint32 txn = 0;
        QByteArray body;
        qint64 started = 0, last = 0;
        quint32 epoch = 0;
    };
    Command m_poseCommand, m_streamCommand, m_returnCommand, m_capacityCommand;
    double m_pendingPan = 0, m_pendingTilt = 0, m_pendingHfov = 90;
    quint32 m_nextTxn = 0, m_creditSeq = 0, m_frameId = 0;
    quint32 m_lastOutputFrame = 0;
    int m_free = 0, m_slotCount = 4;
    bool m_streaming = false, m_paused = false, m_desiredPaused = false, m_stopping = false;
    bool m_queuedPose = false;
    QString m_disconnectReason;
    double m_queuedPan = 0, m_queuedTilt = 0, m_queuedHfov = 90;
    struct Upload {
        QByteArray jpeg;
        quint32 frame = 0, epoch = 0, crc = 0;
        int index = 0, count = 0;
        qint64 started = 0, captured = 0;
        int writeStalls = 0;
        quint32 cameraTimestampMs = 0;
        quint64 sampleId = 0;
    };
    struct Sent {
        quint32 epoch = 0;
        qint64 captured = 0, started = 0;
        quint64 sampleId = 0;
    };
    struct InputRecord {
        QByteArray jpeg;
        quint32 frame = 0, epoch = 0, cameraTimestamp = 0, crc = 0;
    };
    QList<InputRecord> m_recentInput;
    QString saveFaultInput(const InputRecord &record, quint32 frame, quint8 stage, quint8 reason,
                           const QString &directory);
    Upload m_upload;
    QByteArray m_latestJpeg;
    LatencyTracker *m_latency = nullptr;
    quint64 m_latestSample = 0;
    bool m_latencyActive = false;
    quint32 m_latencyEpoch = 0;
    QByteArray m_capacityJpeg;
    qint64 m_latestCapture = 0, m_nextBatch = 0;
    quint32 m_latestCameraTimestamp = 0;
    QHash<quint32, Sent> m_sent;
    MediaWire::Collector m_collector;
    MediaWire::Collector m_returnCollector;
    bool m_returnRequested = false, m_returnActive = false;
    qint64 m_returnStartedMs = 0;
    int m_returnLastProgress = -1;
    quint64 m_returnHostFrames = 0, m_returnHostBytes = 0, m_returnBad = 0;
    QByteArray m_returnSample;
    bool m_returnAwaitingDecode = false;
    bool m_capacityRequested = false, m_capacityActive = false;
    quint8 m_capacityKind = 0, m_capacityStage = 0;
    quint32 m_capacityTestId = 0;
    qint64 m_capacityStartedMs = 0;
    int m_capacityLastProgress = -1;
    struct CapacityHostBaseline {
        quint64 attempts = 0, frames = 0, bytes = 0, timeouts = 0, stalls = 0, failures = 0;
        quint64 replacements = 0, durationTotal = 0, maxDuration = 0;
        quint32 incomplete = 0, crc = 0, stale = 0, noSlot = 0;
    } m_capacityBase;
    qint64 m_stageWaitWindowAt = -1;
    quint32 m_stageWaitFrames = 0;
    quint64 m_stageWaitUs[3] = {};
    UploadWaitMeter m_uploadWait;
    FlowDiagnostics m_psFlow;
    bool m_psWaitBaseline = false;
    quint32 m_psWaitSamples = 0;
    quint64 m_psWaitSum[6] = {};
    QTimer m_mediaTimer;
    struct LinkDiagnostics {
        qint64 lastReport = -1, heartbeatAt = -1, creditAt = -1, resultAt = -1, outputAt = -1,
               notifyAt = -1;
        quint32 psRx = 0, psTx = 0, psFree = 0, psDrop[4] = {}, stageUs[3] = {}, inputBytes = 0,
                outputBytes = 0;
        quint32 psCompleteFrames = 0, psCompleteBytes = 0;
        quint32 psOutputCompleteFrames = 0, psOutputCompleteBytes = 0;
        quint32 psOutputSuperseded = 0, psOutputSendDrop = 0;
        quint64 resultSeen = 0, txAttempt = 0, txComplete = 0, txBytes = 0, txStall = 0, txFail = 0,
                txTimeout = 0;
        quint64 txReplaced = 0, txDurationMsTotal = 0, txLastDurationMs = 0;
        quint64 rxPackets = 0, accepted = 0, jpegComplete = 0;
        quint64 filterBad = 0, filterIdentity = 0, filterOld = 0, filterLedger = 0;
        quint64 rxNotify = 0, rxPollDrains = 0, rxReadError = 0, rxSocketError = 0;
        QString lastRxError;
        qint64 ctrlNotifyAt = -1, ctrlAcceptedAt = -1;
        quint64 ctrlNotify = 0, ctrlPollDrains = 0, ctrlPackets = 0, ctrlAccepted = 0,
                ctrlFiltered = 0;
        quint64 ctrlReadError = 0, ctrlSocketError = 0;
        QString lastCtrlError;
    } m_link;
    bool m_outputReading = false, m_outputDrainQueued = false;
    quint64 m_outputReadGeneration = 0;
    bool m_controlReading = false, m_controlDrainQueued = false;
    bool m_controlTimeoutPending = false;
    quint64 m_controlReadGeneration = 0;
    QString controlDiagnostic() const;
    void checkControlDeadline(bool allowContinuation);
    void reportLinkDiagnostics();
    void receiveOutput();
    void pumpMedia();
    void handleMediaControl(const FptzProtocol::Packet &packet);
    bool sendCommand(Command &command);
    void streamCommand(quint8 action);
    void returnTestCommand(quint8 action);
    void capacityTestCommand(quint8 action);
    void finishCapacityTestError(const QString &reason);
    void finishReturnTestError(const QString &reason);
    void resetMedia();
    void emitUploadCounters(quint64 completedDurationMs = 0);
    bool handleUploadWriteFailure(QAbstractSocket::SocketError error, const QString &errorText,
                                  qint64 now);
    void sendQueuedPose();
};
