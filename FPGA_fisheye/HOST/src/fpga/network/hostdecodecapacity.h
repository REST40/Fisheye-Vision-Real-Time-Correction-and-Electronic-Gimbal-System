#pragma once

#include <QByteArray>
#include <QSize>
#include <QtGlobal>
#include <functional>

struct HostDecodeCapacityResult {
    double elapsedSeconds = 0.0;
    double fps = 0.0;
    double averageMs = 0.0;
    quint64 attempts = 0;
    quint64 completed = 0;
    quint64 failures = 0;
    quint64 sizeErrors = 0;
};

HostDecodeCapacityResult runHostDecodeCapacity(const QByteArray &jpeg, const QSize &expectedSize,
                                               int durationMs,
                                               const std::function<void(int)> &progress = {},
                                               const std::function<bool()> &keepRunning = {});
