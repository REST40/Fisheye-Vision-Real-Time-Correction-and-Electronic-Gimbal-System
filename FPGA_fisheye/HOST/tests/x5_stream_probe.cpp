#include "core/types.h"
#include "fpga/network/boardvideoservice.h"
#include "ui/settings_dialog.h"
#include "x5/capture_service.h"
#include <QApplication>
#include <QDir>
#include <QJsonDocument>
#include <QTimer>
#include <iostream>
extern "C" {
#include "fixtures/ps_protocol/fptz_protocol.h"
}
int main(int argc, char **argv) {
    QApplication app(argc, argv);
    const auto root = df::projectRoot();
    df::SettingsDialog settings(root);
    df::CaptureService capture(root);
    BoardVideoService renderer;
    QJsonObject report;
    QString error, path;
    int frames = 0;
    bool stopping = false;
    qint64 last = -1;
    QTimer timer;
    QElapsedTimer elapsed;
    elapsed.start();
    auto stop = [&] {
        if (!stopping) {
            stopping = true;
            capture.shutdown();
            timer.stop();
        }
    };
    QObject::connect(&capture, &df::CaptureService::error, &app, [&](const QString &s) {
        error = s;
        stop();
    });
    QObject::connect(&capture, &df::CaptureService::ended, &app, [&](int code) {
        if (code && error.isEmpty())
            error = "camera process failed";
        report["passed"] = error.isEmpty() && frames >= 10;
        report["frames_checked"] = frames;
        report["local_fisheye_rendering_disabled"] = true;
        report["error"] = error;
        report["elapsed_ms"] = elapsed.elapsed();
        df::writeJson(root + "/build/verification/realtime_probe.json", report);
        std::cout << QJsonDocument(report).toJson().constData();
        app.exit(report["passed"].toBool() ? 0 : 1);
    });
    QObject::connect(&timer, &QTimer::timeout, &app, [&] {
        QImage raw;
        qint64 timestamp = 0;
        if (capture.fullRaw(raw, timestamp)) {
            if (raw.size() != QSize(2656, 1328)) {
                error = QString("unexpected input %1x%2").arg(raw.width()).arg(raw.height());
                stop();
                return;
            }
            if (last >= 0 && timestamp <= last) {
                error = "non-increasing camera timestamp";
                stop();
                return;
            }
            last = timestamp;
            ++frames;
            renderer.submit(raw, timestamp);
            if (frames == 1) {
                QString codecError;
                auto pair = MediaWire::encode(raw, &codecError);
                FptzJpegPair parsed{};
                if (pair.isEmpty() ||
                    fptz_jpeg_pair_parse(reinterpret_cast<const uint8_t *>(pair.constData()),
                                         pair.size(), &parsed)) {
                    error = "actual camera JPEG incompatible: " + codecError;
                    stop();
                    return;
                }
                report["raw_width"] = raw.width();
                report["raw_height"] = raw.height();
                report["pair_bytes"] = pair.size();
                report["front_jpeg_bytes"] = int(parsed.length[0]);
                report["rear_jpeg_bytes"] = int(parsed.length[1]);
                report["ps_jpeg_validator_passed"] = true;
            }
        }
        if (!renderer.takeSphere().isNull() || !renderer.takeDisplay().image.isNull()) {
            error = "bottom views must wait for board panorama";
            stop();
            return;
        }
        if (frames >= 10)
            stop();
        if (elapsed.elapsed() > 30000) {
            error = "camera readiness timeout";
            stop();
        }
    });
    QObject::connect(&capture, &df::CaptureService::event, &app, [&](const QJsonObject &data) {
        if (data["event"].toString() == "session")
            path = data["path"].toString();
    });
    auto config = settings.captureSettings();
    config["output_root"] = root + "/build/verification/realtime_sessions";
    config["exposure_mode"] = "keep";
    config["encoder"] = "libx264";
    config["seconds"] = 0;
    QDir().mkpath(root + "/build/verification");
    capture.start(config);
    timer.start(50);
    QTimer::singleShot(60000, &app, [&] {
        if (!stopping)
            error = "overall timeout";
        app.exit(1);
    });
    return app.exec();
}
