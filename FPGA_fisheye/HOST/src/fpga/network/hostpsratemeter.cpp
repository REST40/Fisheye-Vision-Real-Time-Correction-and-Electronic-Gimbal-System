#include "hostpsratemeter.h"

#include <algorithm>

namespace {
quint64 delta32(quint32 current, quint32 first) {
    return quint32(current - first);
}
} // namespace

HostPsRateMeter::HostPsRateMeter(qint64 durationMs)
    : m_durationMs(std::max<qint64>(1, durationMs)) {}

void HostPsRateMeter::start(qint64 nowMs, const HostPsRateSnapshot &snapshot) {
    m_startedMs = nowMs;
    m_first = snapshot;
    m_active = true;
}

qint64 HostPsRateMeter::elapsedMs(qint64 nowMs) const {
    return m_active ? std::max<qint64>(0, nowMs - m_startedMs) : 0;
}

std::optional<HostPsRateResult>
HostPsRateMeter::sample(qint64 nowMs, const HostPsRateSnapshot &snapshot, bool force) {
    if (!m_active)
        return std::nullopt;
    const qint64 elapsed = elapsedMs(nowMs);
    if (!force && elapsed < m_durationMs)
        return std::nullopt;
    if (elapsed <= 0)
        return std::nullopt;

    HostPsRateResult result;
    result.elapsedSeconds = elapsed / 1000.0;
    result.hostFrames = snapshot.hostFrames - m_first.hostFrames;
    result.hostBytes = snapshot.hostBytes - m_first.hostBytes;
    result.hostAttempts = snapshot.hostAttempts - m_first.hostAttempts;
    result.hostTimeouts = snapshot.hostTimeouts - m_first.hostTimeouts;
    result.hostWriteFailures = snapshot.hostWriteFailures - m_first.hostWriteFailures;
    result.hostWriteStalls = snapshot.hostWriteStalls - m_first.hostWriteStalls;
    result.hostReplacements = snapshot.hostReplacements - m_first.hostReplacements;
    const quint64 durationMs = snapshot.hostDurationMsTotal - m_first.hostDurationMsTotal;
    result.hostAverageUploadMs =
        result.hostFrames ? double(durationMs) / double(result.hostFrames) : 0.0;
    result.hostMaxUploadMs = snapshot.hostWindowMaxDurationMs;
    result.psFrames = delta32(snapshot.psFrames, m_first.psFrames);
    result.psBytes = delta32(snapshot.psBytes, m_first.psBytes);
    result.missingFrames =
        result.hostFrames > result.psFrames ? result.hostFrames - result.psFrames : 0;
    result.hostFps = result.hostFrames / result.elapsedSeconds;
    result.psFps = result.psFrames / result.elapsedSeconds;
    result.hostMBps = result.hostBytes / result.elapsedSeconds / 1000000.0;
    result.psMBps = result.psBytes / result.elapsedSeconds / 1000000.0;
    result.incompleteDrop = quint32(snapshot.incompleteDrop - m_first.incompleteDrop);
    result.crcDrop = quint32(snapshot.crcDrop - m_first.crcDrop);
    result.staleDrop = quint32(snapshot.staleDrop - m_first.staleDrop);
    result.noSlotDrop = quint32(snapshot.noSlotDrop - m_first.noSlotDrop);
    m_active = false;
    return result;
}
