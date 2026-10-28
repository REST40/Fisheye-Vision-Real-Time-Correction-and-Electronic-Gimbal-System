#include "boardconnection.h"
#include "fptzprotocol.h"
#include <QRandomGenerator>
#include <QScopedValueRollback>
#include <QVariant>
#include <QtEndian>
#ifdef Q_OS_WIN
#include <winsock2.h>
#endif

BoardConnection::BoardConnection(QObject *parent) : QObject(parent) {
    m_timer.setInterval(25);
    connect(&m_timer, &QTimer::timeout, this, &BoardConnection::tick);
    connect(&m_control, &QUdpSocket::readyRead, this, [this] {
        ++m_link.ctrlNotify;
        m_link.ctrlNotifyAt = MediaWire::nowMs();
        receiveControl();
    });
    connect(&m_control, &QUdpSocket::errorOccurred, this,
            [this](QAbstractSocket::SocketError error) {
                if (!m_connected && !m_connecting)
                    return;
                ++m_link.ctrlSocketError;
                m_link.lastCtrlError = QStringLiteral("qt_error=%1 error_text=\"%2\"")
                                           .arg(int(error))
                                           .arg(m_control.errorString());
            });
    connect(&m_output, &QUdpSocket::readyRead, this, [this] {
        ++m_link.rxNotify;
        m_link.notifyAt = MediaWire::nowMs();
        receiveOutput();
    });
    connect(&m_output, &QUdpSocket::errorOccurred, this,
            [this](QAbstractSocket::SocketError error) {
                if (!m_connected)
                    return;
                ++m_link.rxSocketError;
                m_link.lastRxError = QStringLiteral("qt_error=%1 error_text=\"%2\"")
                                         .arg(int(error))
                                         .arg(m_output.errorString());
            });
    connect(&m_input, &QUdpSocket::readyRead, this, [this] {
        int budget = 32;
        while (m_input.state() == QAbstractSocket::BoundState && m_input.hasPendingDatagrams() &&
               budget-- > 0)
            m_input.readDatagram(nullptr, 0);
    });
    m_mediaTimer.setTimerType(Qt::PreciseTimer);
    m_mediaTimer.setInterval(1);
    connect(&m_mediaTimer, &QTimer::timeout, this, &BoardConnection::pumpMedia);
}

void BoardConnection::closeSockets() {
    m_timer.stop();
    m_control.close();
    m_input.close();
    m_output.close();
    m_mediaTimer.stop();
    resetMedia();
    const bool wasConnected = m_connected;
    m_connected = false;
    m_connecting = false;
    m_session = 0;
    if (wasConnected)
        emit connectedChanged(false);
}

void BoardConnection::fail(const QString &message) {
    closeSockets();
    emit statusChanged(message);
}

void BoardConnection::connectToBoard(const BoardEndpoints &endpoints) {
    closeSockets();
    m_endpoints = endpoints;
    if (endpoints.localAddress.protocol() != QAbstractSocket::IPv4Protocol ||
        endpoints.boardAddress.protocol() != QAbstractSocket::IPv4Protocol ||
        endpoints.localAddress.isNull() || endpoints.boardAddress.isNull() ||
        endpoints.localAddress == QHostAddress::AnyIPv4 ||
        endpoints.boardAddress == QHostAddress::AnyIPv4 ||
        endpoints.boardAddress == QHostAddress::Broadcast || endpoints.localAddress.isMulticast() ||
        endpoints.boardAddress.isMulticast() || !endpoints.boardControlPort ||
        endpoints.helloTimeoutMs <= 0 || endpoints.idleTimeoutMs <= 0 ||
        endpoints.retryIntervalMs <= 0 || endpoints.commandTimeoutMs <= 0) {
        fail(QStringLiteral("无效的 IPv4 地址或连接配置"));
        return;
    }
    const QList<quint16> ports{endpoints.controlPort, endpoints.inputPort, endpoints.outputPort};
    const QList<QUdpSocket *> sockets{&m_control, &m_input, &m_output};
    for (int i = 0; i < sockets.size(); ++i) {
        if (!sockets[i]->bind(endpoints.localAddress, ports[i], QUdpSocket::DontShareAddress)) {
            const QString error = sockets[i]->errorString();
            fail(QStringLiteral("绑定 %1:%2 失败：%3；检查网卡 IP、端口占用及其他测试程序")
                     .arg(endpoints.localAddress.toString())
                     .arg(ports[i])
                     .arg(error));
            return;
        }
        sockets[i]->setSocketOption(QAbstractSocket::ReceiveBufferSizeSocketOption,
                                    4 * 1024 * 1024);
        if (sockets[i] == &m_input) {
            sockets[i]->setSocketOption(QAbstractSocket::SendBufferSizeSocketOption,
                                        4 * 1024 * 1024);
#ifdef Q_OS_WIN
            const int sendBufferBytes = 4 * 1024 * 1024;
            (void)::setsockopt(SOCKET(m_input.socketDescriptor()), SOL_SOCKET, SO_SNDBUF,
                               reinterpret_cast<const char *>(&sendBufferBytes),
                               sizeof(sendBufferBytes));
#endif
        }
    }
    m_helloTxn = QRandomGenerator::global()->bounded(1u, 0x7ffffff0u);
    m_nextTxn = m_helloTxn + 1;
    m_connecting = true;
    m_elapsed.start();
    m_lastHelloMs = 0;
    emit statusChanged(QStringLiteral("连接中：等待 %1:%2 的 HELLO_ACK（v3）")
                           .arg(endpoints.boardAddress.toString())
                           .arg(endpoints.boardControlPort));
    sendHello();
    if (m_connecting)
        m_timer.start();
}

void BoardConnection::sendHello() {
    const auto bytes =
        FptzProtocol::pack({1, 0, m_helloTxn, 0, QByteArray::fromHex("0578004000040000")});
    if (m_control.writeDatagram(bytes, m_endpoints.boardAddress, m_endpoints.boardControlPort) !=
        bytes.size()) {
        fail(QStringLiteral("发送 HELLO 失败：") + m_control.errorString());
        return;
    }
    m_lastHelloMs = m_elapsed.elapsed();
}

void BoardConnection::receiveControl() {
    if (m_controlReading || (!m_connected && !m_connecting))
        return;
    QScopedValueRollback<bool> reading(m_controlReading, true);
    const auto generation = m_controlReadGeneration;
    int processed = 0;
    while ((m_connected || m_connecting) && generation == m_controlReadGeneration &&
           processed < 32 && m_control.state() == QAbstractSocket::BoundState &&
           m_control.hasPendingDatagrams()) {
        QHostAddress sender;
        quint16 port = 0;
        const auto pending = m_control.pendingDatagramSize();
        QByteArray bytes(pending < 0 ? 2048 : int(qMin<qint64>(pending, 2048)), 0);
        const auto read = m_control.readDatagram(bytes.data(), bytes.size(), &sender, &port);
        if (read < 0) {
            ++m_link.ctrlReadError;
            m_link.lastCtrlError = QStringLiteral("readDatagram qt_error=%1 error_text=\"%2\"")
                                       .arg(int(m_control.error()))
                                       .arg(m_control.errorString());
            break;
        }
        ++processed;
        if (pending < 0)
            bytes.resize(int(read));
        if (sender != m_endpoints.boardAddress || port != m_endpoints.boardControlPort) {
            ++m_link.ctrlFiltered;
            continue;
        }
        ++m_link.ctrlPackets;
        FptzProtocol::Packet p;
        if (read != bytes.size() || !FptzProtocol::parse(bytes, &p)) {
            ++m_link.ctrlFiltered;
            continue;
        }
        if (m_connecting) {
            if (p.txn != m_helloTxn) {
                ++m_link.ctrlFiltered;
                continue;
            }
            if (p.type == 11 && p.payload.size() == 8) {
                const auto code =
                    qFromBigEndian<quint16>(reinterpret_cast<const uchar *>(p.payload.constData()));
                fail(QStringLiteral("板卡拒绝握手，错误码 %1；请先停止其他视频测试").arg(code));
                return;
            }
            if (p.type != 2) {
                ++m_link.ctrlFiltered;
                continue;
            }
            if (!p.session || p.epoch || p.payload.size() != 20) {
                fail(QStringLiteral("板卡 HELLO_ACK 能力信息无效"));
                return;
            }
            const auto *b = reinterpret_cast<const uchar *>(p.payload.constData());
            const auto slotCount = qFromBigEndian<quint16>(b + 4);
            if (qFromBigEndian<quint16>(b) != 1400 || qFromBigEndian<quint16>(b + 2) != 64 ||
                slotCount < 1 || slotCount > 4 || b[6] != 1 || b[7] != 0 ||
                qFromBigEndian<quint32>(b + 8) != 64 + 2 * ((8 * 1024 * 1024 + 127) & ~63) ||
                qFromBigEndian<quint16>(b + 12) != 1 || qFromBigEndian<quint16>(b + 14) != 2 ||
                qFromBigEndian<quint16>(b + 16) != 1328 || qFromBigEndian<quint16>(b + 18) != 1) {
                fail(QStringLiteral("板卡能力不匹配：需要 v3、PS 生成 LUT、前后双路完整 1328×1328 "
                                    "JPEG及360°全景输出；请下载新版PS"));
                return;
            }
            m_session = p.session;
            m_connecting = false;
            m_connected = true;
            m_lastRx.start();
            ++m_link.ctrlAccepted;
            m_link.ctrlAcceptedAt = MediaWire::nowMs();
            m_link.lastReport = MediaWire::nowMs();
            m_slotCount = slotCount;
            m_mediaTimer.start();
            emit connectedChanged(true);
            emit statusChanged(
                QStringLiteral("已连接 · v3 · session=%1 · PS slots=%2 · LUT mode=1 · 视频未启动")
                    .arg(m_session)
                    .arg(slotCount));
        } else if (m_connected && p.session == m_session) {
            ++m_link.ctrlAccepted;
            m_link.ctrlAcceptedAt = MediaWire::nowMs();
            if ((p.type == 10 && (p.payload.size() == 24 || p.payload.size() == 32 ||
                                  p.payload.size() == 80 || p.payload.size() == 96)) ||
                (p.type == 9 && p.payload.size() == 12))
                m_lastRx.restart();
            else if (p.type == 11 && p.payload.size() == 8)
                emit statusChanged(QStringLiteral("板卡 ERROR，错误码 %1")
                                       .arg(qFromBigEndian<quint16>(reinterpret_cast<const uchar *>(
                                           p.payload.constData()))));
            handleMediaControl(p);
        } else
            ++m_link.ctrlFiltered;
    }
    if (processed == 32 && (m_connected || m_connecting) && generation == m_controlReadGeneration &&
        !m_controlDrainQueued && m_control.state() == QAbstractSocket::BoundState &&
        m_control.hasPendingDatagrams()) {
        m_controlDrainQueued = true;
        QTimer::singleShot(0, this, [this, generation] {
            if (generation != m_controlReadGeneration)
                return;
            const bool adjudicate = m_controlTimeoutPending;
            m_controlDrainQueued = false;
            receiveControl();
            if (generation == m_controlReadGeneration && adjudicate) {
                m_controlTimeoutPending = false;
                checkControlDeadline(false);
            }
        });
    }
}

void BoardConnection::checkControlDeadline(bool allowContinuation) {
    if (allowContinuation && m_controlTimeoutPending)
        return;
    const bool expired = m_connecting
                             ? m_elapsed.elapsed() >= m_endpoints.helloTimeoutMs
                             : m_connected && m_lastRx.elapsed() >= m_endpoints.idleTimeoutMs;
    // Let the already-queued bounded batch finish before adjudication, once.
    // Invalid traffic cannot repeatedly grant more batches or renew the deadline.
    if (expired && allowContinuation && m_controlDrainQueued) {
        m_controlTimeoutPending = true;
        return;
    }
    if (m_connecting) {
        if (m_elapsed.elapsed() >= m_endpoints.helloTimeoutMs)
            fail(QStringLiteral("握手超时：未收到有效 HELLO_ACK；检查板卡、网线及防火墙\n") +
                 controlDiagnostic());
        else if (m_elapsed.elapsed() - m_lastHelloMs >= m_endpoints.retryIntervalMs)
            sendHello();
    } else if (m_connected && m_lastRx.elapsed() >= m_endpoints.idleTimeoutMs)
        fail(QStringLiteral("连接超时：板卡心跳中断，已释放 UDP 端口\n") + controlDiagnostic());
}

void BoardConnection::tick() {
    if (m_controlTimeoutPending)
        return;
    // Read already-pending control messages before judging their deadline.
    if ((m_connected || m_connecting) && m_control.state() == QAbstractSocket::BoundState &&
        m_control.hasPendingDatagrams()) {
        ++m_link.ctrlPollDrains;
        receiveControl();
    }
    checkControlDeadline(true);
    if (m_connected) {
        // Nonblocking receive watchdog in the network thread, not the UI thread.
        // A readable socket must still be drained if readyRead notifications stop.
        if (m_output.state() == QAbstractSocket::BoundState && m_output.hasPendingDatagrams()) {
            ++m_link.rxPollDrains;
            receiveOutput();
        }
        reportLinkDiagnostics();
    }
}

void BoardConnection::disconnectFromBoard() {
    closeSockets();
    emit statusChanged(QStringLiteral("未连接 · 视频未启动"));
}
