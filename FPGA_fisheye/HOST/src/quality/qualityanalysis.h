#pragma once
#include <QImage>
#include <QJsonObject>
#include <functional>
#include <opencv2/core.hpp>

namespace Quality {
using Cancel = std::function<bool()>;
struct Scores {
    double psnr = 0, ssim = 0, coverage = 0;
    int pixels = 0, windows = 0;
    bool infinite = false;
};
struct Result {
    quint32 session = 0, epoch = 0, frame = 0;
    qint64 cameraTimestamp = 0;
    QByteArray front, rear, output;
    QImage reference, fpga;
    Scores scores;
    QString referenceId, capturedAt;
};
Scores compare(const QImage &reference, const QImage &actual, const cv::Mat &mask);
QImage renderReference(const QByteArray &pair, cv::Mat &mask, const Cancel &cancel = {});
void splitPair(const QByteArray &pair, QByteArray &front, QByteArray &rear);
Result analyze(const QByteArray &pair, const QByteArray &output, quint32 session, quint32 epoch,
               quint32 frame, qint64 timestamp, const Cancel &cancel = {});
QString referenceId();
QImage inputImage(const QByteArray &front, const QByteArray &rear);
void save(const Result &result, const QString &directory);
Result load(const QString &directory, const Cancel &cancel = {});
} // namespace Quality
