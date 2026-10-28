#include "boardconnection.h"
#include <QDateTime>
#include <QDir>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QScopedValueRollback>
#include <QtEndian>
#include <algorithm>
#include <cmath>
#include <iterator>

QString BoardConnection::saveFaultInput(const InputRecord &record, quint32 frame, quint8 stage,
                                        quint8 reason, const QString &directory) {
    const QString folder = QDir(directory).absoluteFilePath(
        QStringLiteral("fault_%1_frame_%2")
            .arg(QDateTime::currentDateTimeUtc().toString("yyyyMMdd_HHmmss_zzz"))
            .arg(frame));
    if (!QDir().mkpath(folder))
        return {};
    auto write = [&](const QString &name, const QByteArray &bytes) {
        QSaveFile file(QDir(folder).filePath(name));
        return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size() &&
               file.commit();
    };
    QJsonObject metadata{{"frame", double(frame)},
                         {"epoch", double(record.epoch)},
                         {"stage", int(stage)},
                         {"reason", int(reason)},
                         {"camera_timestamp_ms", double(record.cameraTimestamp)},
                         {"pair_crc32", QString::number(record.crc, 16)},
                         {"input_available", !record.jpeg.isEmpty()}};
    if (!record.jpeg.isEmpty()) {
        if (!MediaWire::validInput(record.jpeg) || !write("input_pair.bin", record.jpeg))
            return {};
        const auto *b = reinterpret_cast<const uchar *>(record.jpeg.constData());
        const auto front = qFromBigEndian<quint32>(b), rear = qFromBigEndian<quint32>(b + 4);
        const auto rearOffset = 64 + ((front + 127) & ~63u);
        if (!write("front.jpg", record.jpeg.mid(64, front)) ||
            !write("rear.jpg", record.jpeg.mid(rearOffset, rear)))
            return {};
        metadata.insert("front_bytes", double(front));
        metadata.insert("rear_bytes", double(rear));
    }
    if (!write("metadata.json", QJsonDocument(metadata).toJson()))
        return {};
    return folder;
}

void BoardConnection::resetMedia() {
    if (m_latency)
        m_latency->setActive(false, "connection_reset");
    m_latencyActive = false;
    m_latencyEpoch = 0;
    m_latestSample = 0;
    m_recentInput.clear();
    m_uploadWait.reset();
    m_stageWaitWindowAt = -1;
    m_stageWaitFrames = 0;
    std::fill(std::begin(m_stageWaitUs), std::end(m_stageWaitUs), quint64(0));
    m_psFlow.reset();
    m_psWaitBaseline = false;
    m_psWaitSamples = 0;
    std::fill(std::begin(m_psWaitSum), std::end(m_psWaitSum), quint64(0));
    m_epoch = 0;
    m_free = 0;
    m_creditSeq = 0;
    m_frameId = 0;
    m_nextBatch = 0;
    m_lastOutputFrame = 0;
    m_poseCommand = {};
    m_streamCommand = {};
    m_returnCommand = {};
    m_capacityCommand = {};
    m_upload = {};
    m_sent.clear();
    m_collector.clear();
    m_latestJpeg.clear();
    m_capacityJpeg.clear();
    m_returnCollector.clear();
    m_returnRequested = false;
    m_returnActive = false;
    m_returnAwaitingDecode = false;
    m_returnStartedMs = 0;
    m_returnLastProgress = -1;
    m_returnHostFrames = m_returnHostBytes = m_returnBad = 0;
    m_returnSample.clear();
    m_capacityRequested = false;
    m_capacityActive = false;
    m_capacityKind = m_capacityStage = 0;
    m_capacityTestId = 0;
    m_capacityStartedMs = 0;
    m_capacityLastProgress = -1;
    m_capacityBase = {};
    m_streaming = false;
    m_paused = false;
    m_desiredPaused = false;
    m_stopping = false;
    m_queuedPose = false;
    m_disconnectReason.clear();
    m_link = {};
    m_outputDrainQueued = false;
    ++m_outputReadGeneration;
    m_controlDrainQueued = false;
    ++m_controlReadGeneration;
    m_controlTimeoutPending = false;
}

void BoardConnection::emitUploadCounters(quint64 completedDurationMs) {
    emit hostUploadCountersChanged(
        m_link.txAttempt, m_link.txComplete, m_link.txBytes, m_link.txTimeout, m_link.txStall,
        m_link.txFail, m_link.txReplaced, m_link.txDurationMsTotal, completedDurationMs);
}

bool BoardConnection::handleUploadWriteFailure(QAbstractSocket::SocketError error,
                                               const QString &errorText, qint64 now) {
    if (error == QAbstractSocket::SocketResourceError || error == QAbstractSocket::TemporaryError) {
        ++m_link.txStall;
        ++m_upload.writeStalls;
        m_nextBatch = now + 1;
        emitUploadCounters();
        if (m_upload.writeStalls == 1)
            emit statusChanged(QStringLiteral("UDP发送缓冲暂时受阻，将从当前分片重试；frame=%1 "
                                              "fragment=%2/%3 qt_error=%4 error_text=\"%5\"")
                                   .arg(m_upload.frame)
                                   .arg(m_upload.index + 1)
                                   .arg(m_upload.count)
                                   .arg(int(error))
                                   .arg(errorText));
        return true;
    }
    ++m_link.txFail;
    emitUploadCounters();
    return false;
}

QString BoardConnection::controlDiagnostic() const {
    const auto now = MediaWire::nowMs();
    const auto age = [now](qint64 at) {
        return at < 0 ? QStringLiteral("unknown") : QString::number(now - at);
    };
    return QStringLiteral(
               "CTRL ctrl_notify=%1 ctrl_notify_age_ms=%2 ctrl_poll_drains=%3 ctrl_packets=%4 "
               "ctrl_accepted=%5 ctrl_accepted_age_ms=%6 ctrl_filtered=%7 ctrl_read_error=%8 "
               "ctrl_socket_error=%9 ctrl_socket_state=%10 ctrl_socket_valid=%11 ctrl_pending=%12 "
               "last_ctrl_error=%13")
        .arg(m_link.ctrlNotify)
        .arg(age(m_link.ctrlNotifyAt))
        .arg(m_link.ctrlPollDrains)
        .arg(m_link.ctrlPackets)
        .arg(m_link.ctrlAccepted)
        .arg(age(m_link.ctrlAcceptedAt))
        .arg(m_link.ctrlFiltered)
        .arg(m_link.ctrlReadError)
        .arg(m_link.ctrlSocketError)
        .arg(int(m_control.state()))
        .arg(m_control.isValid() ? 1 : 0)
        .arg(m_control.hasPendingDatagrams() ? 1 : 0)
        .arg(m_link.lastCtrlError.isEmpty() ? QStringLiteral("none") : m_link.lastCtrlError);
}

void BoardConnection::reportLinkDiagnostics() {
    const auto now = MediaWire::nowMs();
    if (m_link.lastReport < 0 || now - m_link.lastReport < 1000)
        return;
    m_link.lastReport = now;
    const auto age = [now](qint64 at) {
        return at < 0 ? QStringLiteral("unknown") : QString::number(now - at);
    };
    const auto known = [](qint64 at, quint64 value) {
        return at < 0 ? QStringLiteral("unknown") : QString::number(value);
    };
    const QString stream = m_stopping                      ? QStringLiteral("stopping")
                           : !m_streaming                  ? QStringLiteral("idle")
                           : (m_paused || m_desiredPaused) ? QStringLiteral("paused")
                                                           : QStringLiteral("running");
    QString line = QStringLiteral("链路诊断 session=%1 epoch=%2 stream=%3 free=%4/%5 credit_seq=%6 "
                                  "credit_age_ms=%7 pending=%8 ps_free=%9\n")
                       .arg(m_session)
                       .arg(m_epoch)
                       .arg(stream)
                       .arg(known(m_link.creditAt, m_free))
                       .arg(m_slotCount)
                       .arg(known(m_link.creditAt, m_creditSeq))
                       .arg(age(m_link.creditAt))
                       .arg(m_sent.size())
                       .arg(known(m_link.creditAt, m_link.psFree));
    line += QStringLiteral("Host上传 batch_packets=%1 batch_gap_ms=1 deadline_ms=150\n")
                .arg(qBound(1, m_endpoints.uploadBatchPackets, 32));
    line += QStringLiteral("PS_rx=%1 PS_tx=%2 PS_complete=%3 PS_complete_bytes=%4 "
                           "heartbeat_age_ms=%5 PS_drop=%6/%7/%8/%9 result_seen=%10 "
                           "jpeg_bytes=%11/%12 stage_us=%13/%14/%15 result_age_ms=%16\n")
                .arg(known(m_link.heartbeatAt, m_link.psRx))
                .arg(known(m_link.heartbeatAt, m_link.psTx))
                .arg(known(m_link.heartbeatAt, m_link.psCompleteFrames))
                .arg(known(m_link.heartbeatAt, m_link.psCompleteBytes))
                .arg(age(m_link.heartbeatAt))
                .arg(known(m_link.heartbeatAt, m_link.psDrop[0]))
                .arg(known(m_link.heartbeatAt, m_link.psDrop[1]))
                .arg(known(m_link.heartbeatAt, m_link.psDrop[2]))
                .arg(known(m_link.heartbeatAt, m_link.psDrop[3]))
                .arg(known(m_link.resultAt, m_link.resultSeen))
                .arg(known(m_link.resultAt, m_link.inputBytes))
                .arg(known(m_link.resultAt, m_link.outputBytes))
                .arg(known(m_link.resultAt, m_link.stageUs[0]))
                .arg(known(m_link.resultAt, m_link.stageUs[1]))
                .arg(known(m_link.resultAt, m_link.stageUs[2]))
                .arg(age(m_link.resultAt));
    line += QStringLiteral("PS_output_complete=%1 PS_output_bytes=%2 PS_output_superseded=%3 "
                           "PS_output_send_drop=%4\n")
                .arg(known(m_link.heartbeatAt, m_link.psOutputCompleteFrames))
                .arg(known(m_link.heartbeatAt, m_link.psOutputCompleteBytes))
                .arg(known(m_link.heartbeatAt, m_link.psOutputSuperseded))
                .arg(known(m_link.heartbeatAt, m_link.psOutputSendDrop));
    line +=
        QStringLiteral(
            "Host tx_attempt=%1 tx_complete=%2 tx_stall=%3 tx_fail=%4 tx_timeout=%5 tx_replaced=%6 "
            "tx_avg_ms=%7 tx_last_ms=%8 rx_packets=%9 accepted=%10 jpeg_complete=%11 rx_age_ms=%12 "
            "filter_bad=%13 filter_identity=%14 filter_old=%15 filter_ledger=%16")
            .arg(m_link.txAttempt)
            .arg(m_link.txComplete)
            .arg(m_link.txStall)
            .arg(m_link.txFail)
            .arg(m_link.txTimeout)
            .arg(m_link.txReplaced)
            .arg(m_link.txComplete ? double(m_link.txDurationMsTotal) / double(m_link.txComplete)
                                   : 0.0,
                 0, 'f', 2)
            .arg(m_link.txLastDurationMs)
            .arg(m_link.rxPackets)
            .arg(m_link.accepted)
            .arg(m_link.jpegComplete)
            .arg(age(m_link.outputAt))
            .arg(m_link.filterBad)
            .arg(m_link.filterIdentity)
            .arg(m_link.filterOld)
            .arg(m_link.filterLedger);
    line += QStringLiteral("\nRX rx_notify=%1 notify_age_ms=%2 rx_poll_drains=%3 rx_read_error=%4 "
                           "rx_socket_error=%5 rx_socket_state=%6 rx_socket_valid=%7 rx_pending=%8 "
                           "last_rx_error=%9")
                .arg(m_link.rxNotify)
                .arg(age(m_link.notifyAt))
                .arg(m_link.rxPollDrains)
                .arg(m_link.rxReadError)
                .arg(m_link.rxSocketError)
                .arg(int(m_output.state()))
                .arg(m_output.isValid() ? 1 : 0)
                .arg(m_output.hasPendingDatagrams() ? 1 : 0)
                .arg(m_link.lastRxError.isEmpty() ? QStringLiteral("none") : m_link.lastRxError);
    line += QStringLiteral("\n") + controlDiagnostic();
    emit statusChanged(line);
}
bool BoardConnection::sendCommand(Command &c) {
    if (!m_connected || !c.type)
        return false;
    auto raw = FptzProtocol::pack({c.type, m_session, c.txn, c.epoch, c.body});
    if (m_control.writeDatagram(raw, m_endpoints.boardAddress, m_endpoints.boardControlPort) !=
        raw.size()) {
        emit statusChanged(QStringLiteral("指令发送失败：") + m_control.errorString());
        return false;
    }
    c.last = MediaWire::nowMs();
    return true;
}
void BoardConnection::requestPose(double pan, double tilt, double hfov) {
    if (!m_connected || m_stopping)
        return;
    if (!std::isfinite(pan) || !std::isfinite(tilt) || !std::isfinite(hfov) ||
        std::abs(pan) > 180 || std::abs(tilt) > 90 ||
        (std::abs(hfov - 90) > 1e-6 && !(hfov == 360 && pan == 0 && tilt == 0))) {
        emit poseResolved(false, pan, tilt, hfov, m_epoch, 1);
        return;
    }
    if (m_poseCommand.type) {
        const bool replaced = m_queuedPose;
        m_queuedPose = true;
        m_queuedPan = pan;
        m_queuedTilt = tilt;
        m_queuedHfov = hfov;
        emit poseCommandQueued(replaced);
        return;
    }
    QByteArray body(12, 0);
    auto *b = reinterpret_cast<uchar *>(body.data());
    qToBigEndian<qint32>(qRound(pan * 10) * 100, b);
    qToBigEndian<qint32>(qRound(tilt * 10) * 100, b + 4);
    qToBigEndian<quint32>(qRound(hfov * 1000), b + 8);
    m_pendingPan = pan;
    m_pendingTilt = tilt;
    m_pendingHfov = hfov;
    m_poseCommand = {3, m_nextTxn++, body, MediaWire::nowMs(), 0, m_epoch};
    if (sendCommand(m_poseCommand))
        emit poseCommandSent(pan, tilt, hfov);
    emit statusChanged(QStringLiteral("PS正在生成LUT；旧有效视角继续播放"));
}
void BoardConnection::sendQueuedPose() {
    if (!m_queuedPose)
        return;
    m_queuedPose = false;
    if (m_queuedPan != m_pendingPan || m_queuedTilt != m_pendingTilt ||
        m_queuedHfov != m_pendingHfov)
        requestPose(m_queuedPan, m_queuedTilt, m_queuedHfov);
}
void BoardConnection::streamCommand(quint8 action) {
    QByteArray body(4, 0);
    body[0] = char(action);
    body[1] = action == 2 ? 0 : 2;
    m_streamCommand = {5, m_nextTxn++, body, MediaWire::nowMs(), 0, m_epoch};
    sendCommand(m_streamCommand);
}
void BoardConnection::returnTestCommand(quint8 action) {
    QByteArray body(4, 0);
    body[0] = char(action);
    m_returnCommand = {14, m_nextTxn++, body, MediaWire::nowMs(), 0, m_epoch};
    sendCommand(m_returnCommand);
}
void BoardConnection::capacityTestCommand(quint8 action) {
    QByteArray body(8, 0);
    body[0] = char(action);
    body[1] = char(m_capacityKind);
    qToBigEndian<quint32>(m_capacityTestId, reinterpret_cast<uchar *>(body.data()) + 4);
    m_capacityCommand = {16, m_nextTxn++, body, MediaWire::nowMs(), 0, m_epoch};
    sendCommand(m_capacityCommand);
}
void BoardConnection::startHostPsRateTest() {
    if (!m_connected || !m_streaming || m_paused || m_desiredPaused || m_stopping ||
        m_capacityJpeg.isEmpty()) {
        emit hostPsRateTestError(QStringLiteral("请先连接板卡并等待一帧有效JPEG"));
        return;
    }
    if (m_capacityRequested || m_capacityActive || m_returnRequested || m_returnActive ||
        m_returnAwaitingDecode)
        return;
    if (!m_upload.jpeg.isEmpty()) {
        if (m_latency)
            m_latency->status(m_upload.sampleId, "upload_aborted");
        m_sent.remove(m_upload.frame);
        m_upload = {};
    }
    m_capacityKind = 1;
    if (!++m_capacityTestId)
        ++m_capacityTestId;
    m_capacityRequested = true;
    m_desiredPaused = true;
    m_latestJpeg.clear();
    streamCommand(4);
}
void BoardConnection::stopHostPsRateTest() {
    if (m_capacityActive && m_capacityKind == 1 && !m_capacityCommand.type)
        capacityTestCommand(2);
    else if (m_capacityRequested && m_capacityKind == 1)
        finishCapacityTestError(QStringLiteral("测试在启动前已停止"));
}
void BoardConnection::startPsPipelineRateTest() {
    if (!m_connected || !m_streaming || m_paused || m_desiredPaused || m_stopping) {
        emit psPipelineRateTestError(QStringLiteral("请先连接板卡并保持视频运行"));
        return;
    }
    if (m_capacityRequested || m_capacityActive || m_returnRequested || m_returnActive ||
        m_returnAwaitingDecode)
        return;
    if (!m_upload.jpeg.isEmpty()) {
        if (m_latency)
            m_latency->status(m_upload.sampleId, "upload_aborted");
        m_sent.remove(m_upload.frame);
        m_upload = {};
    }
    m_capacityKind = 2;
    if (!++m_capacityTestId)
        ++m_capacityTestId;
    m_capacityRequested = true;
    m_desiredPaused = true;
    m_latestJpeg.clear();
    streamCommand(4);
}
void BoardConnection::stopPsPipelineRateTest() {
    if (m_capacityActive && m_capacityKind == 2 && !m_capacityCommand.type)
        capacityTestCommand(2);
    else if (m_capacityRequested && m_capacityKind == 2)
        finishCapacityTestError(QStringLiteral("测试在启动前已停止"));
}
void BoardConnection::finishCapacityTestError(const QString &reason) {
    const quint8 kind = m_capacityKind;
    const bool was = m_capacityRequested || m_capacityActive;
    m_capacityRequested = false;
    m_capacityActive = false;
    m_capacityCommand = {};
    m_capacityStage = 0;
    m_upload = {};
    m_nextBatch = MediaWire::nowMs();
    if (was) {
        if (kind == 1) {
            emit hostPsRateTestRunningChanged(false);
            emit hostPsRateTestError(reason);
        } else {
            emit psPipelineRateTestRunningChanged(false);
            emit psPipelineRateTestError(reason);
        }
    }
    m_desiredPaused = false;
    if (m_connected && m_streaming && m_paused && !m_streamCommand.type)
        streamCommand(5);
}
void BoardConnection::startPsHostRateTest() {
    if (!m_connected || !m_streaming || m_paused || m_desiredPaused || m_stopping) {
        emit psHostRateTestError(QStringLiteral("请先连接板卡并保持视频运行"));
        return;
    }
    if (m_returnRequested || m_returnActive || m_returnAwaitingDecode || m_returnCommand.type)
        return;
    if (!m_upload.jpeg.isEmpty()) {
        if (m_latency)
            m_latency->status(m_upload.sampleId, "upload_aborted");
        m_sent.remove(m_upload.frame);
        m_upload = {};
        m_nextBatch = MediaWire::nowMs();
    }
    m_returnRequested = true;
    m_desiredPaused = true;
    m_latestJpeg.clear();
    m_returnSample.clear();
    streamCommand(4);
}
void BoardConnection::stopPsHostRateTest() {
    if (m_returnActive && !m_returnCommand.type)
        returnTestCommand(2);
    else if (m_returnRequested)
        finishReturnTestError(QStringLiteral("测试在启动前已停止"));
}
void BoardConnection::finishPsHostDecodeCapacityTest() {
    if (!m_returnAwaitingDecode)
        return;
    m_returnAwaitingDecode = false;
    m_returnSample.clear();
    emit psHostRateTestRunningChanged(false);
    m_desiredPaused = false;
    if (m_connected && m_streaming && m_paused && !m_streamCommand.type)
        streamCommand(5);
}
void BoardConnection::finishReturnTestError(const QString &reason) {
    const bool was = m_returnRequested || m_returnActive || m_returnAwaitingDecode;
    m_returnRequested = false;
    m_returnActive = false;
    m_returnAwaitingDecode = false;
    m_returnCommand = {};
    m_returnCollector.clear();
    m_returnSample.clear();
    if (was)
        emit psHostRateTestRunningChanged(false);
    emit psHostRateTestError(reason);
    m_desiredPaused = false;
    if (m_connected && m_streaming && m_paused && !m_streamCommand.type)
        streamCommand(5);
}
void BoardConnection::setPaused(bool paused) {
    m_desiredPaused = paused;
    if (m_connected && m_streaming && !m_stopping)
        streamCommand(paused ? 4 : 5);
    if (paused)
        m_latestJpeg.clear();
}
void BoardConnection::stopAndDisconnect() {
    if (!m_connected) {
        disconnectFromBoard();
        return;
    }
    m_stopping = true;
    m_latestJpeg.clear();
    m_upload = {};
    m_poseCommand = {};
    streamCommand(2);
    emit statusChanged(QStringLiteral("断开中：等待PS停止视频"));
}
void BoardConnection::handleMediaControl(const FptzProtocol::Packet &p) {
    const auto *b = reinterpret_cast<const uchar *>(p.payload.constData());
    if (p.type == 4 && p.txn == m_poseCommand.txn && m_poseCommand.type &&
        (p.payload.size() == 8 ||
         ((p.payload.size() == 16 || p.payload.size() == 20) && b[0] == 2)) &&
        !b[2] && !b[3] && qFromBigEndian<quint32>(b + 4) == p.epoch) {
        m_lastRx.restart();
        if (b[0] == 1 && !b[1]) {
            emit poseAccepted(m_pendingPan, m_pendingTilt, m_pendingHfov);
        } else if (b[0] == 2 && !b[1]) {
            emit poseReady(m_pendingPan, m_pendingTilt, m_pendingHfov);
            if (p.payload.size() == 16 || p.payload.size() == 20) {
                const quint32 wallUs = qFromBigEndian<quint32>(b + 8);
                const quint32 workUs = qFromBigEndian<quint32>(b + 12);
                const quint32 anchorUs =
                    p.payload.size() == 20 ? qFromBigEndian<quint32>(b + 16) : quint32(-1);
                if (workUs <= wallUs && (anchorUs == quint32(-1) || anchorUs <= workUs))
                    emit poseLutProfile(m_pendingPan, m_pendingTilt, m_pendingHfov, wallUs, workUs,
                                        anchorUs);
            }
        } else if (b[0] == 3 && !b[1] && p.epoch) {
            m_epoch = p.epoch;
            m_poseCommand = {};
            m_free = 0;
            m_upload = {};
            m_collector.clear();
            m_link.heartbeatAt = m_link.creditAt = m_link.resultAt = -1;
            m_link.outputAt = -1;
            m_link.resultSeen = 0;
            emit poseResolved(true, m_pendingPan, m_pendingTilt, m_pendingHfov, m_epoch, 0);
            emit statusChanged(QStringLiteral("角度已生效 · epoch=%1").arg(m_epoch));
            if (!m_streaming)
                streamCommand(1);
            sendQueuedPose();
        } else if (b[0] == 4) {
            m_poseCommand = {};
            emit poseResolved(false, m_pendingPan, m_pendingTilt, m_pendingHfov, m_epoch, b[1]);
            emit statusChanged(QStringLiteral("PS拒绝角度，reason=%1；保留旧有效视角").arg(b[1]));
            sendQueuedPose();
        }
    } else if (p.type == 6 && p.txn == m_streamCommand.txn && m_streamCommand.type &&
               p.payload.size() == 4 && p.epoch >= m_streamCommand.epoch && p.epoch <= m_epoch &&
               b[0] == uchar(m_streamCommand.body[0]) && b[1] <= 1 && b[2] <= 2 && b[3] <= 1) {
        const int action = b[0];
        if ((action == 1 || action == 5) && (b[1] != 1 || b[3] != 0))
            return;
        if (action == 4 && (b[1] != 0 || b[3] != 1))
            return;
        if (action == 2 && (b[1] != 0 || b[3] != 0))
            return;
        m_lastRx.restart();
        m_streamCommand = {};
        if (action == 2) {
            const auto reason = m_disconnectReason;
            if (reason.isEmpty())
                disconnectFromBoard();
            else
                fail(reason);
            return;
        }
        m_streaming = true;
        m_paused = b[3];
        emit streamChanged(true, m_paused);
        emit statusChanged(m_paused ? QStringLiteral("PS暂停已确认")
                                    : QStringLiteral("视频运行中 · 等待/接收FPGA回图"));
        if (action == 4 && m_returnRequested && !m_returnCommand.type)
            returnTestCommand(1);
        else if (action == 4 && m_capacityRequested && !m_capacityCommand.type)
            capacityTestCommand(1);
        else if (m_paused != m_desiredPaused)
            streamCommand(m_desiredPaused ? 4 : 5);
    } else if (p.type == 15 && p.txn == m_returnCommand.txn && m_returnCommand.type &&
               p.payload.size() == 32 && p.epoch == m_epoch && !b[2] && !b[3]) {
        const quint8 action = b[0], state = b[1];
        m_lastRx.restart();
        m_returnCommand = {};
        if (action == 1 && state == 1) {
            m_returnRequested = false;
            m_returnActive = true;
            m_returnStartedMs = MediaWire::nowMs();
            m_returnLastProgress = 0;
            m_returnHostFrames = m_returnHostBytes = m_returnBad = 0;
            m_returnCollector.clear();
            m_returnSample.clear();
            emit psHostRateTestRunningChanged(true);
            emit psHostRateTestProgress(0, 10);
        } else if (action == 2 && state == 2 && m_returnActive) {
            const double elapsed = qMax(0.001, double(qFromBigEndian<quint32>(b + 8)) / 1000.0);
            const quint64 psFrames = qFromBigEndian<quint32>(b + 16),
                          psBytes = qFromBigEndian<quint32>(b + 20);
            const quint32 failures = qFromBigEndian<quint32>(b + 24);
            const quint64 missing =
                psFrames > m_returnHostFrames ? psFrames - m_returnHostFrames : 0;
            emit psHostRateTestCompleted(
                elapsed, m_returnHostFrames / elapsed, m_returnHostBytes / elapsed / 1000000.0,
                psFrames / elapsed, psBytes / elapsed / 1000000.0, m_returnHostFrames, psFrames,
                missing, quint64(m_returnCollector.partialCount()), m_returnBad, failures);
            m_returnActive = false;
            m_returnRequested = false;
            m_returnAwaitingDecode = true;
            m_returnCollector.clear();
            if (m_returnSample.isEmpty())
                finishReturnTestError(QStringLiteral("回传容量测试未得到可用于Host解码的完整JPEG"));
            else
                emit psHostCapacityJpegReady(m_returnSample);
        } else if (state == 3)
            finishReturnTestError(QStringLiteral("PS尚未缓存有效回图；请先让实时视频成功回传一帧"));
        else
            finishReturnTestError(QStringLiteral("PS当前状态不允许独立回传测试"));
    } else if (p.type == 17 && p.payload.size() == 84 && p.epoch == m_epoch && m_epoch) {
        const quint8 action = b[0], state = b[1], kind = b[2], stage = b[3];
        const quint32 testId = qFromBigEndian<quint32>(b + 4);
        if (testId != m_capacityTestId || kind != m_capacityKind)
            return;
        m_lastRx.restart();
        m_capacityCommand = {};
        m_capacityStage = stage;
        if (state == 1) {
            m_capacityRequested = false;
            m_capacityActive = true;
            m_capacityStartedMs = MediaWire::nowMs();
            m_capacityLastProgress = 0;
            m_capacityBase = {m_link.txAttempt,  m_link.txComplete,        m_link.txBytes,
                              m_link.txTimeout,  m_link.txStall,           m_link.txFail,
                              m_link.txReplaced, m_link.txDurationMsTotal, 0,
                              m_link.psDrop[0],  m_link.psDrop[1],         m_link.psDrop[2],
                              m_link.psDrop[3]};
            if (kind == 1) {
                emit hostPsRateTestRunningChanged(true);
                emit hostPsRateTestProgress(0, 5);
            } else {
                emit psPipelineRateTestRunningChanged(true);
                emit psPipelineRateTestProgress(0, 15);
            }
        } else if (state == 2 && m_capacityActive && kind == 2) {
            const int done = qBound(0, int(stage - 1) * 5, 15);
            emit psPipelineRateTestProgress(done, 15);
        } else if (state == 3 && m_capacityActive) {
            if (kind == 1) {
                const double elapsed =
                    qMax(0.001, double(qFromBigEndian<quint32>(b + 12)) / 1000.0);
                const quint64 hostFrames = m_link.txComplete - m_capacityBase.frames;
                const quint64 hostBytes = m_link.txBytes - m_capacityBase.bytes;
                const quint64 attempts = m_link.txAttempt - m_capacityBase.attempts;
                const quint64 psFrames = qFromBigEndian<quint32>(b + 20);
                const quint64 psBytes = qFromBigEndian<quint32>(b + 24);
                const quint32 failures = qFromBigEndian<quint32>(b + 28);
                const quint32 timeouts = qFromBigEndian<quint32>(b + 32);
                const quint64 duration = m_link.txDurationMsTotal - m_capacityBase.durationTotal;
                emit hostPsRateTestCompleted(
                    elapsed, hostFrames / elapsed, hostBytes / elapsed / 1000000.0,
                    psFrames / elapsed, psBytes / elapsed / 1000000.0, hostFrames, psFrames,
                    hostFrames > psFrames ? hostFrames - psFrames : 0, timeouts, failures, 0, 0,
                    attempts, m_link.txTimeout - m_capacityBase.timeouts,
                    m_link.txFail - m_capacityBase.failures,
                    m_link.txReplaced - m_capacityBase.replacements,
                    hostFrames ? double(duration) / double(hostFrames) : 0.0,
                    m_capacityBase.maxDuration, m_link.txStall - m_capacityBase.stalls);
                emit hostPsRateTestRunningChanged(false);
            } else {
                PsPipelineRateResult r;
                for (int i = 0; i < 3; ++i) {
                    const int o = 12 + i * 24;
                    const double elapsed =
                        qMax(0.001, double(qFromBigEndian<quint32>(b + o)) / 1000.0);
                    const quint64 attempted = qFromBigEndian<quint32>(b + o + 4);
                    const quint64 completed = qFromBigEndian<quint32>(b + o + 8);
                    const quint64 failed = qFromBigEndian<quint32>(b + o + 16);
                    const quint64 timeout = qFromBigEndian<quint32>(b + o + 20);
                    if (i == 0) {
                        r.elapsedSeconds = elapsed;
                        r.decodeStarted = attempted;
                        r.decodeCompleted = completed;
                        r.decodeBoundaryLoss = attempted > completed ? attempted - completed : 0;
                        r.decodeFailed = failed;
                        r.decodeTimeout = timeout;
                        r.decodeFps = completed / elapsed;
                    } else if (i == 1) {
                        r.correctStarted = attempted;
                        r.correctCompleted = completed;
                        r.correctBoundaryLoss = attempted > completed ? attempted - completed : 0;
                        r.correctFailed = failed;
                        r.correctTimeout = timeout;
                        r.correctFps = completed / elapsed;
                    } else {
                        r.encodeStarted = attempted;
                        r.encodeCompleted = completed;
                        r.encodeBoundaryLoss = attempted > completed ? attempted - completed : 0;
                        r.encodeFailed = failed;
                        r.encodeTimeout = timeout;
                        r.encodeFps = completed / elapsed;
                    }
                }
                emit psPipelineRateTestCompleted(r);
                emit psPipelineRateTestRunningChanged(false);
            }
            m_capacityActive = false;
            m_capacityRequested = false;
            m_capacityStage = 0;
            m_upload = {};
            m_nextBatch = MediaWire::nowMs();
            m_desiredPaused = false;
            if (m_streaming && m_paused && !m_streamCommand.type)
                streamCommand(5);
        } else if (state == 4)
            finishCapacityTestError(
                QStringLiteral("PS没有可用于隔离压测的有效帧，请先运行实时视频"));
        else if (state == 5)
            finishCapacityTestError(QStringLiteral("PS当前状态不允许容量测试"));
        Q_UNUSED(action);
    } else if (p.type == 9 && p.payload.size() == 12 && p.epoch == m_epoch && m_epoch) {
        const auto free = qFromBigEndian<quint16>(b), total = qFromBigEndian<quint16>(b + 2);
        const auto seq = qFromBigEndian<quint32>(b + 8);
        if (!qFromBigEndian<quint16>(b + 6) && total == m_slotCount && free <= total &&
            qFromBigEndian<quint16>(b + 4) <= total && seq > m_creditSeq) {
            m_creditSeq = seq;
            m_free = free;
            m_link.psFree = free;
            m_link.creditAt = MediaWire::nowMs();
        }
    } else if (p.type == 10 && (p.payload.size() == 256 || p.payload.size() == 280) &&
               p.epoch == m_epoch && m_epoch) {
        if (m_latency)
            m_latency->counters({{"ps_ready_replaced", qFromBigEndian<quint32>(b + 80)}});
        const auto text =
            m_psFlow.sample(p.payload, m_link.txReplaced, m_link.txTimeout, m_link.txFail);
        if (!text.isEmpty())
            emit psFlowWindow(text);
    } else if (p.type == 10 && p.payload.size() == 152 && p.epoch == m_epoch && m_epoch &&
               qFromBigEndian<quint32>(b + 96) == 0x57414954u) {
        {
            const auto count = qFromBigEndian<quint32>(b + 100);
            quint64 sums[6];
            bool valid = m_psWaitBaseline && count >= m_psWaitSamples;
            for (int i = 0; i < 6; ++i) {
                sums[i] = qFromBigEndian<quint64>(b + 104 + i * 8);
                valid = valid && sums[i] >= m_psWaitSum[i];
            }
            const auto n = valid ? count - m_psWaitSamples : 0;
            auto average = [&](int i) {
                return n ? double(sums[i] - m_psWaitSum[i]) / n / 1000. : 0.;
            };
            emit psWaitWindow(true, n, average(1), average(2), average(3));
            if (m_latency && n) {
                const QString names[] = {"ps_rx",   "ps_ready", "ps_pipe",
                                         "ps_outq", "ps_tx",    "ps_total"};
                for (int i = 0; i < 6; ++i)
                    m_latency->legacy(names[i], average(i));
            }
            std::copy(std::begin(sums), std::end(sums), std::begin(m_psWaitSum));
            m_psWaitSamples = count;
            m_psWaitBaseline = true;
        }
    } else if (p.type == 10 &&
               (p.payload.size() == 24 || p.payload.size() == 32 || p.payload.size() == 80 ||
                p.payload.size() == 96) &&
               p.epoch == m_epoch && m_epoch) {
        m_link.psRx = qFromBigEndian<quint32>(b);
        m_link.psTx = qFromBigEndian<quint32>(b + 4);
        for (int i = 0; i < 4; ++i)
            m_link.psDrop[i] = qFromBigEndian<quint32>(b + 8 + i * 4);
        if (p.payload.size() >= 32) {
            m_link.psCompleteFrames = qFromBigEndian<quint32>(b + 24);
            m_link.psCompleteBytes = qFromBigEndian<quint32>(b + 28);
            emit psReceiveCountersChanged(m_link.psCompleteFrames, m_link.psCompleteBytes,
                                          m_link.psDrop[0], m_link.psDrop[1], m_link.psDrop[2],
                                          m_link.psDrop[3]);
        }
        if (p.payload.size() >= 80) {
            quint32 v[12];
            for (int i = 0; i < 12; ++i)
                v[i] = qFromBigEndian<quint32>(b + 32 + i * 4);
            if (m_latency)
                m_latency->counters({{"ps_decode_completed", v[1]},
                                     {"ps_correct_completed", v[5]},
                                     {"ps_encode_completed", v[9]}});
            emit psPipelineCountersChanged(v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7], v[8],
                                           v[9], v[10], v[11]);
        }
        if (p.payload.size() >= 96) {
            m_link.psOutputCompleteFrames = qFromBigEndian<quint32>(b + 80);
            m_link.psOutputCompleteBytes = qFromBigEndian<quint32>(b + 84);
            m_link.psOutputSuperseded = qFromBigEndian<quint32>(b + 88);
            m_link.psOutputSendDrop = qFromBigEndian<quint32>(b + 92);
            emit psOutputCountersChanged(m_link.psOutputCompleteFrames,
                                         m_link.psOutputCompleteBytes, m_link.psOutputSuperseded,
                                         m_link.psOutputSendDrop);
        }
        if (!m_psWaitBaseline)
            emit psWaitWindow(false, 0, 0, 0, 0);
        if (m_latency)
            m_latency->counters({{"ps_input_incomplete", m_link.psDrop[0]},
                                 {"ps_input_crc", m_link.psDrop[1]},
                                 {"ps_stale_epoch", m_link.psDrop[2]},
                                 {"ps_no_slot", m_link.psDrop[3]},
                                 {"ps_output_replaced", m_link.psOutputSuperseded},
                                 {"ps_output_failed", m_link.psOutputSendDrop}});
        m_link.heartbeatAt = MediaWire::nowMs();
    } else if (p.type == 8 && p.payload.size() == 160 && p.epoch == m_epoch && m_epoch) {
        if (m_latency)
            m_latency->board(p.session, p.epoch, p.frame, p.payload);
    } else if (p.type == 8 && p.payload.size() == 48 && p.epoch == m_epoch && m_epoch &&
               !qFromBigEndian<quint32>(b)) {
        const auto now = MediaWire::nowMs();
        if (m_stageWaitWindowAt < 0)
            m_stageWaitWindowAt = now;
        ++m_stageWaitFrames;
        for (int i = 0; i < 3; ++i)
            m_stageWaitUs[i] += qFromBigEndian<quint32>(b + 36 + i * 4);
        if (now - m_stageWaitWindowAt >= 1000) {
            emit psStageWaitWindow(m_stageWaitFrames,
                                   double(m_stageWaitUs[0]) / m_stageWaitFrames / 1000.,
                                   double(m_stageWaitUs[1]) / m_stageWaitFrames / 1000.,
                                   double(m_stageWaitUs[2]) / m_stageWaitFrames / 1000.);
            m_stageWaitFrames = 0;
            std::fill(std::begin(m_stageWaitUs), std::end(m_stageWaitUs), quint64(0));
            m_stageWaitWindowAt = now;
        }
    } else if (p.type == 8 && p.payload.size() == 36 && p.epoch == m_epoch && m_epoch &&
               !qFromBigEndian<quint32>(b)) {
        for (int i = 0; i < 3; ++i)
            m_link.stageUs[i] = qFromBigEndian<quint32>(b + 4 + i * 4);
        emit stageTimesChanged(m_link.stageUs[0], m_link.stageUs[1], m_link.stageUs[2]);
        if (m_latency) {
            m_latency->legacyCycles(p.session, p.epoch, p.frame, qFromBigEndian<quint32>(b + 24));
            m_latency->legacy("ps_front", qFromBigEndian<quint32>(b + 28) / 1000.);
            m_latency->legacy("ps_rear", qFromBigEndian<quint32>(b + 32) / 1000.);
            m_latency->legacy("ps_decode", m_link.stageUs[0] / 1000.);
            m_latency->legacy("ps_correct", m_link.stageUs[1] / 1000.);
            m_latency->legacy("ps_encode", m_link.stageUs[2] / 1000.);
        }
        m_link.inputBytes = qFromBigEndian<quint32>(b + 16);
        m_link.outputBytes = qFromBigEndian<quint32>(b + 20);
        ++m_link.resultSeen;
        m_link.resultAt = MediaWire::nowMs();
    } else if (p.type == 11 && p.payload.size() == 8) {
        if (!p.txn && p.epoch == m_epoch && m_epoch &&
            qFromBigEndian<quint16>(b) == FptzProtocol::HardwareFault && b[2] >= 1 && b[2] <= 3) {
            const auto frame = qFromBigEndian<quint32>(b + 4);
            InputRecord record;
            for (auto i = m_recentInput.crbegin(); i != m_recentInput.crend(); ++i)
                if (i->frame == frame && i->epoch == p.epoch) {
                    record = *i;
                    break;
                }
            const auto directory = m_endpoints.diagnosticsDirectory;
            const auto stage = b[2], reason = b[3];
            const QString stageName = stage == 1   ? QStringLiteral("JPEG解码")
                                      : stage == 2 ? QStringLiteral("校正拼接")
                                                   : QStringLiteral("JPEG编码");
            QString message = QStringLiteral("板端%1%2，帧%3；已停止上传，请重启板卡")
                                  .arg(stageName)
                                  .arg(reason & 1 ? QStringLiteral("超时") : QStringLiteral("故障"))
                                  .arg(frame);
            if (stage == 1)
                message += reason & 2 ? QStringLiteral(" · 后镜头") : QStringLiteral(" · 前镜头");
            fail(message); // Stop sockets/timers and clear outgoing frames before disk I/O.
            const auto saved = saveFaultInput(record, frame, stage, reason, directory);
            emit statusChanged(
                message + (saved.isEmpty() ? QStringLiteral(" · 故障样本保存失败")
                                           : QStringLiteral(" · 样本：") + saved +
                                                 (record.jpeg.isEmpty()
                                                      ? QStringLiteral("（该帧未缓存，仅有元数据）")
                                                      : QString())));
            return;
        }
        if (p.txn == m_poseCommand.txn && m_poseCommand.type) {
            m_poseCommand = {};
            emit poseResolved(false, m_pendingPan, m_pendingTilt, m_pendingHfov, m_epoch,
                              qFromBigEndian<quint16>(b));
            sendQueuedPose();
        }
        if (p.txn == m_streamCommand.txn && m_streamCommand.type) {
            if (m_stopping) {
                fail(QStringLiteral("PS拒绝STOP，端口已释放；重新连接前请确认PS停止或复位"));
                return;
            }
            m_streamCommand = {};
            m_streaming = false;
            emit streamChanged(false, false);
        }
    }
}
void BoardConnection::submitJpeg(const QByteArray &jpeg, qint64 capturedMs,
                                 quint32 cameraTimestampMs, quint64 sampleId) {
    if (!m_connected || m_stopping || !m_epoch || !MediaWire::validInput(jpeg)) {
        if (m_latency)
            m_latency->status(sampleId, "upload_rejected");
        return;
    }
    m_capacityJpeg = jpeg;
    if (m_desiredPaused) {
        if (m_latency)
            m_latency->status(sampleId, "upload_paused");
        return;
    }
    if (!m_latestJpeg.isEmpty()) {
        if (m_latency)
            m_latency->status(m_latestSample, "upload_queue_replaced");
        ++m_link.txReplaced;
        emitUploadCounters();
    }
    m_latestJpeg = jpeg;
    m_latestCapture = capturedMs;
    m_latestSample = sampleId;
    m_latestCameraTimestamp = cameraTimestampMs;
}
void BoardConnection::pumpMedia() {
    if (!m_connected)
        return;
    const auto now = MediaWire::nowMs();
    const bool observing = m_streaming && !m_paused && !m_desiredPaused && !m_stopping &&
                           !m_capacityActive && !m_capacityRequested && !m_returnRequested &&
                           !m_returnActive && !m_returnAwaitingDecode;
    if (m_latency && (observing != m_latencyActive || m_epoch != m_latencyEpoch)) {
        m_latency->setActive(observing,
                             observing ? "stream_or_epoch_changed" : "paused_or_capacity_test");
        m_latencyActive = observing;
        m_latencyEpoch = m_epoch;
    }
    UploadWaitMeter::State state = UploadWaitMeter::Suspended;
    if (m_streaming && !m_paused && !m_desiredPaused && !m_stopping && !m_capacityActive &&
        !m_capacityRequested && !m_returnRequested && !m_returnActive && !m_returnAwaitingDecode) {
        state = !m_upload.jpeg.isEmpty() ? UploadWaitMeter::Sending
                : m_latestJpeg.isEmpty() ? UploadWaitMeter::Source
                : !m_free                ? UploadWaitMeter::Credit
                                         : UploadWaitMeter::Ready;
    }
    UploadWaitMeter::Window waitWindow;
    if (m_uploadWait.sample(now, state, waitWindow)) {
        emit uploadWaitWindow(
            waitWindow.ms[UploadWaitMeter::Credit], waitWindow.activeMs(),
            waitWindow.ms[UploadWaitMeter::Sending], waitWindow.ms[UploadWaitMeter::Source],
            waitWindow.ms[UploadWaitMeter::Ready], waitWindow.longestCreditMs, m_free, m_slotCount);
    }
    if (m_stopping && m_streamCommand.type && now - m_streamCommand.started >= 1200) {
        fail(QStringLiteral("STOP未确认，端口已释放；重新连接前请确认PS停止或复位"));
        return;
    }
    for (auto *c : {&m_poseCommand, &m_streamCommand, &m_returnCommand, &m_capacityCommand})
        if (c->type) {
            if (now - c->started > m_endpoints.commandTimeoutMs) {
                if (c == &m_poseCommand) {
                    emit poseResolved(false, m_pendingPan, m_pendingTilt, m_pendingHfov, m_epoch,
                                      255);
                    m_disconnectReason = QStringLiteral(
                        "角度确认超时：PS实际角度/epoch未知，会话已停止，请重新连接");
                    stopAndDisconnect();
                    emit statusChanged(m_disconnectReason);
                    return;
                } else if (c == &m_returnCommand) {
                    finishReturnTestError(QStringLiteral("PS→Host测试指令确认超时"));
                    return;
                } else if (c == &m_capacityCommand) {
                    finishCapacityTestError(
                        QStringLiteral("PS程序不支持隔离容量测试或指令确认超时"));
                    return;
                } else {
                    m_streaming = false;
                    emit streamChanged(false, false);
                    emit statusChanged(QStringLiteral("流控确认超时"));
                }
                *c = {};
            } else if (now - c->last >= 500)
                sendCommand(*c);
        }
    if (m_returnActive) {
        const int elapsed = int((now - m_returnStartedMs) / 1000);
        if (elapsed != m_returnLastProgress) {
            m_returnLastProgress = elapsed;
            emit psHostRateTestProgress(qMin(elapsed, 10), 10);
        }
        if (now - m_returnStartedMs >= 5000 && !m_returnCommand.type)
            returnTestCommand(2);
    }
    if (m_capacityActive) {
        const int elapsed = int((now - m_capacityStartedMs) / 1000);
        const int total = m_capacityKind == 1 ? 5 : 15;
        if (elapsed != m_capacityLastProgress) {
            m_capacityLastProgress = elapsed;
            if (m_capacityKind == 1)
                emit hostPsRateTestProgress(qMin(elapsed, total), total);
            else
                emit psPipelineRateTestProgress(qMin(elapsed, total), total);
        }
    }
    for (auto i = m_sent.begin(); i != m_sent.end();) {
        if (now - i->started >= 3000) {
            if (m_latency)
                m_latency->status(i->sampleId, "return_timeout");
            i = m_sent.erase(i);
        } else
            ++i;
    }
    const bool capacityInput = m_capacityActive && m_capacityKind == 1;
    const int batchPackets = qBound(1, m_endpoints.uploadBatchPackets, 32);
    if (!m_streaming || ((!capacityInput) && (m_paused || m_desiredPaused)) || m_stopping)
        return;
    if (m_upload.jpeg.isEmpty() && (capacityInput || !m_latestJpeg.isEmpty()) && m_free > 0 &&
        now >= m_nextBatch) {
        auto jpeg = capacityInput ? m_capacityJpeg : std::move(m_latestJpeg);
        if (!capacityInput)
            m_latestJpeg.clear();
        const int count = (jpeg.size() + 1399) / 1400;
        if (((count + batchPackets - 1) / batchPackets - 1) >= 150) {
            if (m_latency && !capacityInput)
                m_latency->status(m_latestSample, "upload_budget_exceeded");
            emit statusChanged(QStringLiteral("JPEG过大，上传预算超过150ms"));
            return;
        }
        if (m_sent.size() >= 128) {
            if (m_latency && !capacityInput)
                m_latency->status(m_latestSample, "upload_ledger_full");
            return;
        }
        const auto crc = FptzProtocol::crc32(jpeg);
        m_upload = {jpeg,
                    ++m_frameId,
                    m_epoch,
                    crc,
                    0,
                    count,
                    now,
                    m_latestCapture,
                    0,
                    m_latestCameraTimestamp,
                    capacityInput ? 0 : m_latestSample};
        if (m_latency && !capacityInput)
            m_latency->bind(m_upload.sampleId, m_session, m_epoch, m_frameId);
        --m_free;
        ++m_link.txAttempt;
        emitUploadCounters();
        if (!capacityInput)
            m_sent.insert(m_frameId, {m_epoch, m_latestCapture, now, m_upload.sampleId});
    }
    if (m_upload.jpeg.isEmpty() || now < m_nextBatch)
        return;
    if (now - m_upload.started >= 150) {
        ++m_link.txTimeout;
        emitUploadCounters();
        const QString diagnostic =
            QStringLiteral(
                "一帧上传超时已丢弃，等待最新帧；session=%1 frame=%2 epoch=%3 sent_fragments=%4/%5 "
                "jpeg_bytes=%6 elapsed_ms=%7 deadline_ms=150 dst=%8:%9")
                .arg(m_session)
                .arg(m_upload.frame)
                .arg(m_upload.epoch)
                .arg(m_upload.index)
                .arg(m_upload.count)
                .arg(m_upload.jpeg.size())
                .arg(now - m_upload.started)
                .arg(m_endpoints.boardAddress.toString())
                .arg(m_endpoints.boardInputPort);
        if (m_latency)
            m_latency->status(m_upload.sampleId, "upload_aborted");
        m_sent.remove(m_upload.frame);
        m_upload = {};
        emit statusChanged(diagnostic);
        return;
    }
    for (int n = 0; n < batchPackets && m_upload.index < m_upload.count; ++n) {
        const auto &u = m_upload;
        const auto raw = MediaWire::pack({0x23, m_session, u.frame, u.epoch, quint32(u.jpeg.size()),
                                          u.crc, quint16(u.index), quint16(u.count),
                                          u.jpeg.mid(u.index * 1400, 1400), u.cameraTimestampMs});
        const qint64 writeAt = LatencyTracker::nowUs();
        const qint64 written =
            m_input.writeDatagram(raw, m_endpoints.boardAddress, m_endpoints.boardInputPort);
        if (written != raw.size()) {
            // Snapshot before resetting Upload or inspecting other socket properties.
            const int error = int(m_input.error());
            const QString errorText = m_input.errorString();
            if (handleUploadWriteFailure(m_input.error(), errorText, MediaWire::nowMs()))
                break;
            const QString diagnostic =
                QStringLiteral(
                    "视频发送失败，本帧丢弃不重传；qt_error=%1 error_text=\"%2\" socket_state=%3 "
                    "written=%4 expected=%5 session=%6 frame=%7 epoch=%8 fragment=%9/%10 "
                    "sent_fragments=%11 jpeg_bytes=%12 elapsed_ms=%13 src=%14:%15 dst=%16:%17")
                    .arg(error)
                    .arg(errorText)
                    .arg(int(m_input.state()))
                    .arg(written)
                    .arg(raw.size())
                    .arg(m_session)
                    .arg(u.frame)
                    .arg(u.epoch)
                    .arg(u.index + 1)
                    .arg(u.count)
                    .arg(u.index)
                    .arg(u.jpeg.size())
                    .arg(MediaWire::nowMs() - u.started)
                    .arg(m_input.localAddress().toString())
                    .arg(m_input.localPort())
                    .arg(m_endpoints.boardAddress.toString())
                    .arg(m_endpoints.boardInputPort);
            if (m_latency)
                m_latency->status(u.sampleId, "upload_failed");
            m_sent.remove(u.frame);
            m_upload = {};
            emit statusChanged(diagnostic);
            break;
        }
        if (!m_upload.index && m_latency)
            m_latency->mark(m_upload.sampleId, "tx_first", writeAt);
        ++m_upload.index;
    }
    m_nextBatch = MediaWire::nowMs() + 1;
    if (!m_upload.jpeg.isEmpty() && m_upload.index == m_upload.count) {
        if (m_latency)
            m_latency->mark(m_upload.sampleId, "tx_end");
        const quint64 durationMs = quint64(qMax<qint64>(0, MediaWire::nowMs() - m_upload.started));
        ++m_link.txComplete;
        m_link.txBytes += quint64(m_upload.jpeg.size());
        m_link.txDurationMsTotal += durationMs;
        m_link.txLastDurationMs = durationMs;
        if (capacityInput)
            m_capacityBase.maxDuration = qMax(m_capacityBase.maxDuration, durationMs);
        if (!capacityInput) {
            m_recentInput.append({m_upload.jpeg, m_upload.frame, m_upload.epoch,
                                  m_upload.cameraTimestampMs, m_upload.crc});
            while (m_recentInput.size() > 8)
                m_recentInput.removeFirst();
        }
        emitUploadCounters(durationMs);
        emit frameSent(m_upload.frame);
        emit frameUploaded(m_upload.frame, quint32(m_upload.jpeg.size()));
        m_upload = {};
    }
}
void BoardConnection::receiveOutput() {
    if (m_outputReading || !m_connected)
        return;
    QScopedValueRollback<bool> reading(m_outputReading, true);
    const auto generation = m_outputReadGeneration;
    int processed = 0;
    while (m_connected && generation == m_outputReadGeneration && processed < 64 &&
           m_output.state() == QAbstractSocket::BoundState && m_output.hasPendingDatagrams()) {
        const auto pending = m_output.pendingDatagramSize();
        // If a size probe fails, let readDatagram report/consume the actual error.
        QByteArray raw(pending < 0 ? 2048 : int(qMin<qint64>(pending, 2048)), 0);
        QHostAddress sender;
        quint16 port = 0;
        const auto bytes = m_output.readDatagram(raw.data(), raw.size(), &sender, &port);
        if (bytes < 0) {
            ++m_link.rxReadError;
            m_link.lastRxError = QStringLiteral("readDatagram qt_error=%1 error_text=\"%2\"")
                                     .arg(int(m_output.error()))
                                     .arg(m_output.errorString());
            // Do not spin/requeue failed reads. The next timer tick can retry.
            break;
        }
        ++processed;
        if (pending < 0)
            raw.resize(int(bytes));
        if (!m_connected || sender != m_endpoints.boardAddress ||
            port != m_endpoints.boardOutputPort)
            continue;
        ++m_link.rxPackets;
        MediaWire::Chunk c;
        if (bytes != raw.size() || !MediaWire::parse(raw, &c) || c.type != 0x21) {
            ++m_link.filterBad;
            continue;
        }
        if (c.session != m_session || c.epoch != m_epoch) {
            ++m_link.filterIdentity;
            continue;
        }
        if ((c.frame & 0x80000000u) && m_returnActive) {
            ++m_link.accepted;
            auto jpeg = m_returnCollector.add(c, MediaWire::nowMs());
            if (jpeg.isEmpty())
                continue;
            ++m_returnHostFrames;
            m_returnHostBytes += quint64(jpeg.size());
            m_returnSample = jpeg;
            continue;
        }
        if (c.frame <= m_lastOutputFrame) {
            ++m_link.filterOld;
            continue;
        }
        if (!m_sent.contains(c.frame) || m_sent[c.frame].epoch != c.epoch) {
            ++m_link.filterLedger;
            continue;
        }
        ++m_link.accepted;
        auto jpeg = m_collector.add(c, MediaWire::nowMs());
        if (jpeg.isEmpty())
            continue;
        const auto info = m_sent.take(c.frame);
        if (m_latency)
            m_latency->mark(info.sampleId, "rx");
        m_lastRx.restart();
        m_lastOutputFrame = c.frame;
        ++m_link.jpegComplete;
        m_link.outputAt = MediaWire::nowMs();
        emit jpegReady(jpeg, c.frame, c.epoch, double(MediaWire::nowMs() - info.captured));
    }
    if (processed == 64 && m_connected && generation == m_outputReadGeneration &&
        !m_outputDrainQueued && m_output.state() == QAbstractSocket::BoundState &&
        m_output.hasPendingDatagrams()) {
        m_outputDrainQueued = true;
        QTimer::singleShot(0, this, [this, generation] {
            if (generation != m_outputReadGeneration)
                return;
            m_outputDrainQueued = false;
            receiveOutput();
        });
    }
}
