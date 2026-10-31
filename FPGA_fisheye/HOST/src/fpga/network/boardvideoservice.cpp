#include "boardvideoservice.h"
#include "hostpsratemeter.h"
#include "panoramaview.h"
#include <QEventLoop>
#include <QTimer>
#include <QtMath>
#include <chrono>
#include <memory>
#include <opencv2/imgproc.hpp>

namespace {
struct BusyReset {
    std::atomic_bool &flag;
    ~BusyReset() {
        flag.store(false);
    }
};
qint64 steadyUs() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}
} // namespace

BoardVideoService::BoardVideoService(QObject *parent) : QThread(parent) {
    qRegisterMetaType<PsPipelineRateResult>();
    start();
}
BoardVideoService::~BoardVideoService() {
    m_decodeCapacityCancel.store(true);
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_exit = true;
    }
    m_wake.notify_all();
    wait();
}
void BoardVideoService::connectToBoard(const BoardEndpoints &endpoints) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_quality)
        return;
    m_endpoints = endpoints;
    m_connectRequest = true;
    m_disconnectRequest = false;
    m_connecting = true;
}
void BoardVideoService::disconnectFromBoard() {
    m_decodeCapacityCancel.store(true);
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_quality)
        return;
    m_disconnectRequest = true;
    m_connectRequest = false;
}
bool BoardVideoService::isConnected() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_connected;
}
bool BoardVideoService::isConnecting() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_connecting;
}
void BoardVideoService::requestPose(double pan, double tilt, double hfov) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_quality)
        return;
    if (m_pan == pan && m_tilt == tilt && m_hfov == hfov)
        return;
    m_pan = pan;
    m_tilt = tilt;
    m_hfov = hfov;
    m_viewDirty = true;
    m_wake.notify_all();
}
void BoardVideoService::setPaused(bool paused) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_quality)
        return;
    m_paused = paused;
    m_pauseDirty = true;
    if (paused) {
        m_raw = {};
        m_encoded.clear();
        m_decode = {};
        m_display = {};
    }
}
bool BoardVideoService::enterQualityMode() {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_quality || m_exit)
        return false;
    m_qualityRestoreConnection = m_connected || m_connecting || m_connectRequest;
    m_qualityRestorePaused = m_paused;
    m_quality = m_qualityEnter = true;
    m_qualityLeave = false;
    m_connectRequest = m_disconnectRequest = false;
    m_paused = true;
    ++m_generation;
    m_raw = {};
    m_encoded.clear();
    m_decode = {};
    m_display = {};
    m_sphere = {};
    m_panorama = {};
    m_viewDirty = false;
    m_latency.setActive(false, "图像质量独立测试");
    return true;
}
void BoardVideoService::leaveQualityMode() {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_quality)
        m_qualityLeave = true;
}
bool BoardVideoService::qualityCanCapture() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_quality && m_qualityRestoreConnection;
}
BoardEndpoints BoardVideoService::qualityEndpoints() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_endpoints;
}
void BoardVideoService::setInputJpegQuality(int quality, bool smooth) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_inputJpegQuality = qBound(50, quality, 95);
    m_smoothInput = smooth;
}
void BoardVideoService::setDisplaySizes(int sphereDiameter, int perspectiveWidth) {
    // Quantize to avoid rebuilding maps for every pixel while resizing.
    const auto rounded = [](int value) { return ((value + 63) / 64) * 64; };
    // Viewport crops the empty margin of the sphere, whose visible diameter
    // is about 85% of the projection texture. Account for that margin here.
    const int sphereSide = qBound(720, rounded(qCeil(sphereDiameter * 1.18)), 1440);
    const int width = qBound(1280, rounded(perspectiveWidth), 1920);
    const QSize sphereSize(sphereSide, sphereSide), perspectiveSize(width, width * 9 / 16);
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_sphereSize == sphereSize && m_perspectiveSize == perspectiveSize)
        return;
    m_sphereSize = sphereSize;
    m_perspectiveSize = perspectiveSize;
    m_viewDirty = true;
    m_wake.notify_all();
}
void BoardVideoService::submit(const QImage &image, qint64 cameraTimestampMs) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_connected || m_paused || m_exit || m_quality)
        return;
    if (!m_raw.isNull()) {
        m_latency.status(m_rawSample, "raw_queue_replaced");
        ++m_encodeReplacements;
    }
    m_rawSample = m_latency.accept();
    m_raw = image;
    m_cameraTimestamp = quint32(cameraTimestampMs);
    m_capture = MediaWire::nowMs();
    m_wake.notify_all();
}
void BoardVideoService::submitJpeg(const QByteArray &jpeg, qint64 capturedMs) {
    if (!MediaWire::validInput(jpeg))
        return;
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_connected || m_paused || m_exit || m_quality)
        return;
    m_latency.status(m_rawSample, "raw_queue_replaced");
    m_latency.status(m_encodedSample, "encoded_queue_replaced");
    m_encodedSample = m_latency.accept(false);
    m_raw = {};
    m_encoded = jpeg;
    m_encodedCapture = capturedMs;
    m_wake.notify_all();
}
void BoardVideoService::startHostPsRateTest() {
    bool connected = false;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        connected = m_connected && !m_paused && !m_exit;
        if (connected) {
            m_hostPsStartRequest = true;
            m_hostPsStopRequest = false;
            m_hostPsRequestAt = MediaWire::nowMs();
        }
    }
    if (!connected)
        emit hostPsRateTestError(QStringLiteral("请先连接板卡并保持视频运行"));
}
void BoardVideoService::stopHostPsRateTest() {
    bool cancelledBeforeBaseline = false;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_hostPsStartRequest) {
            m_hostPsStartRequest = false;
            cancelledBeforeBaseline = true;
        } else {
            m_hostPsStopRequest = true;
        }
    }
    if (cancelledBeforeBaseline) {
        emit hostPsRateTestRunningChanged(false);
        emit hostPsRateTestError(QStringLiteral("测试在取得PS起始计数前已停止"));
    }
}
void BoardVideoService::startPsPipelineRateTest() {
    bool connected = false;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        connected = m_connected && !m_paused && !m_exit;
        if (connected) {
            m_psPipelineStartRequest = true;
            m_psPipelineStopRequest = false;
            m_psPipelineRequestAt = MediaWire::nowMs();
        }
    }
    if (!connected)
        emit psPipelineRateTestError(QStringLiteral("请先连接板卡并保持视频运行"));
}
void BoardVideoService::stopPsPipelineRateTest() {
    bool cancelledBeforeBaseline = false;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_psPipelineStartRequest) {
            m_psPipelineStartRequest = false;
            cancelledBeforeBaseline = true;
        } else
            m_psPipelineStopRequest = true;
    }
    if (cancelledBeforeBaseline) {
        emit psPipelineRateTestRunningChanged(false);
        emit psPipelineRateTestError(QStringLiteral("测试在取得PS处理计数前已停止"));
    }
}
void BoardVideoService::startPsHostRateTest() {
    m_decodeCapacityCancel.store(false);
    std::lock_guard<std::mutex> lock(m_mutex);
    m_psHostStartRequest = true;
    m_psHostStopRequest = false;
    m_raw = {};
    m_encoded.clear();
    m_decode = {};
}
void BoardVideoService::stopPsHostRateTest() {
    m_decodeCapacityCancel.store(true);
    std::lock_guard<std::mutex> lock(m_mutex);
    m_psHostStopRequest = true;
    m_psHostStartRequest = false;
}
BoardVideoService::Display BoardVideoService::takeDisplay() {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto result = m_display;
    m_display = {};
    if (!m_connected || m_paused || result.epoch != m_epoch)
        return {};
    m_latency.mark(result.sampleId, "ui_take");
    return result;
}
QImage BoardVideoService::takeSphere() {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto result = m_sphere;
    m_sphere = {};
    return m_connected && !m_paused ? result : QImage();
}
QImage BoardVideoService::takePanorama() {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto result = m_panorama;
    m_panorama = {};
    return m_connected && !m_paused ? result : QImage();
}
void BoardVideoService::codecLoop() {
    PanoramaView projection;
    QImage panorama;
    Output panoramaIdentity;
    quint64 panoramaGeneration = 0;
    for (;;) {
        BusyReset busy{m_codecBusy};
        Output output;
        quint64 generation;
        df::View view;
        QSize sphereSize, perspectiveSize;
        bool viewDirty = false;
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_wake.wait(lock, [this] {
                return m_codecExit || m_decodeCapacityPending || !m_decode.jpeg.isEmpty() ||
                       m_viewDirty;
            });
            if (m_codecExit)
                break;
            m_codecBusy.store(true);
            if (m_decodeCapacityPending) {
                const QByteArray jpeg = m_decodeCapacityJpeg;
                m_decodeCapacityPending = false;
                m_decodeCapacityJpeg.clear();
                lock.unlock();
                const auto result = runHostDecodeCapacity(
                    jpeg, {2656, 1328}, 5000,
                    [this](int seconds) { emit psHostRateTestProgress(qMin(10, 5 + seconds), 10); },
                    [this] { return !m_decodeCapacityCancel.load(); });
                if (m_decodeCapacityCancel.load())
                    emit psHostRateTestError(QStringLiteral("Host JPEG解码容量测试已停止"));
                else
                    emit hostDecodeCapacityCompleted(
                        result.elapsedSeconds, result.fps, result.averageMs, result.attempts,
                        result.completed, result.failures, result.sizeErrors);
                lock.lock();
                m_decodeCapacityFinishRequest = true;
                lock.unlock();
                continue;
            }
            output = m_decode;
            m_decode = {};
            generation = m_generation;
            view = {m_pan, m_tilt, m_hfov};
            sphereSize = m_sphereSize;
            perspectiveSize = m_perspectiveSize;
            viewDirty = m_viewDirty;
            m_viewDirty = false;
        }
        if (!output.jpeg.isEmpty()) {
            m_latency.mark(output.sampleId, "decode_start");
            auto image = QImage::fromData(output.jpeg, "jpeg");
            m_latency.mark(output.sampleId, "decode_end");
            if (image.size() != QSize(2656, 1328)) {
                m_latency.status(output.sampleId, "decode_failed");
                emit statusChanged(
                    QStringLiteral("回图JPEG必须为2656×1328且解码成功，丢弃不兼容帧"));
            } else {
                panorama = image;
                panoramaIdentity = output;
                panoramaGeneration = generation;
                viewDirty = true;
                std::lock_guard<std::mutex> lock(m_mutex);
                if (generation == m_generation && !m_quality)
                    ++m_decoded;
            }
        }
        if (viewDirty && !panorama.isNull()) {
            try {
                QImage sphere, perspective;
                const auto renderStart = steadyUs();
                projection.render(panorama, view, sphere, perspective, sphereSize, perspectiveSize);
                const auto renderEnd = steadyUs();
                std::lock_guard<std::mutex> lock(m_mutex);
                if (panoramaGeneration == m_generation && panoramaIdentity.epoch == m_epoch &&
                    m_connected && !m_paused) {
                    m_sphere = sphere;
                    m_panorama = panorama;
                    if (m_display.sampleId != panoramaIdentity.sampleId)
                        m_latency.status(m_display.sampleId, "display_queue_replaced");
                    m_latency.mark(panoramaIdentity.sampleId, "render_start", renderStart);
                    m_latency.mark(panoramaIdentity.sampleId, "render_end", renderEnd);
                    m_display = {perspective,
                                 panoramaIdentity.frame,
                                 panoramaIdentity.epoch,
                                 panoramaIdentity.latency,
                                 renderEnd,
                                 panoramaIdentity.sampleId,
                                 renderStart};
                } else
                    panorama = {};
            } catch (const std::exception &e) {
                m_latency.status(panoramaIdentity.sampleId, "render_failed");
                emit statusChanged(QString::fromUtf8(e.what()));
            }
        }
    }
}
void BoardVideoService::encodeLoop() {
    QString previousError;
    for (;;) {
        BusyReset busy{m_encoderBusy};
        QImage raw;
        quint64 generation;
        qint64 captured;
        quint64 sampleId = 0;
        quint32 cameraTimestamp;
        int jpegQuality;
        bool smoothInput;
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_wake.wait(lock, [this] { return m_codecExit || !m_raw.isNull(); });
            if (m_codecExit)
                break;
            m_encoderBusy.store(true);
            raw = m_raw;
            sampleId = m_rawSample;
            m_raw = {};
            generation = m_generation;
            captured = m_capture;
            cameraTimestamp = m_cameraTimestamp;
            jpegQuality = m_inputJpegQuality;
            smoothInput = m_smoothInput;
        }
        if (!raw.isNull()) {
            QString error;
            const auto encodeStarted = steadyUs();
            m_latency.mark(sampleId, "encode_start", encodeStarted);
            QImage uploadImage = raw;
            if (smoothInput) {
                auto rgb = raw.convertToFormat(QImage::Format_RGB888);
                cv::Mat source(rgb.height(), rgb.width(), CV_8UC3, rgb.bits(), rgb.bytesPerLine());
                cv::Mat softened;
                cv::GaussianBlur(source, softened, {13, 13}, 3.0);
                uploadImage = QImage(softened.data, softened.cols, softened.rows,
                                     int(softened.step), QImage::Format_RGB888).copy();
            }
            auto jpeg = MediaWire::encode(uploadImage, &error, jpegQuality);
            const auto encodeEnd = steadyUs();
            const auto encodeUs = quint64(encodeEnd - encodeStarted);
            m_latency.mark(sampleId, "encode_end", encodeEnd);
            if (jpeg.isEmpty()) {
                m_latency.status(sampleId, "encode_failed");
                if (error != previousError)
                    emit statusChanged(error);
                previousError = error;
            } else {
                previousError.clear();
                std::lock_guard<std::mutex> lock(m_mutex);
                if (generation == m_generation && m_connected && !m_paused) {
                    if (!m_encoded.isEmpty()) {
                        m_latency.status(m_encodedSample, "encoded_queue_replaced");
                        ++m_encodeReplacements;
                    }
                    m_encodedSample = sampleId;
                    ++m_encodedFrames;
                    m_encodeWorkUs += encodeUs;
                    m_encoded = jpeg;
                    m_encodedCapture = captured;
                    m_encodedCameraTimestamp = cameraTimestamp;
                }
            }
        }
    }
}

void BoardVideoService::run() {
    BoardConnection connection;
    connection.setLatencyTracker(&m_latency);
    QEventLoop loop;
    QTimer pump;
    HostPsRateMeter hostPsMeter(10000);
    HostPsRateSnapshot hostPsSnapshot;
    int lastHostPsProgress = -1;
    PsPipelineRateMeter psPipelineMeter(10000);
    PsPipelineRateSnapshot psPipelineSnapshot;
    int lastPsPipelineProgress = -1;
    std::thread codec([this] { codecLoop(); });
    std::thread encoder([this] { encodeLoop(); });
    quint64 sent = 0, received = 0, lastSent = 0, lastReceived = 0, lastDecoded = 0;
    quint64 lastEncodedFrames = 0, lastEncodeWorkUs = 0;
    qint64 reportStart = MediaWire::nowMs(), shutdownStart = 0;
    qint64 qualityStopAt = 0;
    bool qualityNotified = false;
    bool havePsOutputBaseline = false;
    quint32 lastPsOutputFrames = 0, lastPsOutputBytes = 0, lastPsOutputSuperseded = 0,
            lastPsOutputSendDrop = 0;
    quint64 lastPsOutputHostFrames = 0;
    qint64 lastPsOutputAt = 0;
    bool havePsReceiveBaseline = false, havePsPipelineBaseline = false;
    quint32 lastPsReceiveFrames = 0, lastPsReceiveBytes = 0, lastPsReceiveIncomplete = 0;
    quint32 lastPsReceiveCrc = 0, lastPsReceiveStale = 0, lastPsReceiveNoSlot = 0;
    qint64 lastPsReceiveAt = 0;
    PsPipelineRateSnapshot lastLivePipeline;
    qint64 lastPsPipelineAt = 0;
    connect(&connection, &BoardConnection::statusChanged, &connection,
            [this, &connection](const QString &s) {
                {
                    std::lock_guard<std::mutex> lock(m_mutex);
                    m_connected = connection.isConnected();
                    m_connecting = connection.isConnecting();
                }
                emit statusChanged(s);
            });
    connect(&connection, &BoardConnection::connectedChanged, &connection,
            [this, &connection, &sent, &received, &lastSent, &lastReceived, &lastDecoded,
             &reportStart, &lastEncodedFrames, &lastEncodeWorkUs, &hostPsMeter, &lastHostPsProgress,
             &psPipelineMeter, &lastPsPipelineProgress, &havePsOutputBaseline,
             &havePsReceiveBaseline, &havePsPipelineBaseline](bool connected) {
                bool paused;
                {
                    std::lock_guard<std::mutex> lock(m_mutex);
                    m_connected = connected;
                    m_connecting = false;
                    ++m_generation;
                    m_epoch = 0;
                    m_encoded.clear();
                    m_raw = {};
                    m_decode = {};
                    m_display = {};
                    m_sphere = {};
                    m_panorama = {};
                    m_decoded = 0;
                    m_encodedFrames = m_encodeWorkUs = m_encodeReplacements = 0;
                    lastEncodedFrames = lastEncodeWorkUs = 0;
                    m_hostPsStartRequest = false;
                    m_hostPsStopRequest = false;
                    m_psPipelineStartRequest = false;
                    m_psPipelineStopRequest = false;
                    paused = m_paused;
                    m_poseDirty = false;
                }
                sent = received = lastSent = lastReceived = lastDecoded = 0;
                reportStart = MediaWire::nowMs();
                havePsOutputBaseline = false;
                havePsReceiveBaseline = false;
                havePsPipelineBaseline = false;
                if (hostPsMeter.active())
                    emit hostPsRateTestRunningChanged(false);
                if (psPipelineMeter.active())
                    emit psPipelineRateTestRunningChanged(false);
                hostPsMeter = HostPsRateMeter(10000);
                lastHostPsProgress = -1;
                psPipelineMeter = PsPipelineRateMeter(10000);
                lastPsPipelineProgress = -1;
                emit connectedChanged(connected);
                if (connected) {
                    connection.requestPose(0, 0, 360);
                    connection.setPaused(paused);
                }
            });
    connect(&connection, &BoardConnection::poseResolved, &connection,
            [this](bool accepted, double pan, double tilt, double hfov, quint32 epoch, int reason) {
                const qint64 atUs = steadyUs();
                if (accepted) {
                    std::lock_guard<std::mutex> lock(m_mutex);
                    m_epoch = epoch;
                    m_decode = {};
                    m_display = {};
                }
                emit poseResolvedTimed(accepted, pan, tilt, hfov, epoch, reason, atUs);
                emit poseResolved(accepted, pan, tilt, hfov, epoch, reason);
            });
    connect(&connection, &BoardConnection::poseCommandSent, &connection,
            [this](double pan, double tilt, double hfov) {
                emit poseCommandDispatched(pan, tilt, hfov, steadyUs());
            });
    connect(&connection, &BoardConnection::poseCommandQueued, &connection,
            [this](bool replaced) { emit poseCommandQueued(replaced); });
    connect(&connection, &BoardConnection::poseAccepted, &connection,
            [this](double pan, double tilt, double hfov) {
                emit poseAcceptedTimed(pan, tilt, hfov, steadyUs());
            });
    connect(&connection, &BoardConnection::poseReady, &connection,
            [this](double pan, double tilt, double hfov) {
                emit poseReadyTimed(pan, tilt, hfov, steadyUs());
            });
    connect(&connection, &BoardConnection::poseLutProfile, &connection,
            [this](double pan, double tilt, double hfov, quint32 wallUs, quint32 workUs,
                   quint32 anchorUs) {
                emit poseLutProfile(pan, tilt, hfov, wallUs, workUs, anchorUs);
            });
    connect(&connection, &BoardConnection::jpegReady, &connection,
            [this, &received, &connection](const QByteArray &jpeg, quint32 frame, quint32 epoch,
                                           double latency) {
                ++received;
                std::lock_guard<std::mutex> lock(m_mutex);
                if (m_connected && !m_paused && m_epoch == epoch) {
                    m_latency.status(m_decode.sampleId, "decode_queue_replaced");
                    m_decode = {jpeg, frame, epoch, latency,
                                m_latency.lookup(connection.session(), epoch, frame)};
                    m_wake.notify_all();
                }
            });
    connect(&connection, &BoardConnection::frameSent, &connection, [&sent](quint32) { ++sent; });
    connect(&connection, &BoardConnection::psHostRateTestRunningChanged, this,
            &BoardVideoService::psHostRateTestRunningChanged);
    connect(&connection, &BoardConnection::psHostRateTestProgress, this,
            &BoardVideoService::psHostRateTestProgress);
    connect(&connection, &BoardConnection::psHostRateTestCompleted, this,
            &BoardVideoService::psHostRateTestCompleted);
    connect(&connection, &BoardConnection::psHostCapacityJpegReady, &connection,
            [this](const QByteArray &jpeg) {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_decodeCapacityJpeg = jpeg;
                m_decodeCapacityPending = true;
                m_wake.notify_all();
            });
    connect(&connection, &BoardConnection::psHostRateTestError, this,
            &BoardVideoService::psHostRateTestError);
    connect(&connection, &BoardConnection::hostPsRateTestRunningChanged, this,
            &BoardVideoService::hostPsRateTestRunningChanged);
    connect(&connection, &BoardConnection::hostPsRateTestProgress, this,
            &BoardVideoService::hostPsRateTestProgress);
    connect(&connection, &BoardConnection::hostPsRateTestCompleted, this,
            &BoardVideoService::hostPsRateTestCompleted);
    connect(&connection, &BoardConnection::hostPsRateTestError, this,
            &BoardVideoService::hostPsRateTestError);
    connect(&connection, &BoardConnection::psPipelineRateTestRunningChanged, this,
            &BoardVideoService::psPipelineRateTestRunningChanged);
    connect(&connection, &BoardConnection::psPipelineRateTestProgress, this,
            &BoardVideoService::psPipelineRateTestProgress);
    connect(&connection, &BoardConnection::psPipelineRateTestCompleted, this,
            &BoardVideoService::psPipelineRateTestCompleted);
    connect(&connection, &BoardConnection::psPipelineRateTestError, this,
            &BoardVideoService::psPipelineRateTestError);
    connect(&connection, &BoardConnection::psStageWaitWindow, this,
            &BoardVideoService::psStageWaitWindow);
    connect(&connection, &BoardConnection::uploadWaitWindow, this,
            &BoardVideoService::uploadWaitWindow);
    connect(&connection, &BoardConnection::psWaitWindow, this, &BoardVideoService::psWaitWindow);
    connect(&connection, &BoardConnection::psFlowWindow, this, &BoardVideoService::psFlowWindow);
    connect(&connection, &BoardConnection::hostUploadCountersChanged, &connection,
            [&hostPsSnapshot,
             &hostPsMeter](quint64 attempts, quint64 completeFrames, quint64 completeBytes,
                           quint64 timeouts, quint64 writeStalls, quint64 writeFailures,
                           quint64 replacements, quint64 durationMsTotal, quint64 lastDurationMs) {
                hostPsSnapshot.hostAttempts = attempts;
                hostPsSnapshot.hostFrames = completeFrames;
                hostPsSnapshot.hostBytes = completeBytes;
                hostPsSnapshot.hostTimeouts = timeouts;
                hostPsSnapshot.hostWriteStalls = writeStalls;
                hostPsSnapshot.hostWriteFailures = writeFailures;
                hostPsSnapshot.hostReplacements = replacements;
                hostPsSnapshot.hostDurationMsTotal = durationMsTotal;
                if (hostPsMeter.active())
                    hostPsSnapshot.hostWindowMaxDurationMs =
                        qMax(hostPsSnapshot.hostWindowMaxDurationMs, lastDurationMs);
            });
    connect(&connection, &BoardConnection::psReceiveCountersChanged, &connection,
            [this, &hostPsMeter, &hostPsSnapshot, &lastHostPsProgress, &havePsReceiveBaseline,
             &lastPsReceiveFrames, &lastPsReceiveBytes, &lastPsReceiveIncomplete, &lastPsReceiveCrc,
             &lastPsReceiveStale, &lastPsReceiveNoSlot,
             &lastPsReceiveAt](quint32 frames, quint32 bytes, quint32 incomplete, quint32 crc,
                               quint32 stale, quint32 noSlot) {
                hostPsSnapshot.psFrames = frames;
                hostPsSnapshot.psBytes = bytes;
                hostPsSnapshot.incompleteDrop = incomplete;
                hostPsSnapshot.crcDrop = crc;
                hostPsSnapshot.staleDrop = stale;
                hostPsSnapshot.noSlotDrop = noSlot;
                const auto now = MediaWire::nowMs();
                if (havePsReceiveBaseline) {
                    const qint64 elapsed = qMax<qint64>(1, now - lastPsReceiveAt);
                    emit psReceiveWindowChanged(
                        double(frames - lastPsReceiveFrames) * 1000.0 / double(elapsed),
                        double(bytes - lastPsReceiveBytes) / double(elapsed) / 1000.0,
                        quint64(frames - lastPsReceiveFrames), incomplete - lastPsReceiveIncomplete,
                        crc - lastPsReceiveCrc, stale - lastPsReceiveStale,
                        noSlot - lastPsReceiveNoSlot);
                }
                havePsReceiveBaseline = true;
                lastPsReceiveFrames = frames;
                lastPsReceiveBytes = bytes;
                lastPsReceiveIncomplete = incomplete;
                lastPsReceiveCrc = crc;
                lastPsReceiveStale = stale;
                lastPsReceiveNoSlot = noSlot;
                lastPsReceiveAt = now;
            });
    connect(&connection, &BoardConnection::stageTimesChanged, &connection,
            [this](quint32 decodeUs, quint32 correctUs, quint32 encodeUs) {
                emit stageTimesChanged(decodeUs, correctUs, encodeUs);
            });
    connect(&connection, &BoardConnection::psPipelineCountersChanged, &connection,
            [this, &psPipelineMeter, &psPipelineSnapshot, &lastPsPipelineProgress,
             &havePsPipelineBaseline, &lastLivePipeline, &lastPsPipelineAt](
                quint32 ds, quint32 dc, quint32 df, quint32 dt, quint32 cs, quint32 cc, quint32 cf,
                quint32 ct, quint32 es, quint32 ec, quint32 ef, quint32 et) {
                psPipelineSnapshot = {ds, dc, df, dt, cs, cc, cf, ct, es, ec, ef, et};
                const auto now = MediaWire::nowMs();
                if (havePsPipelineBaseline) {
                    const double elapsed = qMax<qint64>(1, now - lastPsPipelineAt) / 1000.0;
                    PsPipelineRateResult live;
                    live.elapsedSeconds = elapsed;
                    live.decodeStarted = ds - lastLivePipeline.decodeStarted;
                    live.decodeCompleted = dc - lastLivePipeline.decodeCompleted;
                    live.decodeFailed = df - lastLivePipeline.decodeFailed;
                    live.decodeTimeout = dt - lastLivePipeline.decodeTimeout;
                    live.correctStarted = cs - lastLivePipeline.correctStarted;
                    live.correctCompleted = cc - lastLivePipeline.correctCompleted;
                    live.correctFailed = cf - lastLivePipeline.correctFailed;
                    live.correctTimeout = ct - lastLivePipeline.correctTimeout;
                    live.encodeStarted = es - lastLivePipeline.encodeStarted;
                    live.encodeCompleted = ec - lastLivePipeline.encodeCompleted;
                    live.encodeFailed = ef - lastLivePipeline.encodeFailed;
                    live.encodeTimeout = et - lastLivePipeline.encodeTimeout;
                    live.decodeBoundaryLoss = live.decodeStarted > live.decodeCompleted
                                                  ? live.decodeStarted - live.decodeCompleted
                                                  : 0;
                    live.correctBoundaryLoss = live.correctStarted > live.correctCompleted
                                                   ? live.correctStarted - live.correctCompleted
                                                   : 0;
                    live.encodeBoundaryLoss = live.encodeStarted > live.encodeCompleted
                                                  ? live.encodeStarted - live.encodeCompleted
                                                  : 0;
                    live.decodeFps = double(live.decodeCompleted) / elapsed;
                    live.correctFps = double(live.correctCompleted) / elapsed;
                    live.encodeFps = double(live.encodeCompleted) / elapsed;
                    emit psPipelineWindowChanged(live);
                }
                havePsPipelineBaseline = true;
                lastLivePipeline = psPipelineSnapshot;
                lastPsPipelineAt = now;
            });
    connect(&connection, &BoardConnection::psOutputCountersChanged, &connection,
            [this, &received, &havePsOutputBaseline, &lastPsOutputFrames, &lastPsOutputBytes,
             &lastPsOutputSuperseded, &lastPsOutputSendDrop, &lastPsOutputHostFrames,
             &lastPsOutputAt](quint32 frames, quint32 bytes, quint32 superseded, quint32 sendDrop) {
                const qint64 now = MediaWire::nowMs();
                if (!havePsOutputBaseline) {
                    havePsOutputBaseline = true;
                    lastPsOutputFrames = frames;
                    lastPsOutputBytes = bytes;
                    lastPsOutputSuperseded = superseded;
                    lastPsOutputSendDrop = sendDrop;
                    lastPsOutputHostFrames = received;
                    lastPsOutputAt = now;
                    return;
                }
                const qint64 elapsed = qMax<qint64>(1, now - lastPsOutputAt);
                const quint32 frameDelta = frames - lastPsOutputFrames;
                const quint32 byteDelta = bytes - lastPsOutputBytes;
                const quint32 supersededDelta = superseded - lastPsOutputSuperseded;
                const quint32 sendDropDelta = sendDrop - lastPsOutputSendDrop;
                const quint64 hostDelta = received - lastPsOutputHostFrames;
                emit normalReturnWindowChanged(double(frameDelta) * 1000.0 / double(elapsed),
                                               double(byteDelta) / double(elapsed) / 1000.0,
                                               double(hostDelta) * 1000.0 / double(elapsed),
                                               frameDelta, hostDelta, supersededDelta,
                                               sendDropDelta);
                lastPsOutputFrames = frames;
                lastPsOutputBytes = bytes;
                lastPsOutputSuperseded = superseded;
                lastPsOutputSendDrop = sendDrop;
                lastPsOutputHostFrames = received;
                lastPsOutputAt = now;
            });
    connect(&pump, &QTimer::timeout, &connection, [&] {
        bool exiting, connectRequested, disconnectRequested, poseDirty, pauseDirty, paused,
            psHostStart, psHostStop;
        bool hostPsStart, hostPsStop, psPipelineStart, psPipelineStop, decodeCapacityFinish;
        BoardEndpoints endpoints;
        QByteArray jpeg;
        qint64 captured;
        quint64 sampleId = 0;
        quint64 decoded;
        quint32 cameraTimestamp = 0;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            exiting = m_exit;
            connectRequested = m_connectRequest;
            m_connectRequest = false;
            disconnectRequested = m_disconnectRequest;
            m_disconnectRequest = false;
            poseDirty = m_poseDirty && MediaWire::nowMs() >= m_poseDue;
            if (poseDirty)
                m_poseDirty = false;
            pauseDirty = m_pauseDirty;
            m_pauseDirty = false;
            paused = m_paused;
            endpoints = m_endpoints;
            psHostStart = m_psHostStartRequest;
            m_psHostStartRequest = false;
            psHostStop = m_psHostStopRequest;
            m_psHostStopRequest = false;
            hostPsStart = m_hostPsStartRequest;
            m_hostPsStartRequest = false;
            hostPsStop = m_hostPsStopRequest;
            m_hostPsStopRequest = false;
            psPipelineStart = m_psPipelineStartRequest;
            m_psPipelineStartRequest = false;
            psPipelineStop = m_psPipelineStopRequest;
            m_psPipelineStopRequest = false;
            decodeCapacityFinish = m_decodeCapacityFinishRequest;
            m_decodeCapacityFinishRequest = false;
            jpeg = std::move(m_encoded);
            m_encoded.clear();
            captured = m_encodedCapture;
            sampleId = m_encodedSample;
            decoded = m_decoded;
            cameraTimestamp = m_encodedCameraTimestamp;
        }
        if (exiting) {
            if (!shutdownStart) {
                shutdownStart = MediaWire::nowMs();
                connection.stopAndDisconnect();
            }
            if (!connection.isConnected() || MediaWire::nowMs() - shutdownStart > 1400)
                loop.quit();
            return;
        }
        bool quality, enterQuality, leaveQuality;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            quality = m_quality;
            enterQuality = m_qualityEnter;
            leaveQuality = m_qualityLeave;
            m_qualityEnter = false;
        }
        if (quality) {
            if (enterQuality) {
                qualityStopAt = MediaWire::nowMs();
                qualityNotified = false;
                connection.setLatencyTracker(nullptr);
                connection.stopAndDisconnect();
            }
            if (connection.isConnected() && MediaWire::nowMs() - qualityStopAt >= 1500)
                connection.disconnectFromBoard();
            if (!connection.isConnected() && !connection.isConnecting() && !m_encoderBusy.load() &&
                !m_codecBusy.load()) {
                if (leaveQuality) {
                    std::lock_guard<std::mutex> lock(m_mutex);
                    m_quality = m_qualityLeave = false;
                    m_paused = m_qualityRestorePaused;
                    m_pauseDirty = true;
                    m_connectRequest = m_qualityRestoreConnection;
                    ++m_generation;
                    connection.setLatencyTracker(&m_latency);
                    m_latency.boundary("图像质量测试结束");
                    emit qualityResumed();
                } else if (!qualityNotified) {
                    qualityNotified = true;
                    emit qualitySuspended();
                }
            }
            return;
        }
        if (disconnectRequested)
            connection.stopAndDisconnect();
        else if (connectRequested)
            connection.connectToBoard(endpoints);
        if (poseDirty && connection.isConnected()) {
            connection.requestPose(0, 0, 360);
        }
        if (pauseDirty)
            connection.setPaused(paused);
        if (psHostStart)
            connection.startPsHostRateTest();
        if (psHostStop)
            connection.stopPsHostRateTest();
        if (hostPsStart)
            connection.startHostPsRateTest();
        if (hostPsStop)
            connection.stopHostPsRateTest();
        if (psPipelineStart)
            connection.startPsPipelineRateTest();
        if (psPipelineStop)
            connection.stopPsPipelineRateTest();
        if (decodeCapacityFinish)
            connection.finishPsHostDecodeCapacityTest();
        if (!jpeg.isEmpty())
            connection.submitJpeg(jpeg, captured, cameraTimestamp, sampleId);
        const auto now = MediaWire::nowMs();
        bool telemetryTimeout = false, pipelineTelemetryTimeout = false;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_hostPsStartRequest && now - m_hostPsRequestAt >= 3000) {
                m_hostPsStartRequest = false;
                telemetryTimeout = true;
            }
            if (m_psPipelineStartRequest && now - m_psPipelineRequestAt >= 3000) {
                m_psPipelineStartRequest = false;
                pipelineTelemetryTimeout = true;
            }
        }
        if (telemetryTimeout) {
            emit hostPsRateTestRunningChanged(false);
            emit hostPsRateTestError(
                QStringLiteral("3秒内未收到PS累计接收统计；请下载支持32字节心跳的新PS程序"));
        }
        if (pipelineTelemetryTimeout) {
            emit psPipelineRateTestRunningChanged(false);
            emit psPipelineRateTestError(
                QStringLiteral("3秒内未收到PS处理阶段统计；请下载支持80字节心跳的新PS程序"));
        }
        if (now - reportStart >= 1000) {
            quint64 encoded, work, replacements;
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                encoded = m_encodedFrames;
                work = m_encodeWorkUs;
                replacements = m_encodeReplacements;
            }
            const auto complete = encoded - lastEncodedFrames;
            if (connection.isConnected())
                emit pairEncodingRatesChanged(
                    double(complete) * 1000. / (now - reportStart),
                    complete ? double(work - lastEncodeWorkUs) / complete / 1000. : 0.,
                    replacements);
            lastEncodedFrames = encoded;
            lastEncodeWorkUs = work;
            emit metricsChanged(double(decoded - lastDecoded) * 1000 / (now - reportStart), sent,
                                decoded);
            if (connection.isConnected())
                emit pipelineRatesChanged(
                    double(sent - lastSent) * 1000 / (now - reportStart),
                    double(received - lastReceived) * 1000 / (now - reportStart),
                    double(decoded - lastDecoded) * 1000 / (now - reportStart));
            lastSent = sent;
            lastReceived = received;
            lastDecoded = decoded;
            reportStart = now;
        }
    });
    pump.setTimerType(Qt::PreciseTimer);
    pump.start(1);
    loop.exec();
    connection.disconnectFromBoard();
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_codecExit = true;
    }
    m_wake.notify_all();
    codec.join();
    encoder.join();
}
