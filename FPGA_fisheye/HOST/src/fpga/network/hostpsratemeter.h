#pragma once

#include <QtGlobal>
#include <optional>

struct HostPsRateSnapshot {
    quint64 hostFrames = 0;
    quint64 hostBytes = 0;
    quint32 psFrames = 0;
    quint32 psBytes = 0;
    quint32 incompleteDrop = 0;
    quint32 crcDrop = 0;
    quint32 staleDrop = 0;
    quint32 noSlotDrop = 0;
    quint64 hostAttempts = 0;
    quint64 hostTimeouts = 0;
    quint64 hostWriteFailures = 0;
    quint64 hostReplacements = 0;
    quint64 hostDurationMsTotal = 0;
    quint64 hostWindowMaxDurationMs = 0;
    quint64 hostWriteStalls = 0;
};

struct HostPsRateResult {
    double elapsedSeconds = 0.0;
    quint64 hostFrames = 0;
    quint64 psFrames = 0;
    quint64 hostBytes = 0;
    quint64 psBytes = 0;
    quint64 missingFrames = 0;
    double hostFps = 0.0;
    double psFps = 0.0;
    double hostMBps = 0.0;
    double psMBps = 0.0;
    quint32 incompleteDrop = 0;
    quint32 crcDrop = 0;
    quint32 staleDrop = 0;
    quint32 noSlotDrop = 0;
    quint64 hostAttempts = 0;
    quint64 hostTimeouts = 0;
    quint64 hostWriteFailures = 0;
    quint64 hostReplacements = 0;
    double hostAverageUploadMs = 0.0;
    quint64 hostMaxUploadMs = 0;
    quint64 hostWriteStalls = 0;
};

class HostPsRateMeter {
  public:
    explicit HostPsRateMeter(qint64 durationMs = 10000);
    void start(qint64 nowMs, const HostPsRateSnapshot &snapshot);
    std::optional<HostPsRateResult> sample(qint64 nowMs, const HostPsRateSnapshot &snapshot,
                                           bool force = false);
    bool active() const {
        return m_active;
    }
    qint64 elapsedMs(qint64 nowMs) const;

  private:
    qint64 m_durationMs;
    qint64 m_startedMs = 0;
    bool m_active = false;
    HostPsRateSnapshot m_first;
};
