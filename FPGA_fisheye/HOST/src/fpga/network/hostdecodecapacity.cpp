#include "hostdecodecapacity.h"

#include <QElapsedTimer>
#include <QImage>

HostDecodeCapacityResult runHostDecodeCapacity(const QByteArray &jpeg, const QSize &expectedSize,
                                               int durationMs,
                                               const std::function<void(int)> &progress,
                                               const std::function<bool()> &keepRunning) {
    HostDecodeCapacityResult result;
    if (jpeg.isEmpty() || durationMs <= 0)
        return result;
    QElapsedTimer timer;
    timer.start();
    int lastSecond = -1;
    do {
        const int second = int(timer.elapsed() / 1000);
        if (progress && second != lastSecond) {
            lastSecond = second;
            progress(second);
        }
        ++result.attempts;
        const QImage image = QImage::fromData(jpeg, "jpeg");
        if (image.isNull())
            ++result.failures;
        else if (image.size() != expectedSize)
            ++result.sizeErrors;
        else
            ++result.completed;
    } while (timer.elapsed() < durationMs && (!keepRunning || keepRunning()));
    result.elapsedSeconds = qMax(0.001, double(timer.nsecsElapsed()) / 1000000000.0);
    result.fps = double(result.completed) / result.elapsedSeconds;
    result.averageMs =
        result.completed ? result.elapsedSeconds * 1000.0 / double(result.completed) : 0.0;
    if (progress)
        progress((durationMs + 999) / 1000);
    return result;
}
