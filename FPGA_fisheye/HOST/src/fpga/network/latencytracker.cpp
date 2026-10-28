#include "latencytracker.h"
#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMutexLocker>
#include <QSaveFile>
#include <QtEndian>
#include <algorithm>
#include <chrono>
qint64 LatencyTracker::nowUs() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}
QString LatencyTracker::key(quint32 s, quint32 e, quint32 f) {
    return QString("%1/%2/%3").arg(s).arg(e).arg(f);
}
const QVector<LatencyMetric> &LatencyTracker::definitions() {
    static const QVector<LatencyMetric> d = {
        {"total", "输入→首次绘制", "Host", "input", "paint_end"},
        {"roundtrip", "输入→回图", "Host", "input", "rx"},
        {"ready", "输入→结果就绪", "Host", "input", "render_end"},
        {"encode_wait", "编码等待", "Host", "input", "encode_start"},
        {"encode", "双鱼眼 JPEG 编码", "Host", "encode_start", "encode_end"},
        {"send_wait", "编码后发送等待", "Host", "encode_end", "tx_first"},
        {"upload", "上传（传输子项）", "Host", "tx_first", "tx_end"},
        {"transport", "首片发送→回图（含板端）", "Host", "tx_first", "rx"},
        {"decode_wait", "解码等待", "Host", "rx", "decode_start"},
        {"decode", "JPEG 解码", "Host", "decode_start", "decode_end"},
        {"render_wait", "渲染等待", "Host", "decode_end", "render_start"},
        {"render", "球面与透视渲染", "Host", "render_start", "render_end"},
        {"ui_wait", "UI 取图等待", "Host", "render_end", "ui_take"},
        {"paint_wait", "重绘等待", "Host", "ui_take", "paint_start"},
        {"paint", "绘制", "Host", "paint_start", "paint_end"},
        {"ps_decode", "双路解码整体（PS 观测）", "PS/PL", {}, {}},
        {"ps_front", "前向解码（PS 观测）", "PS/PL", {}, {}},
        {"ps_rear", "后向解码（PS 观测）", "PS/PL", {}, {}},
        {"pl_cycles", "校正核周期时间（含访存/背压）", "PS/PL", {}, {}},
        {"ps_correct", "校正（PS 观测）", "PS/PL", {}, {}},
        {"ps_encode", "编码（PS 观测，含等像素）", "PS/PL", {}, {}},
        {"ps_dw", "解码启动等待", "PS/PL", {}, {}},
        {"ps_cw", "校正启动等待", "PS/PL", {}, {}},
        {"ps_ew", "编码启动等待", "PS/PL", {}, {}},
        {"ps_rx", "RX 首片→输入就绪", "板端", {}, {}},
        {"ps_ready", "READY 就绪→接纳", "板端", {}, {}},
        {"ps_pipe", "PIPE 接纳→结果就绪", "板端", {}, {}},
        {"ps_outq", "OUTQ 输出就绪→首片发送", "板端", {}, {}},
        {"ps_tx", "TX 首片→末片发送调用完成", "板端", {}, {}},
        {"ps_total", "TOTAL 板端驻留", "板端", {}, {}}};
    return d;
}
QMap<QString, double> LatencyTracker::values(const LatencyFrame &f) {
    QMap<QString, double> v;
    for (const auto &d : definitions()) {
        if (d.group == "Host") {
            if (!f.hostSelected || !f.raw)
                continue;
            if (f.host.contains(d.begin) && f.host.contains(d.end) &&
                f.host[d.end] >= f.host[d.begin])
                v[d.key] = (f.host[d.end] - f.host[d.begin]) / 1000.;
        } else if (f.boardSelected) {
            if (d.group == "板端" && d.key != "ps_pipe" && f.boardStatus != "sent")
                continue;
            if (d.key == "pl_cycles") {
                if (f.clockHz && f.processedAt)
                    v[d.key] = double(f.cycles) * 1000. / f.clockHz;
            } else if (f.ps.contains(d.key))
                v[d.key] = f.ps[d.key] / 1000.;
        }
    }
    return v;
}
LatencySummary LatencyTracker::summarize(QVector<double> a) {
    LatencySummary r;
    r.n = a.size();
    if (a.isEmpty())
        return r;
    r.latest = a.last();
    double sum = 0;
    for (double x : a)
        sum += x;
    r.mean = sum / a.size();
    r.maximum = *std::max_element(a.cbegin(), a.cend());
    return r;
}
quint64 LatencyTracker::accept(bool raw, qint64 now) {
    QMutexLocker l(&mutex);
    if (!active)
        return 0;
    if (!now)
        now = nowUs();
    advance(now);
    LatencyFrame f;
    f.id = ++nextId;
    f.raw = raw;
    f.host["input"] = now;
    records[f.id] = f;
    capture(f);
    return f.id;
}
void LatencyTracker::capture(const LatencyFrame &f) {
    if (phase != "预热" && phase != "采集中" && phase != "补齐中")
        return;
    bool h =
        f.host.contains("input") && f.host["input"] >= formalStart && f.host["input"] < formalEnd;
    bool b = f.processedAt >= formalStart && f.processedAt < formalEnd && f.processedAt;
    if (h || b) {
        auto copy = f;
        copy.hostSelected = h;
        copy.boardSelected = b;
        captured[f.id] = copy;
    }
}
void LatencyTracker::mark(quint64 id, const QString &point, qint64 at) {
    QMutexLocker l(&mutex);
    if (!id || !records.contains(id))
        return;
    if (!at)
        at = nowUs();
    advance(at);
    auto &f = records[id];
    if (f.host.contains("paint_end"))
        return;
    // Render and UI marks follow the actual image about to be painted; earlier queue marks are
    // immutable.
    if (!f.host.contains(point) || point.startsWith("render_") || point == "ui_take" ||
        point == "paint_start")
        f.host[point] = at;
    if (point == "paint_end")
        f.hostStatus = "painted";
    capture(f);
}
void LatencyTracker::status(quint64 id, const QString &reason) {
    QMutexLocker l(&mutex);
    if (!records.contains(id) || records[id].host.contains("paint_end"))
        return;
    records[id].hostStatus = reason;
    capture(records[id]);
}
void LatencyTracker::bind(quint64 id, quint32 s, quint32 e, quint32 f) {
    QMutexLocker l(&mutex);
    if (!records.contains(id))
        return;
    auto &r = records[id];
    r.session = s;
    r.epoch = e;
    r.frame = f;
    keys[key(s, e, f)] = id;
    capture(r);
}
quint64 LatencyTracker::lookup(quint32 s, quint32 e, quint32 f) const {
    QMutexLocker l(&mutex);
    return keys.value(key(s, e, f));
}
bool LatencyTracker::board(quint32 s, quint32 e, quint32 frame, const QByteArray &body,
                           qint64 now) {
    if (body.size() != 160 || !frame)
        return false;
    const auto *p = reinterpret_cast<const uchar *>(body.constData());
    auto u = [&](int o) { return qFromBigEndian<quint32>(p + o); };
    if (u(0) != 0x4c415431 || u(4) != 1 || u(8) < 1 || u(8) > 4 || u(60) || u(152) || u(156))
        return false;
    const unsigned event = u(8);
    if (event == 2 && quint64(u(64)) + u(68) + u(72) + u(76) + u(80) != u(84))
        return false;
    for (int i = 0; i < 8; i += 2)
        if (u(88 + i * 4) > u(92 + i * 4))
            return false;
    for (int i = 1; i < (event == 2 ? 6 : 4); ++i)
        if (u(120 + i * 4) < u(116 + i * 4))
            return false;
    QMutexLocker l(&mutex);
    if (!active)
        return true;
    if (!now)
        now = nowUs();
    advance(now);
    extended = true;
    telemetryAttempts = qMax(telemetryAttempts, u(16));
    telemetryFailures = qMax(telemetryFailures, u(20));
    auto k = key(s, e, frame);
    auto id = keys.value(k);
    if (!id) {
        id = ++nextId;
        keys[k] = id;
        records[id].id = id;
        records[id].session = s;
        records[id].epoch = e;
        records[id].frame = frame;
    }
    auto &f = records[id];
    f.clockHz = u(12);
    f.cycles = u(56);
    f.cyclesValid = true;
    if (event == 1 && !f.processedAt) {
        f.processedAt = now;
        const QString names[] = {"ps_decode", "ps_correct", "ps_encode", "ps_front",
                                 "ps_rear",   "ps_dw",      "ps_cw",     "ps_ew"};
        for (int i = 0; i < 8; ++i)
            f.ps[names[i]] = u(24 + i * 4);
        for (int i = 0; i < 8; ++i)
            f.ps[QString("stage_%1").arg(i)] = u(88 + i * 4);
        f.ps["ps_rx"] = u(64);
        f.ps["ps_ready"] = u(68);
        f.ps["ps_pipe"] = u(72);
    }
    if (event != 1 && !f.egressAt) {
        f.egressAt = now;
        f.boardStatus = event == 2 ? "sent" : event == 3 ? "output_replaced" : "send_failed";
        if (event == 2) {
            const QString names[] = {"ps_rx",   "ps_ready", "ps_pipe",
                                     "ps_outq", "ps_tx",    "ps_total"};
            for (int i = 0; i < 6; ++i)
                f.ps[names[i]] = u(64 + i * 4);
        }
    }
    for (int i = 0; i < 6; ++i)
        if (event == 2 || i < 4)
            f.ps[QString("boundary_%1").arg(i)] = u(120 + i * 4);
    capture(f);
    return true;
}
void LatencyTracker::legacyCycles(quint32 s, quint32 e, quint32 frame, quint32 cycles) {
    QMutexLocker l(&mutex);
    auto id = keys.value(key(s, e, frame));
    if (!id || !records.contains(id))
        return;
    auto &f = records[id];
    if (!f.processedAt) {
        f.cycles = cycles;
        f.cyclesValid = true;
        capture(f);
    }
}
void LatencyTracker::legacy(const QString &key, double ms) {
    QMutexLocker l(&mutex);
    fallback[key] = ms;
}
void LatencyTracker::counters(const QMap<QString, quint64> &a) {
    QMutexLocker l(&mutex);
    if (!active)
        return;
    for (auto it = a.begin(); it != a.end(); ++it) {
        if (absoluteCounters.contains(it.key()) && it.value() >= absoluteCounters[it.key()])
            counterTotals[it.key()] += it.value() - absoluteCounters[it.key()];
        absoluteCounters[it.key()] = it.value();
    }
}
void LatencyTracker::advance(qint64 now) {
    if (!liveStart)
        liveStart = now;
    if (phase == "预热" && now >= formalStart) {
        phase = "采集中";
        formalCounterBase = counterTotals;
    }
    if (phase == "采集中" && now >= formalEnd) {
        formalCounterEnd = counterTotals;
        phase = "补齐中";
        drainEnd = formalEnd + 2000000;
        if (endReason.isEmpty())
            endReason = "completed";
    }
    if (phase == "预热" || phase == "采集中" || phase == "补齐中") {
        formalFallback = fallback;
        formalExtended = extended;
        formalAttempts = telemetryAttempts;
        formalFailures = telemetryFailures;
        if (phase != "补齐中")
            formalCounterEnd = counterTotals;
    }
    if (phase == "补齐中" && now >= drainEnd) {
        phase = "已完成";
    }
}
void LatencyTracker::boundary(const QString &reason, qint64 now) {
    QMutexLocker l(&mutex);
    boundaryLocked(reason, now);
}
void LatencyTracker::boundaryLocked(const QString &reason, qint64 now) {
    if (!now)
        now = nowUs();
    advance(now);
    if (phase == "预热" || phase == "采集中" || phase == "补齐中") {
        formalEnd = qMax(formalStart, qMin(formalEnd, now));
        endReason = reason;
        formalCounterEnd = counterTotals;
        formalFallback = fallback;
        formalExtended = extended;
        formalAttempts = telemetryAttempts;
        formalFailures = telemetryFailures;
        phase = "已中断";
    }
    records.clear();
    keys.clear();
    fallback.clear();
    extended = false;
    telemetryAttempts = telemetryFailures = 0;
    liveStart = now;
    absoluteCounters.clear();
    counterTotals.clear();
    counterHistory.clear();
}
void LatencyTracker::setActive(bool value, const QString &reason, qint64 now) {
    QMutexLocker l(&mutex);
    boundaryLocked(reason, now);
    active = value;
}
bool LatencyTracker::start(int seconds, qint64 now) {
    QMutexLocker l(&mutex);
    if (!active || phase == "预热" || phase == "采集中" || phase == "补齐中")
        return false;
    if (!now)
        now = nowUs();
    if (seconds != 60 && seconds != 180 && seconds != 600)
        return false;
    captured.clear();
    requestedSeconds = seconds;
    formalStart = now + 10000000;
    formalEnd = formalStart + qint64(seconds) * 1000000;
    phase = "预热";
    endReason.clear();
    formalCounterBase = formalCounterEnd = counterTotals;
    formalExtended = extended;
    formalFallback = fallback;
    formalAttempts = telemetryAttempts;
    formalFailures = telemetryFailures;
    return true;
}
void LatencyTracker::stop(qint64 now) {
    QMutexLocker l(&mutex);
    if (!now)
        now = nowUs();
    advance(now);
    if (phase == "预热" || phase == "采集中") {
        formalEnd = qMax(formalStart, qMin(formalEnd, now));
        drainEnd = now + 2000000;
        formalCounterEnd = counterTotals;
        phase = "补齐中";
        endReason = "user_stopped";
    }
}
void LatencyTracker::reset(qint64 now) {
    QMutexLocker l(&mutex);
    boundaryLocked("reset", now);
    captured.clear();
    phase = "实时";
    formalStart = formalEnd = 0;
    endReason.clear();
}
LatencySnapshot LatencyTracker::snapshot(bool formal, qint64 now) {
    QMutexLocker l(&mutex);
    if (!now)
        now = nowUs();
    advance(now);
    LatencySnapshot s;
    s.formal = formal && formalStart;
    s.requestedSeconds = s.formal ? requestedSeconds : 0;
    s.samplingState = phase;
    s.state = s.formal ? phase : (active ? "实时" : "未运行");
    s.reason = s.formal ? endReason : QString();
    s.begin = s.formal ? formalStart : qMax(liveStart, now - 60000000);
    s.end = s.formal ? qMax(formalStart, qMin(formalEnd, now)) : now;
    s.extended = extended;
    s.fallback = fallback;
    s.telemetryAttempts = telemetryAttempts;
    s.telemetryFailures = telemetryFailures;
    counterHistory.append({now, counterTotals});
    while (counterHistory.size() > 1 && counterHistory[1].first < now - 60000000)
        counterHistory.removeFirst();
    auto base = s.formal ? formalCounterBase
                         : (counterHistory.isEmpty() ? QMap<QString, quint64>()
                                                     : counterHistory.first().second);
    const auto &totals = s.formal ? formalCounterEnd : counterTotals;
    for (auto it = totals.begin(); it != totals.end(); ++it)
        s.counts[it.key()] =
            it.value() >= base.value(it.key()) ? it.value() - base.value(it.key()) : 0;
    if (s.formal) {
        s.extended = formalExtended;
        s.fallback.clear(); // Legacy averages have no per-frame cohort; never pass them off as
                            // formal samples.
        s.telemetryAttempts = formalAttempts;
        s.telemetryFailures = formalFailures;
    }
    const auto &source = s.formal ? captured : records;
    for (auto f : source) {
        if (!s.formal) {
            f.hostSelected = f.host.value("input") >= s.begin && f.host.value("input") < s.end;
            f.boardSelected =
                f.processedAt >= s.begin &&
                f.processedAt < s.end; // egress-only rows retained to make missing reports visible
            if (!f.hostSelected && !f.boardSelected && f.egressAt < s.begin)
                continue;
        }
        if (f.hostSelected && f.host.contains("input")) {
            ++s.counts[f.raw ? "host_inputs" : "jpeg_inputs"];
            if (f.host.contains("paint_end"))
                ++s.counts[f.raw ? "painted" : "jpeg_painted"];
            else
                ++s.counts[f.hostStatus.isEmpty() ? "host_unfinished" : f.hostStatus];
        }
        if (f.boardSelected && f.processedAt)
            ++s.counts["processed"];
        if (f.boardSelected && f.boardStatus == "sent")
            ++s.counts["sent"];
        if (f.hostSelected && f.host.contains("rx"))
            ++s.counts[f.raw ? "host_received" : "jpeg_received"];
        if (f.frame && !f.processedAt)
            ++s.counts["missing_process_report"];
        if (f.processedAt && !f.egressAt)
            ++s.counts["missing_egress_report"];
        s.frames.append(f);
    }
    for (auto it = records.begin(); it != records.end();) {
        auto age = qMax(it->host.value("input"), qMax(it->processedAt, it->egressAt));
        if (age < now - 65000000) {
            keys.remove(key(it->session, it->epoch, it->frame));
            it = records.erase(it);
        } else
            ++it;
    }
    l.unlock(); // Sorting/statistics must not block the network or codec workers.
    QMap<QString, QVector<QPair<qint64, double>>> samples;
    for (const auto &f : s.frames) {
        auto v = values(f);
        for (auto it = v.begin(); it != v.end(); ++it)
            samples[it.key()].append({it.key().startsWith("ps_") || it.key() == "pl_cycles"
                                          ? f.processedAt
                                          : f.host.value("input"),
                                      it.value()});
    }
    for (auto it = samples.begin(); it != samples.end(); ++it) {
        std::sort(it->begin(), it->end(), [](auto a, auto b) { return a.first < b.first; });
        QVector<double> a;
        for (auto x : it.value())
            a.append(x.second);
        s.metrics[it.key()] = summarize(a);
    }

    return s;
}
static QString csv(QString s) {
    s.replace('"', "\"\"");
    return '"' + s + '"';
}
bool LatencyTracker::exportSnapshot(const LatencySnapshot &s, const QString &directory,
                                    QString *error) {
    QDir dir(directory);
    if (!dir.exists() && !dir.mkpath(".")) {
        if (error)
            *error = "无法创建导出目录";
        return false;
    }
    QJsonObject root{{"schema_version", 1},
                     {"formal", s.formal},
                     {"requested_seconds", s.requestedSeconds},
                     {"warmup_seconds", s.formal ? 10 : 0},
                     {"host_window_begin_us", QString::number(s.begin)},
                     {"host_window_end_us", QString::number(s.end)},
                     {"state", s.state},
                     {"reason", s.reason},
                     {"duration_seconds", qMax<qint64>(0, s.end - s.begin) / 1000000.},
                     {"output_width", 2656},
                     {"output_height", 1328},
                     {"clock_source", "PS XSA corrector/aclk; Host/PS local monotonic"},
                     {"host_version", "latency-v1"},
                     {"ps_version", s.extended ? "LAT1-v1" : "unknown"},
                     {"bitstream_version", "unknown"},
                     {"total_definition", "Host raw submit to first perspective QWidget paint "
                                          "completion; excludes camera and display scanout"}};
    QJsonObject ms, counts, legacy;
    for (auto it = s.metrics.begin(); it != s.metrics.end(); ++it) {
        auto m = it.value();
        ms[it.key()] = QJsonObject{
            {"samples", m.n}, {"latest_ms", m.latest}, {"mean_ms", m.mean}, {"max_ms", m.maximum}};
    }
    for (auto it = s.counts.begin(); it != s.counts.end(); ++it)
        counts[it.key()] = double(it.value());
    for (auto it = s.fallback.begin(); it != s.fallback.end(); ++it)
        legacy[it.key()] = it.value();
    root["metrics"] = ms;
    root["counts"] = counts;
    root["legacy_window_or_latest_ms"] = legacy;
    root["telemetry_attempts_session"] = double(s.telemetryAttempts);
    root["telemetry_send_failures_session"] = double(s.telemetryFailures);
    QJsonObject fps;
    double seconds = qMax<qint64>(0, s.end - s.begin) / 1000000.;
    for (auto k : {"host_inputs", "processed", "sent", "painted"})
        fps[k] = seconds > 0 ? QJsonValue(s.counts.value(k) / seconds) : QJsonValue();
    root["fps"] = fps;
    root["fps_source"] = "selected input/processed-report/egress-report/first-paint cohorts, not "
                         "inferred from latency";
    QStringList hk = {"input",   "encode_start", "encode_end", "tx_first",     "tx_end",
                      "rx",      "decode_start", "decode_end", "render_start", "render_end",
                      "ui_take", "paint_start",  "paint_end"};
    QStringList pk;
    for (auto &f : s.frames)
        for (auto it = f.ps.begin(); it != f.ps.end(); ++it)
            if (!pk.contains(it.key()))
                pk.append(it.key());
    pk.sort();
    QString text = "sample_id,session,epoch,frame,input_type,host_cohort,board_cohort,host_status,"
                   "board_status,process_report,egress_report,clock_hz,cycles";
    for (auto k : hk)
        text += ",host_" + k + "_us";
    for (auto k : pk)
        text += "," + k + "_us";
    for (auto d : definitions())
        text += "," + d.key + "_ms";
    text += '\n';
    for (const auto &f : s.frames) {
        QStringList row = {QString::number(f.id),
                           QString::number(f.session),
                           QString::number(f.epoch),
                           QString::number(f.frame),
                           f.raw ? "raw" : "jpeg",
                           QString::number(f.hostSelected),
                           QString::number(f.boardSelected),
                           csv(f.hostStatus.isEmpty() ? "unfinished" : f.hostStatus),
                           csv(f.boardStatus),
                           QString::number(bool(f.processedAt)),
                           QString::number(bool(f.egressAt)),
                           f.clockHz ? QString::number(f.clockHz) : QString(),
                           f.cyclesValid ? QString::number(f.cycles) : QString()};
        for (auto k : hk)
            row << (f.host.contains(k) ? QString::number(f.host[k]) : QString());
        for (auto k : pk)
            row << (f.ps.contains(k) ? QString::number(f.ps[k]) : QString());
        auto v = values(f);
        for (auto d : definitions())
            row << (v.contains(d.key) ? QString::number(v[d.key], 'f', 6) : QString());
        text += row.join(',') + '\n';
    }
    for (auto item : {qMakePair(QString("frames.csv"), text.toUtf8()),
                      qMakePair(QString("summary.json"), QJsonDocument(root).toJson())}) {
        QSaveFile file(dir.filePath(item.first));
        if (!file.open(QIODevice::WriteOnly) || file.write(item.second) != item.second.size() ||
            !file.commit()) {
            if (error)
                *error = file.errorString();
            return false;
        }
    }
    return true;
}
