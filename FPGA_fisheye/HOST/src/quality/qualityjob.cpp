#include "qualityjob.h"
#include "fpga/network/mediawire.h"
#include <QEventLoop>
#include <QTimer>
#include <stdexcept>
QualityJob::QualityJob(QObject *parent) : QThread(parent) {
    qRegisterMetaType<std::shared_ptr<const Quality::Result>>();
}
QualityJob::~QualityJob() {
    requestInterruption();
    wait();
}
void QualityJob::run() {
    try {
        auto cancel = [this] { return isInterruptionRequested(); };
        auto check = [&] {
            if (cancel())
                throw std::runtime_error("操作已取消");
        };
        check();
        if (operation == Open) {
            auto r = std::make_shared<Quality::Result>(Quality::load(directory, cancel));
            check();
            emit resultReady(r);
            return;
        }
        if (operation == Save) {
            if (!existing)
                throw std::runtime_error("没有可保存结果");
            Quality::save(*existing, directory);
            check();
            emit saved(directory);
            return;
        }
        QString error;
        emit progress("编码同帧双路 JPEG…");
        QByteArray pair = MediaWire::encode(raw, &error);
        raw = {};
        check();
        if (pair.isEmpty())
            throw std::runtime_error(error.toUtf8().constData());
        QByteArray output;
        quint32 session = 0, epoch = 0, frame = 0;
        bool uploaded = false, submitted = false, stopping = false;
        qint64 stopAt = 0;
        BoardConnection board;
        // No LatencyTracker and no normal stream observers are attached here.
        QEventLoop loop;
        QTimer timeout, poll;
        auto stop = [&] {
            if (stopping)
                return;
            stopping = true;
            stopAt = MediaWire::nowMs();
            board.stopAndDisconnect();
        };
        connect(&board, &BoardConnection::connectedChanged, &board, [&](bool connected) {
            if (connected) {
                session = board.session();
                board.requestPose(0, 0, 360);
            } else {
                if (!stopping)
                    error = "板卡连接已断开，样本无效";
                loop.quit();
            }
        });
        connect(&board, &BoardConnection::statusChanged, &board, [&](const QString &text) {
            emit progress(text);
            if (!board.isConnected() && !board.isConnecting()) {
                if (!stopping)
                    error = text;
                loop.quit();
            }
        });
        connect(&board, &BoardConnection::poseResolved, &board,
                [&](bool accepted, double, double, double, quint32 e, int) {
                    if (!accepted || (epoch && epoch != e)) {
                        error = "板端视图未就绪或 epoch 发生变化";
                        stop();
                        return;
                    }
                    epoch = e;
                });
        connect(&board, &BoardConnection::streamChanged, &board, [&](bool running, bool paused) {
            if (running && !paused && !submitted && !stopping) {
                submitted = true;
                epoch = board.epoch();
                emit progress("上传一个测试帧，等待匹配回图…");
                board.submitJpeg(pair, MediaWire::nowMs(), quint32(cameraTimestamp));
            }
        });
        connect(&board, &BoardConnection::frameSent, &board, [&](quint32 f) {
            frame = f;
            uploaded = true;
        });
        connect(&board, &BoardConnection::jpegReady, &board,
                [&](const QByteArray &jpeg, quint32 f, quint32 e, double) {
                    if (stopping || !uploaded || f != frame || e != epoch ||
                        board.session() != session)
                        return;
                    output = jpeg;
                    stop();
                });
        if (captureTimeoutMs <= 0)
            throw std::runtime_error("无效的采样超时配置");
        timeout.setSingleShot(true);
        connect(&timeout, &QTimer::timeout, &board, [&] {
            error = QString("采样超时：%1 秒内未收到匹配的完整回图").arg(captureTimeoutMs / 1000.);
            stop();
        });
        connect(&poll, &QTimer::timeout, &board, [&] {
            if (cancel()) {
                error = "操作已取消";
                stop();
            }
            if (stopping && (!board.isConnected() || MediaWire::nowMs() - stopAt >= 1500)) {
                board.disconnectFromBoard();
                loop.quit();
            }
        });
        timeout.start(captureTimeoutMs);
        poll.start(20);
        emit progress("连接独立测试会话，准备完整全景…");
        board.connectToBoard(endpoints);
        if (board.isConnected() || board.isConnecting())
            loop.exec();
        timeout.stop();
        poll.stop();
        stopping = true;
        board.disconnectFromBoard();
        check();
        if (!error.isEmpty() || output.isEmpty())
            throw std::runtime_error(error.isEmpty() ? "没有完整回图" : error.toUtf8().constData());
        emit progress("正在生成软件参考全景并计算 PSNR / SSIM…");
        auto result = std::make_shared<Quality::Result>(
            Quality::analyze(pair, output, session, epoch, frame, cameraTimestamp, cancel));
        check();
        emit resultReady(result);
    } catch (const std::exception &e) {
        emit failed(QString::fromUtf8(e.what()));
    }
}
