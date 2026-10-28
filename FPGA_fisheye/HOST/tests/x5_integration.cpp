#include "core/types.h"
#include "ui/main_window.h"
#include "ui/settings_dialog.h"
#include "x5/capture_service.h"
#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonDocument>
#include <QTimer>
#include <iostream>
using namespace df;
int main(int argc, char **argv) {
    QApplication app(argc, argv);
    app.setQuitOnLastWindowClosed(false);
    QString root = projectRoot();
    QFile style(root + "/resources/theme.qss");
    if (style.open(QIODevice::ReadOnly))
        app.setStyleSheet(QString::fromUtf8(style.readAll()));
    MainWindow window(root, false);
    window.show();
    auto *capture = window.findChild<CaptureService *>();
    auto *record = window.findChild<QPushButton *>("recordButton");
    auto *snapshot = window.findChild<QPushButton *>("snapshotButton");
    QPushButton *camera = nullptr, *dataset = nullptr;
    for (auto *b : window.findChildren<QPushButton *>()) {
        if (b->property("role") == "connect")
            camera = b;
        if (b->property("role") == "dataset")
            dataset = b;
    }
    QString output = root + "/build/verification/live";
    QDir().mkpath(output);
    window.settings->recordDirectory->setText(output + "/recordings");
    window.settings->datasetDirectory->setText(output + "/datasets");
    window.settings->resolution->setCurrentIndex(1);
    window.settings->exposure->setCurrentIndex(0);
    window.settings->encoder->setCurrentIndex(0);
    QString shot, clip, data, error;
    int stage = 0;
    int attempts = 1;
    bool stopping = false;
    QElapsedTimer elapsed;
    elapsed.start();
    auto finish = [&] {
        if (!stopping) {
            stopping = true;
            camera->click();
        }
    };
    QObject::connect(capture, &CaptureService::event, &window, [&](const QJsonObject &value) {
        if (value["event"] == "operation_error") {
            error = value["message"].toString();
            finish();
            return;
        }
        if (value["event"] != "status")
            return;
        if (stage == 0 && value["gop_ready"].toBool() && snapshot->isEnabled()) {
            stage = 1;
            snapshot->click();
        } else if (stage == 1 && !value["snapshot_path"].toString().isEmpty()) {
            shot = value["snapshot_path"].toString();
            stage = 2;
            record->click();
        } else if (stage == 2 && value["recording"].toBool()) {
            stage = 3;
            QTimer::singleShot(2500, &window, [&] { record->click(); });
        } else if (stage == 3 && !value["recording"].toBool() &&
                   !value["last_clip"].toString().isEmpty()) {
            clip = value["last_clip"].toString();
            stage = 4;
            dataset->click();
        } else if (stage == 4 && value["lens_busy"].toBool())
            stage = 5;
        else if (stage == 5 && !value["lens_busy"].toBool() &&
                 !value["lens_path"].toString().isEmpty()) {
            data = value["lens_path"].toString();
            if (!value["lens_error"].toString().isEmpty())
                error = value["lens_error"].toString();
            finish();
        }
    });
    QObject::connect(capture, &CaptureService::error, &window,
                     [&](const QString &value) { error = value; });
    QObject::connect(capture, &CaptureService::ended, &window, [&](int code) {
        if (code && stage == 0 && attempts < 2 && error.contains("USB") && error.contains("同步")) {
            ++attempts;
            error.clear();
            stopping = false;
            QTimer::singleShot(500, &window, [&] { camera->click(); });
            return;
        }

        QJsonObject report{{"passed", code == 0 && stage == 5 && error.isEmpty()},
                           {"stage", stage},
                           {"attempts", attempts},
                           {"elapsed_seconds", elapsed.elapsed() / 1000.},
                           {"session", capture->sessionPath},
                           {"snapshot", shot},
                           {"clip", clip},
                           {"dataset", data},
                           {"error", error},
                           {"return_code", code}};
        writeJson(output + "/integration.json", report);
        std::cout << QJsonDocument(report).toJson().toStdString() << std::flush;
        window.saveScreenshot(output + "/capture_window.png");
        app.exit(report["passed"].toBool() ? 0 : 1);
    });
    QTimer::singleShot(100, &window, [&] { camera->click(); });
    QTimer::singleShot(110000, &window, [&] {
        error = "实机测试超时";
        finish();
    });
    return app.exec();
}
