#include "core/types.h"
#include "fpga/network/boardvideoservice.h"
#include "quality/qualityanalysis.h"
#include "quality/qualityjob.h"
#include "ui/imagequalitydialog.h"
#include "ui/main_window.h"
#include "ui/settings_dialog.h"
#include <QBuffer>
#include <QFile>
#include <QGraphicsPixmapItem>
#include <QGraphicsScene>
#include <QGraphicsView>
#include <QJsonDocument>
#include <QLabel>
#include <QPushButton>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTimer>
#include <QUdpSocket>
#include <QtEndian>
#include <QtTest>
#include <cmath>
#include <opencv2/imgproc.hpp>

class FakeQualityBoard : public QObject {
  public:
    QUdpSocket control, input, output;
    BoardEndpoints ep;
    quint32 session = 100;
    int uploads = 0, poses = 0;
    bool paused = false;
    bool returnFrames = true, wrongOnly = false, badJpeg = false;
    QByteArray outputJpeg, exactInput;
    QByteArray pendingInput;
    QSet<int> receivedChunks;
    quint32 inputFrame = 0, inputSession = 0;
    FakeQualityBoard() {
        control.bind(QHostAddress(QHostAddress::LocalHost), 0);
        input.bind(QHostAddress(QHostAddress::LocalHost), 0);
        output.bind(QHostAddress(QHostAddress::LocalHost), 0);
        input.setSocketOption(QAbstractSocket::ReceiveBufferSizeSocketOption, 4 * 1024 * 1024);
        QUdpSocket reserve[3];
        quint16 ports[3];
        for (int i = 0; i < 3; ++i) {
            reserve[i].bind(QHostAddress(QHostAddress::LocalHost), 0);
            ports[i] = reserve[i].localPort();
        }
        ep.localAddress = ep.boardAddress = QHostAddress::LocalHost;
        ep.controlPort = ports[0];
        ep.inputPort = ports[1];
        ep.outputPort = ports[2];
        ep.boardControlPort = control.localPort();
        ep.boardInputPort = input.localPort();
        ep.boardOutputPort = output.localPort();
        QImage image(2656, 1328, QImage::Format_RGB888);
        image.fill(QColor(90, 120, 80));
        QBuffer buffer(&outputJpeg);
        buffer.open(QIODevice::WriteOnly);
        image.save(&buffer, "JPEG");
        connect(&control, &QUdpSocket::readyRead, this, [this] {
            while (control.hasPendingDatagrams()) {
                QByteArray wire(int(control.pendingDatagramSize()), 0);
                control.readDatagram(wire.data(), wire.size());
                FptzProtocol::Packet p;
                if (!FptzProtocol::parse(wire, &p))
                    continue;
                QByteArray body;
                quint8 type = 0;
                quint32 epoch = 2;
                if (p.type == 1) {
                    ++session;
                    pendingInput.clear();
                    receivedChunks.clear();
                    inputFrame = inputSession = 0;
                    type = 2;
                    epoch = 0;
                    body = QByteArray(20, 0);
                    auto b = reinterpret_cast<uchar *>(body.data());
                    qToBigEndian<quint16>(1400, b);
                    qToBigEndian<quint16>(64, b + 2);
                    qToBigEndian<quint16>(4, b + 4);
                    b[6] = 1;
                    qToBigEndian<quint32>(64 + 2 * ((8 * 1024 * 1024 + 127) & ~63), b + 8);
                    qToBigEndian<quint16>(1, b + 12);
                    qToBigEndian<quint16>(2, b + 14);
                    qToBigEndian<quint16>(1328, b + 16);
                    qToBigEndian<quint16>(1, b + 18);
                } else if (p.type == 3) {
                    ++poses;
                    type = 4;
                    body = QByteArray(8, 0);
                    body[0] = 3;
                    qToBigEndian<quint32>(2, reinterpret_cast<uchar *>(body.data()) + 4);
                } else if (p.type == 5) {
                    type = 6;
                    body = QByteArray(4, 0);
                    body[0] = p.payload[0];
                    body[1] = body[0] == 1 || body[0] == 5 ? 1 : 0;
                    body[3] = body[0] == 4 ? 1 : 0;
                    paused = body[3];
                    if (body[0] == 1) {
                        QByteArray credit(12, 0);
                        auto b = reinterpret_cast<uchar *>(credit.data());
                        qToBigEndian<quint16>(4, b);
                        qToBigEndian<quint16>(4, b + 2);
                        qToBigEndian<quint32>(1, b + 8);
                        control.writeDatagram(FptzProtocol::pack({9, session, 0, 2, credit}),
                                              QHostAddress::LocalHost, ep.controlPort);
                    }
                }
                if (type)
                    control.writeDatagram(FptzProtocol::pack({type, session, p.txn, epoch, body}),
                                          QHostAddress::LocalHost, ep.controlPort);
            }
        });
        connect(&input, &QUdpSocket::readyRead, this, [this] {
            while (input.hasPendingDatagrams()) {
                QByteArray wire(int(input.pendingDatagramSize()), 0);
                input.readDatagram(wire.data(), wire.size());
                MediaWire::Chunk c;
                if (!MediaWire::parse(wire, &c))
                    continue;
                if (c.frame != inputFrame || c.session != inputSession) {
                    inputFrame = c.frame;
                    inputSession = c.session;
                    pendingInput = QByteArray(int(c.length), 0);
                    receivedChunks.clear();
                }
                pendingInput.replace(c.index * 1400, c.payload.size(), c.payload);
                receivedChunks.insert(c.index);
                if ((c.index + 1) % 32 == 0 || c.index + 1 == c.count) {
                    QByteArray ack(16, 0);
                    auto *bytes = reinterpret_cast<uchar *>(ack.data());
                    bytes[0] = 0x23;
                    bytes[1] = receivedChunks.size() == c.count ? 1 : 0;
                    int contiguous = 0;
                    while (receivedChunks.contains(contiguous))
                        ++contiguous;
                    qToBigEndian<quint16>(contiguous, bytes + 2);
                    qToBigEndian<quint32>(receivedChunks.size(), bytes + 12);
                    control.writeDatagram(
                        FptzProtocol::pack({7, c.session, 0, c.epoch, ack, c.frame}),
                        QHostAddress::LocalHost, ep.controlPort);
                }
                if (receivedChunks.size() != c.count)
                    continue;
                auto pair = pendingInput;
                if (FptzProtocol::crc32(pair) != c.crc || !MediaWire::validInput(pair))
                    continue;
                ++uploads;
                exactInput = pair;
                if (!returnFrames)
                    continue;
                QTimer::singleShot(10, this, [this, c] {
                    auto bytes = badJpeg ? QByteArray::fromHex("ffd800ffd9") : outputJpeg;
                    int count = (bytes.size() + 1399) / 1400;
                    // A complete but wrong-session image must never satisfy the job.
                    for (int pass = 0; pass < (wrongOnly ? 1 : 2); ++pass)
                        for (int i = count - 1; i >= 0; --i) {
                            auto wire = MediaWire::pack(
                                {0x21, pass ? c.session : c.session - 1, c.frame, c.epoch,
                                 quint32(bytes.size()), FptzProtocol::crc32(bytes), quint16(i),
                                 quint16(count), bytes.mid(i * 1400, 1400)});
                            output.writeDatagram(wire, QHostAddress::LocalHost, ep.outputPort);
                            if (i == 0)
                                output.writeDatagram(wire, QHostAddress::LocalHost, ep.outputPort);
                        }
                });
            }
        });
    }
};
class QualityTests : public QObject {
    Q_OBJECT
    static QImage solid(int value, int size = 32) {
        QImage im(size, size, QImage::Format_RGB888);
        im.fill(QColor(value, value, value));
        return im;
    }
  private slots:
    void initTestCase() {
        QFile theme((df::projectRoot() + QStringLiteral("/resources/theme.qss")));
        if (theme.open(QIODevice::ReadOnly))
            qApp->setStyleSheet(QString::fromUtf8(theme.readAll()));
        qApp->setFont(QFont("Noto Sans CJK SC", 10));
    }
    void identicalAndKnownError() {
        auto a = solid(0);
        cv::Mat mask(32, 32, CV_8U, cv::Scalar(255));
        auto s = Quality::compare(a, a, mask);
        QVERIFY(s.infinite);
        QCOMPARE(s.ssim, 1.);
        QCOMPARE(s.pixels, 1024);
        QCOMPARE(s.windows, 22 * 22);
        auto b = solid(10);
        s = Quality::compare(a, b, mask);
        QVERIFY(!s.infinite);
        QVERIFY(std::abs(s.psnr - 20 * std::log10(25.5)) < 1e-9);
        QVERIFY(std::abs(s.ssim - 6.5025 / 106.5025) < 1e-9);
    }
    void maskedBoundariesAndInvalidInput() {
        auto a = solid(0), b = solid(0);
        b.setPixelColor(0, 0, Qt::white);
        cv::Mat mask(32, 32, CV_8U, cv::Scalar(255));
        mask.at<uchar>(0, 0) = 0;
        auto s = Quality::compare(a, b, mask);
        QVERIFY(s.infinite);
        QCOMPARE(s.ssim, 1.);
        QCOMPARE(s.windows, 483);
        QVERIFY_EXCEPTION_THROWN(Quality::compare(a, solid(0, 16), mask), std::exception);
        mask.setTo(0);
        QVERIFY_EXCEPTION_THROWN(Quality::compare(a, b, mask), std::exception);
        mask.at<uchar>(16, 16) = 255;
        QVERIFY_EXCEPTION_THROWN(Quality::compare(a, b, mask), std::exception);
    }
    void ssimIndependentWindowCalculation() {
        QImage a(20, 20, QImage::Format_RGB888), b(a.size(), a.format());
        for (int y = 0; y < 20; ++y)
            for (int x = 0; x < 20; ++x) {
                a.setPixelColor(
                    x, y,
                    QColor((x * 13 + y * 7) % 256, (x * 3 + y * 9) % 256, (x * 5 + y * 11) % 256));
                b.setPixelColor(
                    x, y,
                    QColor((x * 11 + y * 8) % 256, (x * 4 + y * 7) % 256, (x * 6 + y * 9) % 256));
            }
        auto Y = [](QColor c) { return .299 * c.red() + .587 * c.green() + .114 * c.blue(); };
        double weights[11], sum = 0, expected = 0;
        for (int k = -5; k <= 5; ++k) {
            weights[k + 5] = std::exp(-k * k / 4.5);
            sum += weights[k + 5];
        }
        for (auto &w : weights)
            w /= sum;
        for (int y = 5; y < 15; ++y)
            for (int x = 5; x < 15; ++x) {
                double ma = 0, mb = 0, aa = 0, bb = 0, ab = 0;
                for (int dy = -5; dy <= 5; ++dy)
                    for (int dx = -5; dx <= 5; ++dx) {
                        double w = weights[dx + 5] * weights[dy + 5],
                               v = Y(a.pixelColor(x + dx, y + dy)),
                               u = Y(b.pixelColor(x + dx, y + dy));
                        ma += w * v;
                        mb += w * u;
                        aa += w * v * v;
                        bb += w * u * u;
                        ab += w * v * u;
                    }
                expected +=
                    (2 * ma * mb + 6.5025) * (2 * (ab - ma * mb) + 58.5225) /
                    ((ma * ma + mb * mb + 6.5025) * (aa - ma * ma + bb - mb * mb + 58.5225));
            }
        auto s = Quality::compare(a, b, cv::Mat(20, 20, CV_8U, cv::Scalar(255)));
        QVERIFY(std::abs(s.ssim - expected / 100) < 1e-10);
    }
    void isolatedCaptureRoundTripAndExport() {
        FakeQualityBoard fake;
        QualityJob job;
        job.endpoints = fake.ep;
        job.raw = QImage(2656, 1328, QImage::Format_RGB888);
        job.raw.fill(QColor(90, 120, 80));
        job.cameraTimestamp = 456;
        QSignalSpy done(&job, &QualityJob::resultReady), errors(&job, &QualityJob::failed);
        job.start();
        QTRY_VERIFY_WITH_TIMEOUT(job.isFinished(), 20000);
        QVERIFY2(errors.isEmpty(),
                 errors.isEmpty() ? "" : qPrintable(errors.first()[0].toString()));
        QCOMPARE(done.size(), 1);
        QCOMPARE(fake.uploads, 1);
        QCOMPARE(fake.poses, 1);
        auto r = qvariant_cast<std::shared_ptr<const Quality::Result>>(done.first()[0]);
        QVERIFY(r);
        QCOMPARE(r->session, fake.session);
        QCOMPARE(r->frame, quint32(1));
        QCOMPARE(r->epoch, quint32(2));
        QByteArray front, rear;
        Quality::splitPair(fake.exactInput, front, rear);
        QCOMPARE(front, r->front);
        QCOMPARE(rear, r->rear);
        QCOMPARE(r->output, fake.outputJpeg);
        QCOMPARE(r->reference.size(), QSize(2656, 1328));
        QCOMPARE(r->scores.coverage, 1.);
        QVERIFY(r->scores.ssim > .99);
        QTemporaryDir dir;
        Quality::save(*r, dir.path());
        auto loaded = Quality::load(dir.path());
        QCOMPARE(loaded.scores.psnr, r->scores.psnr);
        QCOMPARE(loaded.scores.ssim, r->scores.ssim);
        QCOMPARE(loaded.reference, r->reference);
        QCOMPARE(QImage(dir.filePath("fisheye.png")).convertToFormat(QImage::Format_RGB888),
                 Quality::inputImage(r->front, r->rear));
        // Existing saved results have only the two input JPEGs, without a preview PNG.
        QFile manifest(dir.filePath("quality.json"));
        QVERIFY(manifest.open(QIODevice::ReadOnly));
        auto root = QJsonDocument::fromJson(manifest.readAll()).object();
        manifest.close();
        auto hashes = root["files_sha256"].toObject();
        hashes.remove("fisheye.png");
        root["files_sha256"] = hashes;
        QVERIFY(manifest.open(QIODevice::WriteOnly | QIODevice::Truncate));
        manifest.write(QJsonDocument(root).toJson());
        manifest.close();
        QVERIFY(QFile::remove(dir.filePath("fisheye.png")));
        loaded = Quality::load(dir.path());
        QCOMPARE(Quality::inputImage(loaded.front, loaded.rear),
                 Quality::inputImage(r->front, r->rear));
        QFile broken(dir.filePath("fpga.jpg"));
        QVERIFY(broken.open(QIODevice::Append));
        broken.write("x");
        broken.close();
        QVERIFY_EXCEPTION_THROWN(Quality::load(dir.path()), std::exception);
        QDir().mkpath((df::projectRoot() + QStringLiteral("/build/verification/quality_example")));
        Quality::save(*r, (df::projectRoot() + QStringLiteral("/build/verification/quality_example")));
    }
    void inputPreviewPreservesLensOrderAndPixels() {
        QByteArray encoded[2];
        for (int i = 0; i < 2; ++i) {
            QImage lens(1328, 1328, QImage::Format_RGB888);
            lens.fill(i ? Qt::blue : Qt::red);
            lens.setPixelColor(700, 600, Qt::green);
            QBuffer buffer(&encoded[i]);
            QVERIFY(buffer.open(QIODevice::WriteOnly));
            QVERIFY(lens.save(&buffer, "JPEG"));
        }
        const auto image = Quality::inputImage(encoded[0], encoded[1]);
        QCOMPARE(image.size(), QSize(2656, 1328));
        for (int i = 0; i < 2; ++i)
            QCOMPARE(image.copy(i * 1328, 0, 1328, 1328),
                     QImage::fromData(encoded[i], "JPEG").convertToFormat(QImage::Format_RGB888));
        QVERIFY_EXCEPTION_THROWN(Quality::inputImage({}, encoded[1]), std::exception);
    }
    void wrongIdentityAndCancel() {
        FakeQualityBoard fake;
        fake.wrongOnly = true;
        QualityJob job;
        job.endpoints = fake.ep;
        job.raw = QImage(2656, 1328, QImage::Format_RGB888);
        job.raw.fill(Qt::green);
        QSignalSpy done(&job, &QualityJob::resultReady), errors(&job, &QualityJob::failed);
        job.start();
        QTRY_COMPARE(fake.uploads, 1);
        QTest::qWait(100);
        QCOMPARE(done.size(), 0);
        job.requestInterruption();
        QTRY_VERIFY_WITH_TIMEOUT(job.isFinished(), 3000);
        QCOMPARE(done.size(), 0);
        QCOMPARE(errors.size(), 1);
    }
    void invalidJpegFails() {
        FakeQualityBoard fake;
        fake.badJpeg = true;
        QualityJob job;
        job.endpoints = fake.ep;
        job.raw = QImage(2656, 1328, QImage::Format_RGB888);
        job.raw.fill(Qt::green);
        QSignalSpy done(&job, &QualityJob::resultReady), errors(&job, &QualityJob::failed);
        job.start();
        QTRY_VERIFY_WITH_TIMEOUT(job.isFinished(), 5000);
        QCOMPARE(done.size(), 0);
        QCOMPARE(errors.size(), 1);
    }
    void suspensionAndRestoration() {
        FakeQualityBoard fake;
        BoardVideoService service;
        service.connectToBoard(fake.ep);
        QTRY_VERIFY(service.isConnected());
        QTRY_COMPARE(fake.poses, 1);
        QSignalSpy suspended(&service, &BoardVideoService::qualitySuspended),
            resumed(&service, &BoardVideoService::qualityResumed);
        QVERIFY(service.enterQualityMode());
        QTRY_COMPARE(suspended.size(), 1);
        QVERIFY(service.qualityCanCapture());
        QVERIFY(!service.isConnected());
        QImage raw(2656, 1328, QImage::Format_RGB888);
        raw.fill(Qt::green);
        service.submit(raw);
        QTest::qWait(50);
        QCOMPARE(fake.uploads, 0);
        QCOMPARE(service.latencyTracker().snapshot().counts.value("host_inputs"), quint64(0));
        QualityJob job;
        job.endpoints = service.qualityEndpoints();
        job.raw = raw;
        QSignalSpy done(&job, &QualityJob::resultReady);
        job.start();
        QTRY_VERIFY_WITH_TIMEOUT(job.isFinished(), 20000);
        QCOMPARE(done.size(), 1);
        auto s = service.latencyTracker().snapshot();
        QCOMPARE(s.counts.value("host_inputs"), quint64(0));
        QCOMPARE(s.counts.value("processed"), quint64(0));
        service.leaveQualityMode();
        QTRY_COMPARE(resumed.size(), 1);
        QTRY_VERIFY(service.isConnected());
        service.submit(raw);
        QTRY_COMPARE(fake.uploads, 2);
        service.disconnectFromBoard();
        QTRY_VERIFY(!service.isConnected());
    }
    void returnTimeoutAndHandshakeFailure() {
        FakeQualityBoard fake;
        fake.returnFrames = false;
        QualityJob job;
        job.endpoints = fake.ep;
        job.captureTimeoutMs = 250;
        job.raw = QImage(2656, 1328, QImage::Format_RGB888);
        job.raw.fill(Qt::green);
        QSignalSpy done(&job, &QualityJob::resultReady), errors(&job, &QualityJob::failed);
        job.start();
        QTRY_VERIFY_WITH_TIMEOUT(job.isFinished(), 3000);
        QCOMPARE(fake.uploads, 1);
        QCOMPARE(done.size(), 0);
        QCOMPARE(errors.size(), 1);
        QVERIFY(errors.first()[0].toString().contains("超时"));
        QualityJob invalid;
        invalid.endpoints.localAddress = QHostAddress::AnyIPv4;
        invalid.raw = QImage(2656, 1328, QImage::Format_RGB888);
        invalid.raw.fill(Qt::green);
        QSignalSpy invalidError(&invalid, &QualityJob::failed);
        invalid.start();
        QTRY_VERIFY_WITH_TIMEOUT(invalid.isFinished(), 2000);
        QCOMPARE(invalidError.size(), 1);
        QVERIFY(invalidError.first()[0].toString().contains("IPv4"));
    }
    void pausedStateAndFormalSampleSurvive() {
        FakeQualityBoard fake;
        BoardVideoService service;
        service.connectToBoard(fake.ep);
        QTRY_VERIFY(service.isConnected());
        QTRY_COMPARE(fake.poses, 1);
        auto &tracker = service.latencyTracker();
        QTRY_COMPARE(tracker.snapshot().state, QString("实时"));
        // Create a deterministic completed sample in an active formal window.
        auto now = LatencyTracker::nowUs();
        tracker.setActive(true, "test", now - 12000000);
        QVERIFY(tracker.start(60, now - 12000000));
        auto id = tracker.accept(true, now - 1000000);
        tracker.mark(id, "paint_end", now - 976000);
        service.setPaused(true);
        QTRY_VERIFY(fake.paused);
        auto frozen = tracker.snapshot(true);
        QCOMPARE(frozen.metrics["total"].n, 1);
        QSignalSpy suspended(&service, &BoardVideoService::qualitySuspended),
            resumed(&service, &BoardVideoService::qualityResumed);
        QVERIFY(service.enterQualityMode());
        QTRY_COMPARE(suspended.size(), 1);
        service.leaveQualityMode();
        QTRY_COMPARE(resumed.size(), 1);
        QTRY_VERIFY(service.isConnected());
        QTRY_VERIFY(fake.paused);
        QImage raw(2656, 1328, QImage::Format_RGB888);
        raw.fill(Qt::green);
        service.submit(raw);
        QTest::qWait(60);
        QCOMPARE(fake.uploads, 0);
        QCOMPARE(tracker.snapshot(true).metrics["total"].mean, 24.);
        service.disconnectFromBoard();
        QTRY_VERIFY(!service.isConnected());
    }
    void closeDuringCaptureCancelsAndRestores() {
        FakeQualityBoard fake;
        fake.returnFrames = false;
        BoardVideoService service;
        service.connectToBoard(fake.ep);
        QTRY_VERIFY(service.isConnected());
        ImageQualityDialog dialog(&service);
        dialog.takeRaw = [](QImage &raw, qint64 &time) {
            raw = QImage(2656, 1328, QImage::Format_RGB888);
            raw.fill(Qt::green);
            time = 1;
            return true;
        };
        dialog.show();
        QTest::mouseClick(dialog.findChild<QPushButton *>("qualityEnter"), Qt::LeftButton);
        auto *capture = dialog.findChild<QPushButton *>("qualityCapture");
        QTRY_VERIFY(capture->isEnabled());
        QTest::mouseClick(capture, Qt::LeftButton);
        QTRY_COMPARE(fake.uploads, 1);
        dialog.close();
        QTRY_VERIFY_WITH_TIMEOUT(!dialog.isVisible(), 3000);
        QTRY_VERIFY(service.isConnected());
        QCOMPARE(service.latencyTracker().snapshot().counts.value("host_inputs"), quint64(0));
        service.disconnectFromBoard();
        QTRY_VERIFY(!service.isConnected());
    }
    void offlineRecordingQuality_data() {
        QTest::addColumn<bool>("playing");
        QTest::newRow("resume-playing") << true;
        QTest::newRow("keep-paused") << false;
    }
    void offlineRecordingQuality() {
        QFETCH(bool, playing);
        const auto directory = qEnvironmentVariable("DF_OFFLINE_TEST_DATA");
        if (directory.isEmpty())
            QSKIP("Set DF_OFFLINE_TEST_DATA to verify recording quality input");
        FakeQualityBoard fake;
        df::MainWindow window(df::projectRoot(), false);
        auto *service = window.findChild<BoardVideoService *>();
        QVERIFY(service);
        service->connectToBoard(fake.ep);
        QTRY_VERIFY(service->isConnected());
        window.settings->sourceDirectory->setText(directory);
        window.settings->sourceMode->setCurrentIndex(0);
        window.loadOfflineVideo();
        QTRY_VERIFY_WITH_TIMEOUT(window.displayedFrame().has_value(), 15000);
        QVERIFY(!window.displayedFrame()->fullRaw.isNull());
        if (!playing) {
            window.findChild<QPushButton *>("pauseButton")->click();
            QTRY_VERIFY(!window.displayedFrame()->playing);
        }
        window.findChild<QPushButton *>("imageQualityButton")->click();
        auto *dialog = window.findChild<ImageQualityDialog *>();
        QVERIFY(dialog);
        QVERIFY2(dialog->entryError().isEmpty(), qPrintable(dialog->entryError()));
        dialog->findChild<QPushButton *>("qualityEnter")->click();
        const auto frozen = *window.displayedFrame();
        auto *capture = dialog->findChild<QPushButton *>("qualityCapture");
        QTRY_VERIFY(capture->isEnabled());
        QImage raw;
        qint64 timestamp = -1;
        QVERIFY(dialog->takeRaw(raw, timestamp));
        QCOMPARE(raw, frozen.fullRaw);
        QCOMPARE(timestamp, qRound64(frozen.pts * 1000));
        const auto expectedInput = MediaWire::encode(frozen.fullRaw);
        capture->click();
        auto *job = dialog->findChild<QualityJob *>();
        QVERIFY(job);
        QSignalSpy done(job, &QualityJob::resultReady), errors(job, &QualityJob::failed);
        QTRY_COMPARE_WITH_TIMEOUT(done.size(), 1, 20000);
        QCOMPARE(errors.size(), 0);
        QCOMPARE(fake.exactInput, expectedInput);
        const auto result = qvariant_cast<std::shared_ptr<const Quality::Result>>(done.at(0).at(0));
        QCOMPARE(result->cameraTimestamp, timestamp);
        QCOMPARE(window.displayedFrame()->frame, frozen.frame);
        dialog->findChild<QPushButton *>("qualityLeave")->click();
        QTRY_VERIFY(service->isConnected());
        QTRY_COMPARE(window.displayedFrame()->playing, playing);
        if (playing)
            QTRY_VERIFY(window.displayedFrame()->frame != frozen.frame);
        else {
            QTest::qWait(250);
            QCOMPARE(window.displayedFrame()->frame, frozen.frame);
        }
        dialog->close();
        window.exitSimulation();
        window.loadSimulation();
        QCOMPARE(dialog->entryError(), QString("请先退出软件仿真"));
        window.close();
    }
    void offlineDialogAndButtonOrder() {
        BoardVideoService service;
        ImageQualityDialog dialog(&service);
        dialog.resize(1250, 760);
        dialog.show();
        auto *enter = dialog.findChild<QPushButton *>("qualityEnter");
        auto *capture = dialog.findChild<QPushButton *>("qualityCapture");
        auto *rawButton = dialog.findChild<QPushButton *>("qualityRaw");
        auto *openButton = dialog.findChild<QPushButton *>("qualityOpen");
        QVERIFY(enter);
        QVERIFY(rawButton);
        QVERIFY(openButton);
        QVERIFY(!rawButton->isEnabled());
        QVERIFY(rawButton->x() > openButton->x());
        QVERIFY(!capture->isEnabled());
        QTest::mouseClick(enter, Qt::LeftButton);
        QTRY_VERIFY(capture->isEnabled());
        QDir().mkpath((df::projectRoot() + QStringLiteral("/build/verification")));
        dialog.grab().save((df::projectRoot() + QStringLiteral("/build/verification/quality_empty.png")));
        QImage raw(2656, 1328, QImage::Format_RGB888);
        raw.fill(QColor(90, 120, 80));
        QByteArray jpeg;
        QBuffer buffer(&jpeg);
        buffer.open(QIODevice::WriteOnly);
        raw.save(&buffer, "JPEG");
        auto fixture = Quality::analyze(MediaWire::encode(raw), jpeg, 101, 2, 1, 0);
        QTemporaryDir fixtureDir;
        Quality::save(fixture, fixtureDir.path());
        dialog.openResultDirectory(fixtureDir.path());
        QTRY_VERIFY(dialog.findChild<QLabel *>("qualitySsim")->text().contains("0.99") ||
                    dialog.findChild<QLabel *>("qualitySsim")->text().contains("1.000"));
        QTRY_VERIFY(rawButton->isEnabled());
        bool matchingPreview = false;
        QTimer::singleShot(50, &dialog, [&] {
            auto *preview = dialog.findChild<QDialog *>("qualityRawDialog");
            if (!preview)
                return;
            auto *view = preview->findChild<QGraphicsView *>("qualityRawView");
            auto *identity = preview->findChild<QLabel *>("qualityRawIdentity");
            if (view && identity && view->scene()->items().size() == 1) {
                auto *item = qgraphicsitem_cast<QGraphicsPixmapItem *>(view->scene()->items().first());
                matchingPreview = item && identity->text().contains("101 / 2 / 1") &&
                    item->pixmap().toImage().convertToFormat(QImage::Format_RGB888) ==
                        Quality::inputImage(fixture.front, fixture.rear);
            }
            preview->grab().save((df::projectRoot() + QStringLiteral("/build/verification/quality_raw.png")));
            preview->reject();
        });
        QTest::mouseClick(rawButton, Qt::LeftButton);
        QVERIFY(matchingPreview);
        dialog.grab().save((df::projectRoot() + QStringLiteral("/build/verification/quality_populated.png")));
        dialog.close();
        QTRY_VERIFY(!dialog.isVisible());
        df::MainWindow window(df::projectRoot(), false);
        window.show();
        auto *quality = window.findChild<QPushButton *>("imageQualityButton");
        auto *latency = window.findChild<QPushButton *>("latencyAnalysisButton");
        QVERIFY(quality);
        QVERIFY(latency);
        QVERIFY(quality->mapTo(&window, QPoint(0, 0)).x() >
                latency->mapTo(&window, QPoint(0, 0)).x());
        window.grab().save((df::projectRoot() + QStringLiteral("/build/verification/quality_workbench.png")));
    }
};
QTEST_MAIN(QualityTests)
#include "quality_tests.moc"
