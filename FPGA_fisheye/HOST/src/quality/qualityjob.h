#pragma once
#include "fpga/network/boardconnection.h"
#include "qualityanalysis.h"
#include <QThread>
#include <memory>
Q_DECLARE_METATYPE(std::shared_ptr<const Quality::Result>)
// Created only in exclusive quality mode. Owns sockets and image work on its thread.
class QualityJob final : public QThread {
    Q_OBJECT
  public:
    enum Operation { Capture, Open, Save };
    Operation operation = Capture;
    BoardEndpoints endpoints;
    QImage raw;
    qint64 cameraTimestamp = 0;
    int captureTimeoutMs = 30000; // bounded network deadline; may be shortened by tests
    QString directory;
    std::shared_ptr<const Quality::Result> existing;
    explicit QualityJob(QObject *parent = nullptr);
    ~QualityJob() override;
  signals:
    void progress(const QString &message);
    void resultReady(std::shared_ptr<const Quality::Result> result);
    void failed(const QString &reason);
    void saved(const QString &directory);

  protected:
    void run() override;
};
