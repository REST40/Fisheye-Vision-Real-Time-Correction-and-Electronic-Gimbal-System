#pragma once
#include <QByteArray>
#include <QMap>
#include <QMutex>
#include <QString>
#include <QVector>

struct LatencyFrame {
    quint64 id = 0;
    quint32 session = 0, epoch = 0, frame = 0, clockHz = 0, cycles = 0;
    bool raw = true, hostSelected = true, boardSelected = true, cyclesValid = false;
    qint64 processedAt = 0, egressAt = 0;
    QMap<QString, qint64> host, ps;
    QString hostStatus, boardStatus;
};
struct LatencyMetric {
    QString key, label, group, begin, end;
};
struct LatencySummary {
    int n = 0;
    double latest = 0, mean = 0, maximum = 0;
};
struct LatencySnapshot {
    QVector<LatencyFrame> frames;
    QMap<QString, LatencySummary> metrics;
    QMap<QString, quint64> counts;
    QMap<QString, double> fallback;
    QString state, reason;
    qint64 begin = 0, end = 0;
    bool formal = false, extended = false;
    int requestedSeconds = 0;
    QString samplingState;
    quint32 telemetryAttempts = 0, telemetryFailures = 0;
};
class LatencyTracker {
  public:
    static qint64 nowUs();
    static const QVector<LatencyMetric> &definitions();
    static QMap<QString, double> values(const LatencyFrame &f);
    static LatencySummary summarize(QVector<double> values);
    quint64 accept(bool raw = true, qint64 now = 0);
    void mark(quint64 id, const QString &point, qint64 at = 0);
    void status(quint64 id, const QString &reason);
    void bind(quint64 id, quint32 session, quint32 epoch, quint32 frame);
    quint64 lookup(quint32 session, quint32 epoch, quint32 frame) const;
    bool board(quint32 session, quint32 epoch, quint32 frame, const QByteArray &body,
               qint64 now = 0);
    void legacy(const QString &metric, double milliseconds);
    void legacyCycles(quint32 session, quint32 epoch, quint32 frame, quint32 cycles);
    void counters(const QMap<QString, quint64> &absolute);
    void boundary(const QString &reason, qint64 now = 0);
    void setActive(bool active, const QString &reason, qint64 now = 0);
    bool start(int seconds = 60, qint64 now = 0);
    void stop(qint64 now = 0);
    void reset(qint64 now = 0);
    LatencySnapshot snapshot(bool formal = false, qint64 now = 0);
    static bool exportSnapshot(const LatencySnapshot &, const QString &directory, QString *error);

  private:
    mutable QMutex mutex;
    QMap<quint64, LatencyFrame> records, captured;
    QMap<QString, quint64> keys;
    QMap<QString, double> fallback;
    QMap<QString, quint64> absoluteCounters, counterTotals, formalCounterBase, formalCounterEnd;
    QMap<QString, double> formalFallback;
    bool formalExtended = false;
    quint32 formalAttempts = 0, formalFailures = 0;
    QVector<QPair<qint64, QMap<QString, quint64>>> counterHistory;
    quint64 nextId = 0;
    qint64 liveStart = 0, formalStart = 0, formalEnd = 0, drainEnd = 0;
    QString phase = "实时", endReason;
    bool active = false, extended = false;
    quint32 telemetryAttempts = 0, telemetryFailures = 0;
    static QString key(quint32, quint32, quint32);
    void advance(qint64 now);
    void boundaryLocked(const QString &reason, qint64 now);
    int requestedSeconds = 0;
    void capture(const LatencyFrame &f);
};
