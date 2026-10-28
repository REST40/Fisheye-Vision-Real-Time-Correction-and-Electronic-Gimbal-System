#pragma once

#include <QMetaType>
#include <QtGlobal>
#include <optional>

struct PsPipelineRateSnapshot {
    quint32 decodeStarted = 0, decodeCompleted = 0, decodeFailed = 0, decodeTimeout = 0;
    quint32 correctStarted = 0, correctCompleted = 0, correctFailed = 0, correctTimeout = 0;
    quint32 encodeStarted = 0, encodeCompleted = 0, encodeFailed = 0, encodeTimeout = 0;
};

struct PsPipelineRateResult {
    double elapsedSeconds = 0.0;
    double decodeFps = 0.0, correctFps = 0.0, encodeFps = 0.0;
    quint64 decodeStarted = 0, decodeCompleted = 0, decodeBoundaryLoss = 0;
    quint64 correctStarted = 0, correctCompleted = 0, correctBoundaryLoss = 0;
    quint64 encodeStarted = 0, encodeCompleted = 0, encodeBoundaryLoss = 0;
    quint64 decodeFailed = 0, decodeTimeout = 0;
    quint64 correctFailed = 0, correctTimeout = 0;
    quint64 encodeFailed = 0, encodeTimeout = 0;
};
Q_DECLARE_METATYPE(PsPipelineRateResult)

class PsPipelineRateMeter {
  public:
    explicit PsPipelineRateMeter(qint64 durationMs = 10000);
    void start(qint64 nowMs, const PsPipelineRateSnapshot &snapshot);
    std::optional<PsPipelineRateResult> sample(qint64 nowMs, const PsPipelineRateSnapshot &snapshot,
                                               bool force = false);
    bool active() const {
        return m_active;
    }
    qint64 elapsedMs(qint64 nowMs) const;

  private:
    qint64 m_durationMs, m_startedMs = 0;
    bool m_active = false;
    PsPipelineRateSnapshot m_first;
};
