#include "fpga/network/boardconnection.h"
#include "fpga/network/boardvideoservice.h"
#include "fpga/network/mediawire.h"
#include "fpga/network/panoramaview.h"
#include "ui/view_panel.h"
#include <QBuffer>
#include <QImageWriter>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPainter>
#include <QTemporaryDir>
#include <QtEndian>
#include <QtTest>
extern "C" {
#include "fixtures/ps_protocol/fptz_protocol.h"
}
class BoardConnectionTests : public QObject {
    Q_OBJECT
  private slots:
    void uploadWaitWindowsExcludeTests() {
        UploadWaitMeter meter;
        UploadWaitMeter::Window window;
        QVERIFY(!meter.sample(0, UploadWaitMeter::Source, window));
        QVERIFY(!meter.sample(100, UploadWaitMeter::Credit, window));
        QVERIFY(!meter.sample(500, UploadWaitMeter::Sending, window));
        QVERIFY(!meter.sample(800, UploadWaitMeter::Ready, window));
        QVERIFY(meter.sample(1000, UploadWaitMeter::Suspended, window));
        QCOMPARE(window.activeMs(), qint64(1000));
        QCOMPARE(window.ms[UploadWaitMeter::Credit], qint64(400));
        QCOMPARE(window.ms[UploadWaitMeter::Sending], qint64(300));
        QCOMPARE(window.ms[UploadWaitMeter::Source], qint64(100));
        QCOMPARE(window.ms[UploadWaitMeter::Ready], qint64(200));
        QCOMPARE(window.longestCreditMs, qint64(400));
        QVERIFY(meter.sample(2000, UploadWaitMeter::Credit, window));
        QCOMPARE(window.activeMs(), qint64(0));
        QVERIFY(meter.sample(3000, UploadWaitMeter::Credit, window));
        QCOMPARE(window.longestCreditMs, qint64(1000));
        QVERIFY(meter.sample(4000, UploadWaitMeter::Source, window));
        QCOMPARE(window.longestCreditMs, qint64(2000));
        meter.reset();
        QVERIFY(!meter.sample(9000, UploadWaitMeter::Source, window));
    }
    void flowDiagnosticsWindowAndBoundaries() {
        FlowDiagnostics meter;
        QByteArray body(256, 0);
        auto p32 = [&](int o, quint32 v) {
            qToBigEndian<quint32>(v, reinterpret_cast<uchar *>(body.data()) + o);
        };
        auto p64 = [&](int o, quint64 v) {
            qToBigEndian<quint64>(v, reinterpret_cast<uchar *>(body.data()) + o);
        };
        p32(0, 0x464c4f57);
        p32(4, 1);
        p32(8, 1);
        p32(120, 1);
        p64(16, 1000000);
        QVERIFY(meter.sample(body, 0, 0, 0).contains(QStringLiteral("基线")));
        p64(16, 2000000);
        p32(24, 2);
        p64(32, 6000);
        p64(40, 2000);
        p32(48, 2);
        p64(56, 8000);
        p64(64, 20000);
        p32(52, 1);
        p64(72, 33333);
        p32(80, 3);
        p32(28, 4);
        p32(128, 2);
        p64(136, 40000);
        p32(132, 1);
        p64(144, 5000);
        p64(152, 35000);
        p32(160, 1);
        const auto text = meter.sample(body, 5, 1, 2);
        QVERIFY(text.contains(QStringLiteral("CRC 3.00")));
        QVERIFY(text.contains(QStringLiteral("准备完成→首包 4.00")));
        QVERIFY(text.contains(QStringLiteral("首包→末包交付 10.00")));
        QVERIFY(text.contains(QStringLiteral("忙 20.00")));
        QVERIFY(text.contains(QStringLiteral("空闲 5.00")));
        QVERIFY(text.contains(QStringLiteral("Host待上传5、PS待处理3")));
        QVERIFY(meter.sample(body, 5, 1, 2).isEmpty()); // Duplicate.
        p64(16, 1500000);
        QVERIFY(meter.sample(body, 5, 1, 2).isEmpty());
        p64(16, 3000000);
        p32(8, 0);
        p32(120, 2);
        QVERIFY(meter.sample(body, 5, 1, 2).contains(QStringLiteral("暂停")));
        p64(16, 4000000);
        p32(8, 1);
        p32(120, 3);
        QVERIFY(meter.sample(body, 5, 1, 2).contains(QStringLiteral("基线")));
        p64(16, 5000000);
        p32(24, 0); // Counter reset is a new baseline.
        QVERIFY(meter.sample(body, 5, 1, 2).contains(QStringLiteral("基线")));
        QVERIFY(meter.sample(body.left(255), 5, 1, 2).isEmpty());
        p32(4, 2);
        QVERIFY(meter.sample(body, 5, 1, 2).isEmpty());
        meter.reset();
        p32(4, 1);
        QVERIFY(meter.sample(body, 0, 0, 0).contains(QStringLiteral("基线")));
    }
    void inputCrcTelemetryExtension() {
        FlowDiagnostics meter;
        QByteArray body(280, 0);
        auto p32 = [&](int o, quint32 v) {
            qToBigEndian<quint32>(v, reinterpret_cast<uchar *>(body.data()) + o);
        };
        auto p64 = [&](int o, quint64 v) {
            qToBigEndian<quint64>(v, reinterpret_cast<uchar *>(body.data()) + o);
        };
        p32(0, 0x464c4f57);
        p32(4, 2);
        p32(8, 1);
        p32(120, 1);
        p64(16, 1000);
        QVERIFY(meter.sample(body, 0, 0, 0).contains(QStringLiteral("基线")));
        p64(16, 1001000);
        p32(256, 2);
        p64(264, 6000);
        p64(272, 1000);
        const auto text = meter.sample(body, 0, 0, 0);
        QVERIFY(text.contains(QStringLiteral("整包CRC 3.00")));
        QVERIFY(text.contains(QStringLiteral("双JPEG格式检查 0.50")));
        p64(16, 2001000);
        p64(264, 0);
        QVERIFY(meter.sample(body, 0, 0, 0).contains(QStringLiteral("基线")));
        p32(4, 1);
        QVERIFY(meter.sample(body, 0, 0, 0).isEmpty());
        p32(4, 2);
        p64(16, 3001000);
        p32(256, 0);
        QVERIFY(meter.sample(body, 0, 0, 0).contains(QStringLiteral("基线")));
        p64(16, 4001000);
        QVERIFY(meter.sample(body, 0, 0, 0).contains(QStringLiteral("整包CRC —")));
        BoardConnection board;
        board.m_epoch = 2;
        QSignalSpy flow(&board, &BoardConnection::psFlowWindow);
        board.handleMediaControl({10, 99, 0, 2, body});
        QCOMPARE(flow.count(), 1);
    }
    void flowControlDoesNotDoubleCountLegacyFrames() {
        BoardConnection board;
        board.m_epoch = 2;
        QSignalSpy flow(&board, &BoardConnection::psFlowWindow);
        QSignalSpy receive(&board, &BoardConnection::psReceiveCountersChanged);
        QByteArray body(256, 0);
        auto *b = reinterpret_cast<uchar *>(body.data());
        qToBigEndian<quint32>(0x464c4f57, b);
        qToBigEndian<quint32>(1, b + 4);
        qToBigEndian<quint32>(1, b + 8);
        qToBigEndian<quint64>(100, b + 16);
        board.handleMediaControl({10, 99, 0, 3, body});
        QCOMPARE(flow.count(), 0);
        board.handleMediaControl({10, 99, 0, 2, body});
        QCOMPARE(flow.count(), 1);
        QCOMPARE(receive.count(), 0);
        board.resetMedia();
        board.m_epoch = 2;
        board.handleMediaControl({10, 99, 0, 2, body});
        QCOMPARE(flow.count(), 2);
    }
    void psWaitingTelemetryCompatibility() {
        BoardConnection board;
        board.m_epoch = 2;
        QSignalSpy spy(&board, &BoardConnection::psWaitWindow);
        QSignalSpy frameCounts(&board, &BoardConnection::psReceiveCountersChanged);
        QByteArray body(152, 0);
        auto *b = reinterpret_cast<uchar *>(body.data());
        qToBigEndian<quint32>(0x57414954, b + 96);
        auto send = [&](quint32 count, quint64 ready, quint64 pipeline, quint64 outQueue) {
            qToBigEndian<quint32>(count, b + 100);
            qToBigEndian<quint64>(ready, b + 112);
            qToBigEndian<quint64>(pipeline, b + 120);
            qToBigEndian<quint64>(outQueue, b + 128);
            board.handleMediaControl({10, 99, 0, 2, body});
        };
        send(10, 10000, 100000, 2000);
        QCOMPARE(spy.last()[1].toUInt(), quint32(0));
        send(12, 20000, 180000, 7000);
        QCOMPARE(frameCounts.count(), 0);
        QVERIFY(spy.last()[0].toBool());
        QCOMPARE(spy.last()[1].toUInt(), quint32(2));
        QCOMPARE(spy.last()[2].toDouble(), 5.0);
        QCOMPARE(spy.last()[3].toDouble(), 40.0);
        QCOMPARE(spy.last()[4].toDouble(), 2.5);
        send(1, 100, 1000, 0);
        QCOMPARE(spy.last()[1].toUInt(), quint32(0));
        const auto extendedSignals = spy.count();
        board.handleMediaControl({10, 99, 0, 2, body.left(96)});
        QCOMPARE(spy.count(), extendedSignals);
        board.resetMedia();
        board.m_epoch = 2;
        board.handleMediaControl({10, 99, 0, 2, body.left(96)});
        QVERIFY(!spy.last()[0].toBool());
    }
    void stageWaitExtensionDoesNotCountFramesTwice() {
        BoardConnection board;
        board.m_epoch = 2;
        QSignalSpy legacy(&board, &BoardConnection::stageTimesChanged);
        QSignalSpy waits(&board, &BoardConnection::psStageWaitWindow);
        QByteArray body(48, 0);
        auto *b = reinterpret_cast<uchar *>(body.data());
        qToBigEndian<quint32>(10000, b + 36);
        qToBigEndian<quint32>(20000, b + 40);
        qToBigEndian<quint32>(30000, b + 44);
        board.handleMediaControl({8, 99, 0, 2, body.left(36)});
        QCOMPARE(legacy.count(), 1);
        board.m_stageWaitWindowAt = MediaWire::nowMs() - 1001;
        board.handleMediaControl({8, 99, 0, 2, body});
        QCOMPARE(legacy.count(), 1);
        QCOMPARE(board.m_link.resultSeen, quint64(1));
        QCOMPARE(waits.count(), 1);
        QCOMPARE(waits[0][1].toDouble(), 10.0);
        QCOMPARE(waits[0][2].toDouble(), 20.0);
        QCOMPARE(waits[0][3].toDouble(), 30.0);
        board.handleMediaControl({8, 99, 0, 3, body});
        QCOMPARE(board.m_stageWaitFrames, quint32(0));
    }
    void hardwareFaultStopsUploadAndSavesExactInput() {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        BoardConnection board;
        board.m_connected = true;
        board.m_epoch = 2;
        board.m_endpoints.diagnosticsDirectory = temporary.path();
        QImage input(2656, 1328, QImage::Format_RGB888);
        input.fill(Qt::red);
        const auto pair = MediaWire::encode(input);
        QVERIFY(!pair.isEmpty());
        board.m_recentInput.append({pair, 544, 2, 4321, FptzProtocol::crc32(pair)});
        board.m_upload.jpeg = pair;
        board.m_latestJpeg = pair;
        QByteArray error(8, 0);
        auto *b = reinterpret_cast<uchar *>(error.data());
        qToBigEndian<quint16>(FptzProtocol::HardwareFault, b);
        b[2] = 1;
        b[3] = 3; // Rear decoder timeout.
        qToBigEndian<quint32>(544, b + 4);
        board.handleMediaControl({11, 99, 0, 3, error});
        QVERIFY(board.isConnected()); // Reject old/different epoch notification.
        QCOMPARE(QDir(temporary.path()).entryList(QDir::Dirs | QDir::NoDotAndDotDot).size(), 0);
        QSignalSpy messages(&board, &BoardConnection::statusChanged);
        board.handleMediaControl({11, 99, 0, 2, error});
        QVERIFY(!board.isConnected());
        QVERIFY(board.m_upload.jpeg.isEmpty());
        QVERIFY(board.m_latestJpeg.isEmpty());
        QVERIFY(board.m_recentInput.isEmpty());
        QVERIFY(messages.last()[0].toString().contains(QStringLiteral("后镜头")));
        const auto folders = QDir(temporary.path()).entryList(QDir::Dirs | QDir::NoDotAndDotDot);
        QCOMPARE(folders.size(), 1);
        const QDir folder(QDir(temporary.path()).filePath(folders[0]));
        QFile saved(folder.filePath("input_pair.bin"));
        QVERIFY(saved.open(QIODevice::ReadOnly));
        QCOMPARE(saved.readAll(), pair);
        QFile meta(folder.filePath("metadata.json"));
        QVERIFY(meta.open(QIODevice::ReadOnly));
        const auto object = QJsonDocument::fromJson(meta.readAll()).object();
        QCOMPARE(object["frame"].toInt(), 544);
        QVERIFY(object["input_available"].toBool());
        QCOMPARE(object["camera_timestamp_ms"].toInt(), 4321);
        QCOMPARE(QImage(folder.filePath("rear.jpg")).size(), QSize(1328, 1328));
    }
    void crcMatchesPs() {
        QCOMPARE(FptzProtocol::crc32(QByteArray()), quint32(0));
        QCOMPARE(FptzProtocol::crc32("123456789"), quint32(0xcbf43926));
        QByteArray bytes(65537, 0);
        for (int i = 0; i < bytes.size(); ++i)
            bytes[i] = char((i * 29 + i / 251) & 255);
        QCOMPARE(FptzProtocol::crc32(bytes), fptz_crc32(bytes.constData(), bytes.size()));
    }
    void onlyBoardPanoramaProducesBottomViews() {
        BoardVideoService service;
        QImage raw(2656, 1328, QImage::Format_RGB888);
        raw.fill(Qt::red);
        service.submit(raw, 100);
        service.requestPose(45, 20, 90);
        QTest::qWait(30);
        QVERIFY(service.takeSphere().isNull());
        QVERIFY(service.takeDisplay().image.isNull());
    }
    void panoramaViewingAngles() {
        QImage panorama(2656, 1328, QImage::Format_RGB888);
        panorama.fill(Qt::red);
        QPainter paint(&panorama);
        paint.fillRect(1660, 0, 996, 1328, Qt::blue);
        paint.fillRect(0, 0, 830, 1328, Qt::yellow);
        paint.end();
        PanoramaView renderer;
        QImage sphere, view;
        renderer.render(panorama, {0, 0, 90}, sphere, view);
        QCOMPARE(sphere.size(), QSize(360, 360));
        QCOMPARE(view.size(), QSize(640, 360));
        QVERIFY(view.pixelColor(320, 180).red() > 240);
        QCOMPARE(sphere.pixelColor(0, 0), QColor(Qt::black));
        renderer.render(panorama, {90, 0, 90}, sphere, view);
        QVERIFY(view.pixelColor(320, 180).blue() > 240);
        renderer.render(panorama, {-90, 0, 90}, sphere, view);
        QVERIFY(view.pixelColor(320, 180).red() > 240 && view.pixelColor(320, 180).green() > 240);
    }
    void panoramaServiceDoesNotRebuildPsLutWhenDragging() {
        QUdpSocket control, input, output;
        QVERIFY(control.bind(QHostAddress(QHostAddress::LocalHost), 0));
        QVERIFY(input.bind(QHostAddress(QHostAddress::LocalHost), 0));
        QVERIFY(output.bind(QHostAddress(QHostAddress::LocalHost), 0));
        QUdpSocket reserve[3];
        quint16 ports[3];
        for (int i = 0; i < 3; ++i) {
            QVERIFY(reserve[i].bind(QHostAddress(QHostAddress::LocalHost), 0));
            ports[i] = reserve[i].localPort();
        }
        for (auto &socket : reserve)
            socket.close();
        BoardEndpoints ep;
        ep.localAddress = ep.boardAddress = QHostAddress::LocalHost;
        ep.controlPort = ports[0];
        ep.inputPort = ports[1];
        ep.outputPort = ports[2];
        ep.boardControlPort = control.localPort();
        ep.boardInputPort = input.localPort();
        ep.boardOutputPort = output.localPort();
        int poseCommands = 0;
        bool fullPanoramaRequested = false, stream = false, uploaded = false;
        connect(&control, &QUdpSocket::readyRead, this, [&] {
            while (control.hasPendingDatagrams()) {
                QByteArray wire(control.pendingDatagramSize(), 0);
                control.readDatagram(wire.data(), wire.size());
                FptzProtocol::Packet packet;
                if (!FptzProtocol::parse(wire, &packet))
                    continue;
                QByteArray reply;
                if (packet.type == 1) {
                    QByteArray body(20, 0);
                    auto *b = reinterpret_cast<uchar *>(body.data());
                    qToBigEndian<quint16>(1400, b);
                    qToBigEndian<quint16>(64, b + 2);
                    qToBigEndian<quint16>(4, b + 4);
                    b[6] = 1;
                    qToBigEndian<quint32>(FPTZ_MAX_INPUT_BYTES, b + 8);
                    qToBigEndian<quint16>(1, b + 12);
                    qToBigEndian<quint16>(2, b + 14);
                    qToBigEndian<quint16>(1328, b + 16);
                    qToBigEndian<quint16>(1, b + 18);
                    reply = FptzProtocol::pack({2, 77, packet.txn, 0, body});
                } else if (packet.type == 3) {
                    ++poseCommands;
                    const auto *b = reinterpret_cast<const uchar *>(packet.payload.constData());
                    fullPanoramaRequested = qFromBigEndian<quint32>(b) == 0 &&
                                            qFromBigEndian<quint32>(b + 4) == 0 &&
                                            qFromBigEndian<quint32>(b + 8) == 360000;
                    QByteArray body(8, 0);
                    body[0] = 3;
                    qToBigEndian<quint32>(2, reinterpret_cast<uchar *>(body.data()) + 4);
                    reply = FptzProtocol::pack({4, 77, packet.txn, 2, body});
                } else if (packet.type == 5) {
                    QByteArray body(4, 0);
                    body[0] = packet.payload[0];
                    body[1] = body[0] == 1 ? 1 : 0;
                    reply = FptzProtocol::pack({6, 77, packet.txn, 2, body});
                    stream = body[1];
                    if (stream) {
                        QByteArray credit(12, 0);
                        auto *b = reinterpret_cast<uchar *>(credit.data());
                        qToBigEndian<quint16>(4, b);
                        qToBigEndian<quint16>(4, b + 2);
                        qToBigEndian<quint32>(1, b + 8);
                        control.writeDatagram(FptzProtocol::pack({9, 77, 0, 2, credit}),
                                              QHostAddress::LocalHost, ep.controlPort);
                    }
                }
                if (!reply.isEmpty())
                    control.writeDatagram(reply, QHostAddress::LocalHost, ep.controlPort);
            }
        });
        QImage panorama(2656, 1328, QImage::Format_RGB888);
        panorama.fill(Qt::red);
        QPainter paint(&panorama);
        paint.fillRect(1660, 0, 996, 1328, Qt::blue);
        paint.end();
        QByteArray jpeg;
        QBuffer buffer(&jpeg);
        buffer.open(QIODevice::WriteOnly);
        QVERIFY(panorama.save(&buffer, "JPEG"));
        connect(&input, &QUdpSocket::readyRead, this, [&] {
            while (input.hasPendingDatagrams()) {
                QByteArray wire(input.pendingDatagramSize(), 0);
                input.readDatagram(wire.data(), wire.size());
                FptzHeader h{};
                if (fptz_parse_packet(reinterpret_cast<const uint8_t *>(wire.constData()),
                                      wire.size(), &h))
                    continue;
                if (h.chunk_index + 1 != h.chunk_count)
                    continue;
                uploaded = true;
                // New PS telemetry crosses the real UDP/control parsing path.
                QByteArray telemetry(160, 0);
                auto put = [&](int at, quint32 value) {
                    qToBigEndian(value, reinterpret_cast<uchar *>(telemetry.data()) + at);
                };
                put(0, 0x4c415431);
                put(4, 1);
                put(8, 1);
                put(12, 166664993);
                put(16, 1);
                put(56, 166665);
                const quint32 bounds[] = {0, 100, 200, 4000, 4100, 4500};
                for (int k = 0; k < 4; ++k)
                    put(120 + k * 4, bounds[k]);
                for (int k = 0; k < 3; ++k)
                    put(64 + k * 4, bounds[k + 1] - bounds[k]);
                control.writeDatagram(FptzProtocol::pack({8, 77, 0, 2, telemetry, h.frame_id}),
                                      QHostAddress::LocalHost, ep.controlPort);
                put(8, 2);
                put(16, 2);
                for (int k = 0; k < 6; ++k)
                    put(120 + k * 4, bounds[k]);
                put(76, 100);
                put(80, 400);
                put(84, 4500);
                control.writeDatagram(FptzProtocol::pack({8, 77, 0, 2, telemetry, h.frame_id}),
                                      QHostAddress::LocalHost, ep.controlPort);
                const int count = (jpeg.size() + 1399) / 1400;
                for (int i = 0; i < count; ++i)
                    output.writeDatagram(
                        MediaWire::pack({0x21, 77, h.frame_id, 2, quint32(jpeg.size()),
                                         FptzProtocol::crc32(jpeg), quint16(i), quint16(count),
                                         jpeg.mid(i * 1400, 1400)}),
                        QHostAddress::LocalHost, ep.outputPort);
            }
        });
        BoardVideoService service;
        QSignalSpy encoding(&service, &BoardVideoService::pairEncodingRatesChanged);
        service.connectToBoard(ep);
        QTRY_VERIFY(stream);
        QVERIFY(fullPanoramaRequested);
        QCOMPARE(poseCommands, 1);
        QVERIFY(service.takeSphere().isNull());
        QVERIFY(service.takeDisplay().image.isNull());
        QImage raw(2656, 1328, QImage::Format_RGB888);
        raw.fill(Qt::green);
        service.submit(raw, 200);
        BoardVideoService::Display frame;
        auto pollFrame = [&] {
            if (frame.image.isNull())
                frame = service.takeDisplay();
            return !frame.image.isNull();
        };
        QTRY_VERIFY(pollFrame());
        QVERIFY(uploaded);
        QVERIFY(frame.image.pixelColor(320, 180).red() > 230);
        QVERIFY(!service.takeSphere().isNull());
        QVERIFY(frame.sampleId != 0);
        auto &tracker = service.latencyTracker();
        df::Viewport viewport;
        connect(&viewport, &df::Viewport::painted, this,
                [&](quint64 id, qint64 rs, qint64 re, qint64 take, qint64 ps, qint64 pe) {
                    tracker.mark(id, "render_start", rs);
                    tracker.mark(id, "render_end", re);
                    tracker.mark(id, "ui_take", take);
                    tracker.mark(id, "paint_start", ps);
                    tracker.mark(id, "paint_end", pe);
                });
        viewport.setTrackedImage(frame.image, frame.sampleId, frame.renderStartedUs,
                                 frame.renderedAtUs, LatencyTracker::nowUs());
        viewport.show();
        QTRY_COMPARE(tracker.snapshot().metrics.value("total").n, 1);
        auto measured = tracker.snapshot();
        QCOMPARE(measured.metrics["encode"].n, 1);
        QCOMPARE(measured.metrics["decode"].n, 1);
        QCOMPARE(measured.metrics["render"].n, 1);
        QTRY_COMPARE(tracker.snapshot().metrics.value("ps_pipe").n, 1);
        QCOMPARE(tracker.snapshot().metrics["ps_pipe"].mean, 3.8);
        const auto originalLatency = measured.metrics["total"].mean;
        frame = {};
        service.requestPose(90, 0, 90);
        QTRY_VERIFY(pollFrame());
        QVERIFY(frame.image.pixelColor(320, 180).blue() > 230);
        viewport.setTrackedImage(frame.image, frame.sampleId, frame.renderStartedUs,
                                 frame.renderedAtUs, LatencyTracker::nowUs());
        QTest::qWait(20);
        QCOMPARE(tracker.snapshot().metrics["total"].n, 1);
        QCOMPARE(tracker.snapshot().metrics["total"].mean, originalLatency);
        QCOMPARE(poseCommands, 1);
        QTRY_VERIFY(!encoding.isEmpty());
        QVERIFY(encoding.first()[0].toDouble() > 0);
        QVERIFY(encoding.first()[1].toDouble() > 0);
        QCOMPARE(encoding.first()[2].toULongLong(), quint64(0));
        // Both workers share a condition variable: alternating pose updates and
        // raw frames must wake the right worker and continue producing uploads.
        for (int i = 0; i < 3; ++i) {
            uploaded = false;
            service.requestPose(i * 30, 0, 90);
            service.submit(raw, 201 + i);
            QTRY_VERIFY(uploaded);
        }
        service.disconnectFromBoard();
        QTRY_VERIFY(!service.isConnected());
        QVERIFY(service.takeSphere().isNull());
        QVERIFY(service.takeDisplay().image.isNull());
    }
    void exactPsCompatibility() {
        QImage image(2656, 1328, QImage::Format_RGB888);
        image.fill(Qt::red);
        QPainter painter(&image);
        painter.fillRect(1328, 0, 1328, 1328, Qt::blue);
        painter.end();
        QString error;
        auto pair = MediaWire::encode(image, &error);
        QVERIFY2(!pair.isEmpty(), qPrintable(error));
        QVERIFY(MediaWire::validInput(pair));
        FptzJpegPair parsed{};
        QCOMPARE(fptz_jpeg_pair_parse(reinterpret_cast<const uint8_t *>(pair.constData()),
                                      pair.size(), &parsed),
                 0);
        auto front = QImage::fromData(pair.mid(parsed.offset[0], parsed.length[0]), "jpeg");
        auto rear = QImage::fromData(pair.mid(parsed.offset[1], parsed.length[1]), "jpeg");
        QCOMPARE(front.size(), QSize(1328, 1328));
        QCOMPARE(rear.size(), front.size());
        // The read-only stride-preserving view must produce exactly the same
        // JPEG as the former cropped-copy path, for each lens separately.
        for (int i = 0; i < 2; ++i) {
            QByteArray expected;
            QBuffer buffer(&expected);
            QVERIFY(buffer.open(QIODevice::WriteOnly));
            QImageWriter writer(&buffer, "jpeg");
            writer.setQuality(85);
            writer.setOptimizedWrite(false);
            writer.setProgressiveScanWrite(false);
            QVERIFY(writer.write(image.copy(i * 1328, 0, 1328, 1328)));
            QCOMPARE(pair.mid(parsed.offset[i], parsed.length[i]), expected);
        }
        QVERIFY(front.pixelColor(664, 664).red() > 240);
        QVERIFY(rear.pixelColor(664, 664).blue() > 240);
        auto damaged = pair;
        damaged[8] = 1;
        QVERIFY(!MediaWire::validInput(damaged));
        QCOMPARE(fptz_jpeg_pair_parse(reinterpret_cast<const uint8_t *>(damaged.constData()),
                                      damaged.size(), &parsed),
                 -1);
        QVERIFY(MediaWire::encode(QImage(2650, 1328, QImage::Format_RGB888), &error).isEmpty());
        QVERIFY(!error.isEmpty());
        const int count = (pair.size() + 1399) / 1400;
        const auto crc = FptzProtocol::crc32(pair);
        for (int i = 0; i < count; ++i) {
            auto wire = MediaWire::pack({0x23, 99, 1, 2, quint32(pair.size()), crc, quint16(i),
                                         quint16(count), pair.mid(i * 1400, 1400)});
            FptzHeader header{};
            QCOMPARE(fptz_parse_packet(reinterpret_cast<const uint8_t *>(wire.constData()),
                                       wire.size(), &header),
                     0);
            QCOMPARE(header.msg_type, uint8_t(FPTZ_INPUT_JPEG_PAIR));
            QCOMPARE(header.object_crc32, fptz_crc32(pair.constData(), pair.size()));
        }
    }
    void outputReorderingAndCrc() {
        QImage image(2656, 1328, QImage::Format_RGB888);
        image.fill(Qt::green);
        QByteArray jpeg;
        QBuffer buffer(&jpeg);
        buffer.open(QIODevice::WriteOnly);
        QVERIFY(image.save(&buffer, "JPEG"));
        const quint32 crc = FptzProtocol::crc32(jpeg);
        const int count = (jpeg.size() + 1399) / 1400;
        MediaWire::Collector collector;
        QByteArray result;
        for (int i = count - 1; i >= 0; --i) {
            auto wire = MediaWire::pack({0x21, 10, 12, 2, quint32(jpeg.size()), crc, quint16(i),
                                         quint16(count), jpeg.mid(i * 1400, 1400)});
            MediaWire::Chunk chunk;
            QVERIFY(MediaWire::parse(wire, &chunk));
            auto value = collector.add(chunk, 100);
            if (!value.isEmpty())
                result = value;
        }
        QCOMPARE(result, jpeg);
        auto bad = MediaWire::Chunk{0x21, 10, 13, 2, 4, 0, 0, 1, QByteArray::fromHex("ffd8ffd9")};
        QVERIFY(collector.add(bad, 100).isEmpty());
    }
    void dualHandshakeAndPose_data() {
        QTest::addColumn<int>("batchPackets");
        QTest::newRow("original-eight") << 8;
        QTest::newRow("default-sixteen") << 16;
        QTest::newRow("bounded-thirty-two") << 32;
    }
    void dualHandshakeAndPose() {
        QFETCH(int, batchPackets);
        QUdpSocket ps, input, output;
        QVERIFY(input.bind(QHostAddress(QHostAddress::LocalHost), 0));
        QVERIFY(output.bind(QHostAddress(QHostAddress::LocalHost), 0));
        QVERIFY(ps.bind(QHostAddress(QHostAddress::LocalHost), 0));
        BoardConnection board;
        BoardEndpoints endpoints;
        endpoints.uploadBatchPackets = batchPackets;
        endpoints.localAddress = QHostAddress::LocalHost;
        endpoints.boardAddress = QHostAddress::LocalHost;
        endpoints.controlPort = endpoints.inputPort = endpoints.outputPort = 0;
        endpoints.boardControlPort = ps.localPort();
        endpoints.boardInputPort = input.localPort();
        endpoints.boardOutputPort = output.localPort();
        QSignalSpy connected(&board, &BoardConnection::connectedChanged);
        QSignalSpy resolved(&board, &BoardConnection::poseResolved);
        QHostAddress host;
        quint16 port = 0;
        quint32 poseTxn = 0;
        bool validPose = false;
        connect(&ps, &QUdpSocket::readyRead, this, [&] {
            while (ps.hasPendingDatagrams()) {
                QByteArray wire(ps.pendingDatagramSize(), 0);
                ps.readDatagram(wire.data(), wire.size(), &host, &port);
                FptzProtocol::Packet packet;
                if (!FptzProtocol::parse(wire, &packet))
                    continue;
                QByteArray reply;
                if (packet.type == 1) {
                    QByteArray body(20, 0);
                    auto *b = reinterpret_cast<uchar *>(body.data());
                    qToBigEndian<quint16>(1400, b);
                    qToBigEndian<quint16>(64, b + 2);
                    qToBigEndian<quint16>(4, b + 4);
                    b[6] = 1;
                    qToBigEndian<quint32>(FPTZ_MAX_INPUT_BYTES, b + 8);
                    qToBigEndian<quint16>(1, b + 12);
                    qToBigEndian<quint16>(2, b + 14);
                    qToBigEndian<quint16>(1328, b + 16);
                    qToBigEndian<quint16>(1, b + 18);
                    reply = FptzProtocol::pack({2, 99, packet.txn, 0, body});
                } else if (packet.type == 3) {
                    poseTxn = packet.txn;
                    const auto *b = reinterpret_cast<const uchar *>(packet.payload.constData());
                    validPose = qFromBigEndian<qint32>(b) == 170000 &&
                                qFromBigEndian<qint32>(b + 4) == 12300 &&
                                qFromBigEndian<quint32>(b + 8) == 90000;
                    QByteArray body(8, 0);
                    body[0] = 3;
                    qToBigEndian<quint32>(2, reinterpret_cast<uchar *>(body.data()) + 4);
                    reply = FptzProtocol::pack({4, 99, poseTxn, 2, body});
                } else if (packet.type == 5) {
                    QByteArray body(4, 0);
                    body[0] = 1;
                    body[1] = 1;
                    reply = FptzProtocol::pack({6, 99, packet.txn, 2, body});
                }
                if (!reply.isEmpty())
                    ps.writeDatagram(reply, host, port);
            }
        });
        board.connectToBoard(endpoints);
        QTRY_VERIFY(board.isConnected());
        board.requestPose(170, 12.34, 90);
        QTRY_VERIFY(validPose);
        QTRY_COMPARE(resolved.count(), 1);
        QCOMPARE(board.epoch(), quint32(2));
        QTRY_VERIFY(board.m_streaming);
        QSignalSpy returned(&board, &BoardConnection::jpegReady);
        QImage raw(2656, 1328, QImage::Format_RGB888);
        for (int y = 0; y < raw.height(); ++y) {
            auto *row = raw.scanLine(y);
            for (int x = 0; x < raw.width(); ++x) {
                row[x * 3] = uchar(x / 8 + y / 8);
                row[x * 3 + 1] = uchar(x / 8 + y / 4);
                row[x * 3 + 2] = uchar(x / 4 + y / 8);
            }
        }
        auto pair = MediaWire::encode(raw);
        QVERIFY(!pair.isEmpty());
        QVERIFY((pair.size() + 1399) / 1400 > batchPackets);
        QByteArray storage(pair.size(), 0), bitmap(((pair.size() + 1399) / 1400 + 7) / 8, 0);
        FptzRx receiver{};
        bool receiving = false, complete = false, valid = true;
        QImage corrected(2656, 1328, QImage::Format_RGB888);
        corrected.fill(Qt::blue);
        QByteArray jpeg;
        QBuffer buffer(&jpeg);
        buffer.open(QIODevice::WriteOnly);
        QVERIFY(corrected.save(&buffer, "JPEG"));
        connect(&input, &QUdpSocket::readyRead, this, [&] {
            while (input.hasPendingDatagrams()) {
                QByteArray wire(input.pendingDatagramSize(), 0);
                input.readDatagram(wire.data(), wire.size());
                FptzHeader h{};
                if (fptz_parse_packet(reinterpret_cast<const uint8_t *>(wire.constData()),
                                      wire.size(), &h)) {
                    valid = false;
                    continue;
                }
                if (!receiving) {
                    receiving = true;
                    valid = valid && fptz_rx_begin(
                                         &receiver, reinterpret_cast<uint8_t *>(storage.data()),
                                         storage.size(), reinterpret_cast<uint8_t *>(bitmap.data()),
                                         bitmap.size(), 1400, &h) == 0;
                }
                const int added = fptz_rx_push(
                    &receiver, &h, reinterpret_cast<const uint8_t *>(wire.constData()) + 48);
                valid = valid && added >= 0 && h.capture_ts_ms == 20569922;
                if (added == 1) {
                    FptzJpegPair parsed{};
                    valid =
                        valid && fptz_rx_verify(&receiver) == 0 &&
                        fptz_jpeg_pair_parse(reinterpret_cast<const uint8_t *>(storage.constData()),
                                             storage.size(), &parsed) == 0;
                    complete = true;
                    const int count = (jpeg.size() + 1399) / 1400;
                    for (int i = count - 1; i >= 0; --i) {
                        auto chunk = MediaWire::pack({0x21, 99, h.frame_id, 2, quint32(jpeg.size()),
                                                      FptzProtocol::crc32(jpeg), quint16(i),
                                                      quint16(count), jpeg.mid(i * 1400, 1400)});
                        output.writeDatagram(chunk, QHostAddress::LocalHost,
                                             board.m_output.localPort());
                    }
                }
            }
        });
        QByteArray credit(12, 0);
        auto *b = reinterpret_cast<uchar *>(credit.data());
        qToBigEndian<quint16>(4, b);
        qToBigEndian<quint16>(4, b + 2);
        qToBigEndian<quint32>(1, b + 8);
        ps.writeDatagram(FptzProtocol::pack({9, 99, 0, 2, credit}), host, port);
        QTRY_COMPARE(board.m_free, 4);
        board.m_mediaTimer.stop();
        board.submitJpeg(pair, MediaWire::nowMs(), 20569922);
        board.pumpMedia();
        QCOMPARE(board.m_upload.index, batchPackets);
        board.m_mediaTimer.start();
        QTRY_VERIFY(complete);
        QVERIFY(valid);
        QCOMPARE(storage, pair);
        QTRY_COMPARE(returned.count(), 1);
        QCOMPARE(returned[0][0].toByteArray(), jpeg);
        board.disconnectFromBoard();
    }
};
QTEST_MAIN(BoardConnectionTests)
#include "board_pair_tests.moc"
