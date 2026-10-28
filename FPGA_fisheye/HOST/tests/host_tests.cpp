#include "core/projection.h"
#include "fpga/fpga_link.h"
#include "media/paired_source.h"
#include "media/video_probe.h"
#include "simulation/simulation_service.h"
#include "ui/frameratepage.h"
#include "ui/main_window.h"
#include "ui/settings_dialog.h"
#include "ui/view_panel.h"
#include <QApplication>
#include <QDialog>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QtTest>
#include <opencv2/imgcodecs.hpp>
using namespace df;
class HostTests : public QObject {
    Q_OBJECT
  private slots:
    void initTestCase() {
        qApp->setQuitOnLastWindowClosed(false);
        cv::setNumThreads(2);
        QFile style(projectRoot() + "/resources/theme.qss");
        if (style.open(QIODevice::ReadOnly))
            qApp->setStyleSheet(QString::fromUtf8(style.readAll()));
    }
    void modelAndExport() {
        const QString root = projectRoot();
        Projection projection(root + "/config/calibration.json", {320, 160}, {101, 57}, {101, 101});
        projection.setAlignment({});
        projection.setView({});
        auto center = perspectiveRays({101, 57}, {}).at<cv::Vec3d>(28, 50);
        QVERIFY(cv::norm(center - cv::Vec3d(0, 0, 1)) < 1e-12);
        auto R = rotation(20, 30, 40);
        QVERIFY(cv::norm(cv::Mat(R * R.t() - cv::Matx33d::eye())) < 1e-12);
        cv::Mat front(1328, 1328, CV_8UC3, cv::Scalar(0, 0, 255)),
            rear(1328, 1328, CV_8UC3, cv::Scalar(255, 0, 0));
        auto rendered = projection.gimbalMap.render(front, rear);
        QCOMPARE(rendered.at<cv::Vec3b>(28, 50), cv::Vec3b(0, 0, 255));
        projection.setView({180, 0, 90});
        QCOMPARE(projection.gimbalMap.render(front, rear).at<cv::Vec3b>(28, 50),
                 cv::Vec3b(255, 0, 0));
        QVERIFY(projection.sphereDomain.at<uchar>(0, 0) == 0);
        QVERIFY(projection.sphereDomain.at<uchar>(50, 50) == 255);
        front = cv::imread((root + "/data/demo/front.png").toStdString());
        rear = cv::imread((root + "/data/demo/rear.png").toStdString());
        Alignment alignment{{1.2, -.8, .4}, 20};
        projection.setAlignment(alignment);
        projection.setView({35, 15, 105});
        QString output = root + "/build/verification/native_export";
        QDir(output).removeRecursively();
        projection.exportFrame(output, front, rear,
                               {{"directory", root + "/data/demo"},
                                {"mode", "photo"},
                                {"frame_index", 0},
                                {"pts_seconds", 0}});
        QVERIFY(QFile::exists(output + "/gimbal_lut.npz"));
        QCOMPARE(cv::imread((output + "/gimbal.png").toStdString()).size(), cv::Size(101, 57));
        auto info = readJson(output + "/simulation_parameters.json");
        QVERIFY(!info["relative_rotation_factory_verified"].toBool());
        QVERIFY(cv::countNonZero(projection.panoramaMap.mask) > 0);
    }
    void analysisPages() {
        const auto oldStyle = qApp->styleSheet();
        QFile theme(projectRoot() + "/resources/theme.qss");
        if (theme.open(QIODevice::ReadOnly))
            qApp->setStyleSheet(QString::fromUtf8(theme.readAll()));
        MainWindow window(projectRoot(), false);
        window.setFont(QFont("Noto Sans CJK SC", 10));
        window.show();
        auto *link = window.findChild<QPushButton *>("linkAnalysisButton");
        auto *delay = window.findChild<QPushButton *>("latencyAnalysisButton");
        QVERIFY(link);
        QVERIFY(delay);
        QCOMPARE(link->text(), QString("链路分析"));
        QCOMPARE(delay->text(), QString("延迟分析"));
        link->click();
        auto *dialog = window.findChild<QDialog *>("linkAnalysisDialog");
        QVERIFY(dialog && dialog->isVisible());
        QVERIFY(!dialog->isModal());
        QTest::qWait(50);
        QDir().mkpath(projectRoot() + "/build/verification");
        dialog->grab().save(projectRoot() + "/build/verification/link_analysis.png");
        auto *page = window.findChild<FrameRatePage *>();
        QVERIFY(page);
        page->setPsPipelineTestRunning(true);
        QVERIFY(page->findChild<QPushButton *>("psPipelineRateTestButton")->isEnabled());
        QVERIFY(!page->findChild<QPushButton *>("hostPsRateTestButton")->isEnabled());
        page->setPsPipelineTestRunning(false);
        page->setUploadWait(500, 1000, 400, 90, 10, 25, 0, 4);
        QVERIFY(page->findChild<QLabel *>("uploadWaitSummary")->text().contains("50.0%"));
        page->setPsWait(true, 25, 2, 40, 3);
        QVERIFY(page->findChild<QLabel *>("psWaitSummary")->text().contains("40.00"));
        page->setPsStageWait(25, 1, 12, 10);
        QVERIFY(page->findChild<QLabel *>("psStageWaitSummary")->text().contains("不能相加"));
        page->setX5InputRate(30);
        page->setPairEncodingRate(28, 12.5, 3);
        QVERIFY(page->findChild<QLabel *>("cameraRate")->text().contains("30.0"));
        QVERIFY(page->findChild<QLabel *>("transcodeRate")->text().contains("28.00"));
        QVERIFY(page->findChild<QLabel *>("transcodeStatus")->text().contains("12.50"));
        page->findChild<QPushButton *>("frameRateBack")->click();
        QVERIFY(!dialog->isVisible());
        delay->click();
        auto *placeholder = window.findChild<QDialog *>("latencyAnalysisDialog");
        QVERIFY(placeholder && placeholder->isVisible());
        QTest::qWait(30);
        placeholder->grab().save(projectRoot() + "/build/verification/latency_placeholder.png");
        placeholder->close();
        window.captureEvent(
            {{"event", "status"}, {"phase", "preview"}, {"fps", 30}, {"total_pairs", 1}});
        QCOMPARE(link->text(), QString("链路分析"));
        QCOMPARE(delay->text(), QString("延迟分析"));
        window.close();
        qApp->setStyleSheet(oldStyle);
    }
    void hdWorkerExport() {
        SimulationService worker(projectRoot() + "/config/calibration.json");
        QSignalSpy exported(&worker, &SimulationService::exported);
        QSignalSpy failures(&worker, &SimulationService::failed);
        worker.start();
        worker.load(projectRoot() + "/data/demo", false);
        worker.setView({35, 15, 105});
        std::optional<FramePacket> frame;
        QTRY_VERIFY_WITH_TIMEOUT(([&] {
                                     if (!frame) {
                                         auto value = worker.poll();
                                         if (value && value->view.yaw == 35)
                                             frame = std::move(value);
                                     }
                                     return frame.has_value();
                                 })(),
                                 10000);
        QCOMPARE(frame->view.yaw, 35.);
        QTemporaryDir output;
        QVERIFY(output.isValid());
        worker.exportFrame(output.path(), *frame);
        QTRY_COMPARE_WITH_TIMEOUT(exported.count(), 1, 30000);
        QCOMPARE(failures.count(), 0);
        QString folder = exported.at(0).at(1).toString();
        auto info = readJson(folder + "/simulation_parameters.json");
        QCOMPARE(cv::imread((folder + "/panorama.png").toStdString()).size(), cv::Size(2656, 1328));
        QCOMPARE(cv::imread((folder + "/gimbal.png").toStdString()).size(), cv::Size(1920, 1080));
        QCOMPARE(cv::imread((folder + "/sphere.png").toStdString()).size(), cv::Size(720, 720));
        QCOMPARE(info["yaw_degrees"].toDouble(), 35.);
        worker.stop();
        QVERIFY(worker.wait(5000));
    }
    void synchronizedVideo() {
        PairedSource source(projectRoot() + "/data/demo", true, {1328, 1328});
        QCOMPARE(source.pts.size(), size_t(300));
        QCOMPARE(source.duration, 10.);
        QCOMPARE(source.fps, 30.);
        QCOMPARE(verifyVideoPair(projectRoot() + "/data/demo")["paired_frames"].toInt(), 300);
        auto frame = source.read(0)[0].clone();
        source.read(299);
        QVERIFY(cv::norm(frame, source.read(0)[0], cv::NORM_INF) == 0);
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        for (auto name : {"front.png", "rear.png", "front_10s.mp4", "rear_10s.mp4"})
            QVERIFY(
                QFile::link(projectRoot() + "/data/demo/" + name, directory.path() + "/" + name));
        QFile csv(directory.path() + "/frames.csv");
        QVERIFY(csv.open(QIODevice::WriteOnly));
        csv.write("pts_us\n100000\n");
        csv.close();
        QVERIFY_EXCEPTION_THROWN(PairedSource(directory.path(), true, {1328, 1328}),
                                 std::runtime_error);
    }
    void uiAndControls() {
        MainWindow window(projectRoot(), false);
        window.show();
        window.settings->sourceMode->setCurrentIndex(1);
        window.loadSimulation();
        QTRY_VERIFY_WITH_TIMEOUT(window.displayedFrame().has_value(), 10000);
        QVERIFY(!window.displayedFrame()->video);
        for (auto dimensions : {QSize(1560, 940), QSize(1180, 760)}) {
            window.resize(dimensions);
            QTest::qWait(40);
            QCOMPARE(window.size(), dimensions);
            for (int i = 1; i < 4; ++i)
                QVERIFY(std::abs(window.panels[i]->viewport->height() -
                                 window.panels[0]->viewport->height()) <= 1);
        }
        window.settings->openPage(1);
        QVERIFY(!window.settings->isModal());
        window.settings->step->setValue(7);
        window.settings->directions[2]->click();
        QTRY_COMPARE(window.displayedFrame()->view.yaw, 7.);
        QTRY_COMPARE(window.displayedFrame()->view.pitch, 7.);
        window.settings->hfov->setValue(110);
        QTRY_COMPARE(window.displayedFrame()->view.hfov, 110.);
        window.settings->close();
        QVERIFY(window.isVisible());
        window.settings->sourceMode->setCurrentIndex(0);
        QTRY_VERIFY_WITH_TIMEOUT(
            window.displayedFrame().has_value() && window.displayedFrame()->video, 10000);
        auto *pause = window.findChild<QPushButton *>("pauseButton");
        QVERIFY(pause);
        pause->click();
        QTRY_VERIFY(!window.displayedFrame()->playing);
        auto frozen = window.displayedFrame()->pts;
        QTest::qWait(120);
        QCOMPARE(window.displayedFrame()->pts, frozen);
        window.settings->yaw->setValue(450);
        QTRY_COMPARE(window.displayedFrame()->view.yaw, 45.);
        QCOMPARE(window.displayedFrame()->pts, frozen);
        auto *seek = window.findChild<QSlider *>("simulationSeek");
        QVERIFY(seek);
        seek->setValue(9800);
        QMetaObject::invokeMethod(seek, "sliderReleased");
        QTRY_VERIFY(window.displayedFrame()->pts > 9.7);
        pause->click();
        QTRY_VERIFY_WITH_TIMEOUT(
            window.displayedFrame()->playing && window.displayedFrame()->pts < 1, 3000);
        window.close();
        QTRY_VERIFY_WITH_TIMEOUT(!window.isVisible(), 10000);
        QVERIFY(!window.settings->isVisible());
    }
    void captureGuards() {
        MainWindow window(projectRoot(), false);
        auto *record = window.findChild<QPushButton *>("recordButton");
        auto *snapshot = window.findChild<QPushButton *>("snapshotButton");
        QVERIFY(record && snapshot);
        QVERIFY(!record->isEnabled());
        QCOMPARE(window.settings->exposure->currentData().toString(), QString("keep"));
        QVERIFY(!window.settings->iso->isEnabled());
        QJsonObject ready{
            {"event", "status"}, {"phase", "preview"}, {"recording", false}, {"gop_ready", true}};
        window.captureEvent(ready);
        QVERIFY(record->isEnabled());
        QVERIFY(!window.settings->resolution->isEnabled());
        record->click();
        QVERIFY(!record->isEnabled());
        auto active = ready;
        active["recording"] = true;
        window.captureEvent(active);
        QVERIFY(record->isEnabled());
        snapshot->click();
        QVERIFY(!snapshot->isEnabled());
        window.captureEvent(
            {{"event", "operation_error"}, {"cmd", "snapshot"}, {"message", "readback rejected"}});
        QVERIFY(snapshot->isEnabled());
        ready["snapshot_path"] = "/tmp/previous-pair";
        window.captureEvent(ready);
        snapshot->click();
        QVERIFY(!snapshot->isEnabled());
        window.captureEvent(ready);
        QVERIFY(!snapshot->isEnabled());
        ready["snapshot_path"] = "/tmp/new-pair";
        window.captureEvent(ready);
        QVERIFY(snapshot->isEnabled());
        QPushButton *captureButton = nullptr;
        for (auto *item : window.findChildren<QPushButton *>())
            if (item->property("role") == "dataset")
                captureButton = item;
        QVERIFY(captureButton);
        ready["lens_path"] = "/tmp/previous-dataset";
        window.captureEvent(ready);
        captureButton->click();
        QVERIFY(!captureButton->isEnabled());
        window.captureEvent(ready);
        QVERIFY(!captureButton->isEnabled());
        ready["lens_busy"] = true;
        window.captureEvent(ready);
        QVERIFY(!captureButton->isEnabled());
        ready["lens_busy"] = false;
        ready["lens_path"] = "/tmp/new-dataset";
        window.captureEvent(ready);
        QVERIFY(captureButton->isEnabled());
        window.captureEvent({{"event", "stopping"}});
        QVERIFY(!record->isEnabled());
    }
    void fpgaReserved() {
        FpgaLink link;
        QSignalSpy spy(&link, &FpgaLink::unavailable);
        QVERIFY(!link.protocolAvailable());
        QVERIFY(!link.connectBoard("", 0));
        QCOMPARE(spy.count(), 1);
    }
};
QTEST_MAIN(HostTests)
#include "host_tests.moc"
