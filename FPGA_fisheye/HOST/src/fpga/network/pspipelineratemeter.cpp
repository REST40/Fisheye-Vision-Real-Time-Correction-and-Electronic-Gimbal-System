#include "pspipelineratemeter.h"
#include <algorithm>

namespace {
quint64 delta32(quint32 current, quint32 first) {
    return quint32(current - first);
}
quint64 loss(quint64 started, quint64 completed) {
    return started > completed ? started - completed : 0;
}
} // namespace

PsPipelineRateMeter::PsPipelineRateMeter(qint64 durationMs)
    : m_durationMs(std::max<qint64>(1, durationMs)) {}

void PsPipelineRateMeter::start(qint64 nowMs, const PsPipelineRateSnapshot &snapshot) {
    m_startedMs = nowMs;
    m_first = snapshot;
    m_active = true;
}

qint64 PsPipelineRateMeter::elapsedMs(qint64 nowMs) const {
    return m_active ? std::max<qint64>(0, nowMs - m_startedMs) : 0;
}

std::optional<PsPipelineRateResult>
PsPipelineRateMeter::sample(qint64 nowMs, const PsPipelineRateSnapshot &s, bool force) {
    if (!m_active)
        return std::nullopt;
    const qint64 elapsed = elapsedMs(nowMs);
    if (!force && elapsed < m_durationMs)
        return std::nullopt;
    if (elapsed <= 0)
        return std::nullopt;
    PsPipelineRateResult r;
    r.elapsedSeconds = elapsed / 1000.0;
    r.decodeStarted = delta32(s.decodeStarted, m_first.decodeStarted);
    r.decodeCompleted = delta32(s.decodeCompleted, m_first.decodeCompleted);
    r.correctStarted = delta32(s.correctStarted, m_first.correctStarted);
    r.correctCompleted = delta32(s.correctCompleted, m_first.correctCompleted);
    r.encodeStarted = delta32(s.encodeStarted, m_first.encodeStarted);
    r.encodeCompleted = delta32(s.encodeCompleted, m_first.encodeCompleted);
    r.decodeBoundaryLoss = loss(r.decodeStarted, r.decodeCompleted);
    r.correctBoundaryLoss = loss(r.correctStarted, r.correctCompleted);
    r.encodeBoundaryLoss = loss(r.encodeStarted, r.encodeCompleted);
    r.decodeFailed = delta32(s.decodeFailed, m_first.decodeFailed);
    r.decodeTimeout = delta32(s.decodeTimeout, m_first.decodeTimeout);
    r.correctFailed = delta32(s.correctFailed, m_first.correctFailed);
    r.correctTimeout = delta32(s.correctTimeout, m_first.correctTimeout);
    r.encodeFailed = delta32(s.encodeFailed, m_first.encodeFailed);
    r.encodeTimeout = delta32(s.encodeTimeout, m_first.encodeTimeout);
    r.decodeFps = r.decodeCompleted / r.elapsedSeconds;
    r.correctFps = r.correctCompleted / r.elapsedSeconds;
    r.encodeFps = r.encodeCompleted / r.elapsedSeconds;
    m_active = false;
    return r;
}
