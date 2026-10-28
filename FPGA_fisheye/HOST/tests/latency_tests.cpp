#include "core/types.h"
#include "fpga/network/fptzprotocol.h"
#include "fpga/network/latencytracker.h"
#include "ui/latencyanalysispage.h"
#include "ui/view_panel.h"
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QTabWidget>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QtEndian>
#include <QtTest>

class LatencyTests : public QObject {
    Q_OBJECT
    static QByteArray report(int event = 1) {
        QByteArray b(160, 0);
        auto u = [&](int offset, quint32 value) {
            qToBigEndian(value, reinterpret_cast<uchar *>(b.data()) + offset);
        };
        u(0, 0x4c415431);
        u(4, 1);
        u(8, event);
        u(12, 166664993);
        u(16, event);
        u(56, 166664993);
        const quint32 stages[] = {1100, 2000, 2300, 1000, 1050, 100, 200, 150};
        for (int i = 0; i < 8; ++i)
            u(24 + i * 4, stages[i]);
        const quint32 bounds[] = {0, 100, 200, 4000, 4100, 4500};
        for (int i = 0; i < 6; ++i)
            u(120 + i * 4, (event != 2 && i > 3) ? 0 : bounds[i]);
        for (int i = 0; i < (event == 2 ? 5 : 3); ++i)
            u(64 + i * 4, bounds[i + 1] - bounds[i]);
        if (event == 2)
            u(84, 4500);
        const quint32 trace[] = {300, 1300, 350, 1400, 1600, 3600, 1550, 3850};
        for (int i = 0; i < 8; ++i)
            u(88 + i * 4, trace[i]);
        return b;
    }
    static void hostFrame(LatencyTracker &t, quint64 id, qint64 base) {
        const QString keys[] = {"encode_start", "encode_end",   "tx_first",    "tx_end",
                                "rx",           "decode_start", "decode_end",  "render_start",
                                "render_end",   "ui_take",      "paint_start", "paint_end"};
        const qint64 offset[] = {1000,  4000,  5000,  6000,  12000, 13000,
                                 15000, 16000, 20000, 21000, 23000, 24000};
        for (int i = 0; i < 12; ++i)
            t.mark(id, keys[i], base + offset[i]);
    }
  private slots:
    void totalAndPartitions() {
        LatencyTracker t;
        t.setActive(true, "test", 1000000);
        auto id = t.accept(true, 1000000);
        hostFrame(t, id, 1000000);
        auto s = t.snapshot(false, 2000000);
        QCOMPARE(s.metrics["total"].mean, 24.);
        QCOMPARE(s.metrics["roundtrip"].mean, 12.);
        QCOMPARE(s.metrics["ready"].mean, 20.);
        double sum = 0;
        for (auto k : {"encode_wait", "encode", "send_wait", "transport", "decode_wait", "decode",
                       "render_wait", "render", "ui_wait", "paint_wait", "paint"})
            sum += s.metrics[k].mean;
        QCOMPARE(sum, s.metrics["total"].mean);
        t.mark(id, "paint_end", 3000000);
        QCOMPARE(t.snapshot(false, 3000001).metrics["total"].mean, 24.);
        auto jpeg = t.accept(false, 3100000);
        hostFrame(t, jpeg, 3100000);
        auto final = t.snapshot(false, 4000000);
        QCOMPARE(final.metrics["total"].n, 1);
        QCOMPARE(final.counts["jpeg_inputs"], quint64(1));
    }
    void summaryAndWindow() {
        auto v = LatencyTracker::summarize(
            {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20});
        QCOMPARE(v.maximum, 20.);
        QCOMPARE(v.n, 20);
        QCOMPARE(v.mean, 10.5);
        QCOMPARE(v.latest, 20.);
        LatencyTracker t;
        t.setActive(true, "test", 1000000);
        auto id = t.accept(true, 1000000);
        hostFrame(t, id, 1000000);
        QCOMPARE(t.snapshot(false, 62000000).metrics["total"].n, 0);
    }
    void telemetryOrderingAndCompatibility() {
        LatencyTracker t;
        t.setActive(true, "test", 1000000);
        auto id = t.accept(true, 1000000);
        t.bind(id, 1, 2, 7);
        QVERIFY(t.board(1, 2, 7, report(2), 1200000));
        QCOMPARE(t.snapshot(false, 1250000).metrics["ps_pipe"].n, 0);
        QVERIFY(t.board(1, 2, 7, report(), 1300000));
        QVERIFY(t.board(1, 2, 7, report(), 1400000));
        QVERIFY(t.board(1, 2, 7, report(2), 1500000));
        auto s = t.snapshot(false, 1600000);
        QCOMPARE(s.frames.size(), 1);
        QCOMPARE(s.metrics["ps_pipe"].n, 1);
        QCOMPARE(s.metrics["ps_pipe"].mean, 3.8);
        QCOMPARE(s.metrics["pl_cycles"].mean, 1000.);
        QCOMPARE(s.counts["processed"], quint64(1));
        QCOMPARE(s.counts["sent"], quint64(1));
        QVERIFY(t.board(1, 2, 8, report(), 1700000));
        QVERIFY(t.board(1, 2, 8, report(3), 1800000));
        auto dropped = t.snapshot(false, 1900000);
        QCOMPARE(dropped.metrics["ps_pipe"].n, 2);
        QCOMPARE(dropped.metrics["ps_total"].n, 1);
        t.legacy("ps_pipe", 55);
        QCOMPARE(t.snapshot(false, 2000000).metrics["ps_pipe"].n, 2);
        t.boundary("epoch", 2100000);
        auto reset = t.snapshot(false, 2200000);
        QCOMPARE(reset.frames.size(), 0);
        QVERIFY(!reset.extended);
    }
    void legacyFallbackAndFrozenCounters() {
        LatencyTracker t;
        t.setActive(true, "test", 1000000);
        auto id = t.accept(true, 1000000);
        t.bind(id, 1, 2, 1);
        t.legacyCycles(1, 2, 1, 1234);
        t.legacy("ps_pipe", 45);
        auto old = t.snapshot(false, 2000000);
        QCOMPARE(old.frames[0].cycles, quint32(1234));
        QVERIFY(old.frames[0].cyclesValid);
        QCOMPARE(old.metrics["pl_cycles"].n, 0);
        QCOMPARE(old.fallback["ps_pipe"], 45.);
        t.counters({{"ps_output_failed", 100}});
        QVERIFY(t.start(60, 2000000));
        t.counters({{"ps_output_failed", 102}});
        t.snapshot(true, 12000000);
        t.counters({{"ps_output_failed", 105}});
        t.stop(20000000);
        auto stopped = t.snapshot(true, 23000000);
        QCOMPARE(stopped.counts["ps_output_failed"], quint64(3));
        QVERIFY(stopped.fallback.isEmpty());
        t.counters({{"ps_output_failed", 110}});
        t.boundary("disconnect", 24000000);
        auto frozen = t.snapshot(true, 25000000);
        QCOMPARE(frozen.counts["ps_output_failed"], quint64(3));
        QCOMPARE(frozen.requestedSeconds, 60);
    }
    void invalidTelemetry() {
        LatencyTracker t;
        t.setActive(true, "test", 1000000);
        auto b = report();
        QVERIFY(!t.board(1, 2, 7, b.left(159), 1100000));
        b[4] = 1;
        QVERIFY(!t.board(1, 2, 7, b, 1100000));
        b = report(2);
        b[87] = 0;
        QVERIFY(!t.board(1, 2, 7, b, 1100000));
        b = report();
        b[60] = 1;
        QVERIFY(!t.board(1, 2, 7, b, 1100000));
        QCOMPARE(t.snapshot(false, 1200000).frames.size(), 0);
    }
    void formalWarmupDrainFreeze() {
        LatencyTracker t;
        t.setActive(true, "test", 1000000);
        QVERIFY(t.start(60, 1000000));
        auto warm = t.accept(true, 2000000);
        hostFrame(t, warm, 2000000);
        QCOMPARE(t.snapshot(true, 3000000).frames.size(), 0);
        auto id = t.accept(true, 12000000);
        hostFrame(t, id, 12000000);
        t.bind(id, 1, 2, 1);
        t.board(1, 2, 1, report(), 13000000);
        auto last = t.accept(true, 70990000);
        t.mark(last, "encode_start", 70991000);
        t.stop(71000000);
        t.mark(last, "paint_end", 71900000);
        t.board(1, 2, 1, report(2), 72000000);
        auto s = t.snapshot(true, 74000000);
        QCOMPARE(s.state, QString("已完成"));
        QCOMPARE(s.metrics["total"].n, 2);
        QCOMPARE(s.counts["sent"], quint64(1));
        auto late = t.accept(true, 75000000);
        hostFrame(t, late, 75000000);
        QCOMPARE(t.snapshot(true, 76000000).frames.size(), 2);
        t.boundary("disconnect", 77000000);
        auto frozen = t.snapshot(true, 78000000);
        QCOMPARE(frozen.metrics["total"].n, 2);
        QVERIFY(frozen.extended);
    }
    void formalInterruptedAndIndependentCohorts() {
        LatencyTracker t;
        t.setActive(true, "test", 1000000);
        QVERIFY(t.start(60, 1000000));
        auto warm = t.accept(true, 10000000);
        t.bind(warm, 1, 2, 1);
        hostFrame(t, warm, 10000000);
        t.board(1, 2, 1, report(), 12000000);
        auto s = t.snapshot(true, 13000000);
        QCOMPARE(s.metrics["total"].n, 0);
        QCOMPARE(s.metrics["ps_pipe"].n, 1);
        t.setActive(false, "capacity_test", 14000000);
        QCOMPARE(t.accept(true, 15000000), quint64(0));
        auto frozen = t.snapshot(true, 16000000);
        QCOMPARE(frozen.state, QString("已中断"));
        QCOMPARE(frozen.reason, QString("capacity_test"));
        QCOMPARE(frozen.metrics["ps_pipe"].n, 1);
    }
    void missingAndReplaced() {
        LatencyTracker t;
        t.setActive(true, "test", 1000000);
        auto id = t.accept(true, 1000000);
        t.status(id, "raw_queue_replaced");
        auto next = t.accept(true, 1100000);
        t.bind(next, 1, 2, 1);
        hostFrame(t, next, 1100000);
        auto s = t.snapshot(false, 1200000);
        QCOMPARE(s.counts["host_inputs"], quint64(2));
        QCOMPARE(s.counts["raw_queue_replaced"], quint64(1));
        QCOMPARE(s.counts["missing_process_report"], quint64(1));
        QCOMPARE(s.metrics["total"].n, 1);
    }
    void protocolIdentity() {
        FptzProtocol::Packet p{8, 123, 0, 456, report(), 789};
        auto wire = FptzProtocol::pack(p);
        FptzProtocol::Packet parsed;
        QVERIFY(FptzProtocol::parse(wire, &parsed));
        QCOMPARE(parsed.frame, quint32(789));
        QCOMPARE(parsed.session, quint32(123));
        QCOMPARE(parsed.payload, p.payload);
    }
    void firstPaintAndHidden() {
        df::Viewport v;
        QImage image(80, 80, QImage::Format_RGB888);
        image.fill(Qt::red);
        QSignalSpy painted(&v, &df::Viewport::painted);
        QSignalSpy replaced(&v, &df::Viewport::imageReplaced);
        v.setTrackedImage(image, 1, 1, 2, 3);
        QTest::qWait(20);
        QCOMPARE(painted.count(), 0);
        v.setTrackedImage(image, 2, 1, 2, 3);
        QCOMPARE(replaced.count(), 1);
        v.show();
        QTRY_COMPARE(painted.count(), 1);
        QCOMPARE(painted[0][0].toULongLong(), quint64(2));
        QVERIFY(painted[0][5].toLongLong() >= painted[0][4].toLongLong());
        v.update();
        QTest::qWait(20);
        QCOMPARE(painted.count(), 1);
        v.setTrackedImage(image, 2, 4, 5, 6);
        QTest::qWait(20);
        QCOMPARE(painted.count(), 1);
        v.setTrackedImage(image, 3, 4, 5, 6);
        QTRY_COMPARE(painted.count(), 2);
    }
    void exportAndUi() {
        LatencyTracker t;
        auto now = LatencyTracker::nowUs();
        t.setActive(true, "test", now - 1000000);
        auto id = t.accept(true, now - 900000);
        hostFrame(t, id, now - 900000);
        t.bind(id, 1, 2, 1);
        t.board(1, 2, 1, report(), now - 800000);
        t.board(1, 2, 1, report(2), now - 700000);
        QTemporaryDir dir;
        QString error;
        auto s = t.snapshot(false, now);
        QVERIFY2(LatencyTracker::exportSnapshot(s, dir.path(), &error), qPrintable(error));
        QFile csv(dir.filePath("frames.csv"));
        QVERIFY(csv.open(QIODevice::ReadOnly));
        auto content = csv.readAll();
        QVERIFY(content.contains("host_input_us"));
        QVERIFY(content.contains("total_ms"));
        QCOMPARE(content.count('\n'), 2);
        QFile json(dir.filePath("summary.json"));
        QVERIFY(json.open(QIODevice::ReadOnly));
        auto root = QJsonDocument::fromJson(json.readAll()).object();
        QCOMPARE(root["metrics"].toObject()["total"].toObject()["samples"].toInt(), 1);
        QFile theme((df::projectRoot() + QStringLiteral("/resources/theme.qss")));
        if (theme.open(QIODevice::ReadOnly))
            qApp->setStyleSheet(QString::fromUtf8(theme.readAll()));
        LatencyAnalysisPage page(&t);
        page.setFont(QFont("Noto Sans CJK SC", 10));
        page.resize(1250, 820);
        page.show();
        QTest::qWait(50);
        QCOMPARE(page.findChildren<QTableWidget *>().size(), 3);
        QVERIFY(page.findChild<QLabel *>("latencyCard0")->text().contains("24.00"));
        QDir().mkpath((df::projectRoot() + QStringLiteral("/build/verification")));
        page.grab().save((df::projectRoot() + QStringLiteral("/build/verification/latency_populated.png")));
        auto *tabs = page.findChild<QTabWidget *>();
        tabs->setCurrentIndex(3);
        QTest::qWait(20);
        page.grab().save((df::projectRoot() + QStringLiteral("/build/verification/latency_trend.png")));
        tabs->setCurrentIndex(4);
        QTest::qWait(20);
        page.grab().save((df::projectRoot() + QStringLiteral("/build/verification/latency_frame.png")));
    }
};
QTEST_MAIN(LatencyTests)
#include "latency_tests.moc"
