#include "path_dialog.h"
#include "latencyanalysispage.h"
#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QFileDialog>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QMessageBox>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QTabWidget>
#include <QTableWidget>
#include <QTimer>
#include <QVBoxLayout>

class LatencyPlot : public QWidget {
  public:
    LatencySnapshot data;
    QString metric = "total";
    LatencyFrame frame;
    bool detail = false;
    explicit LatencyPlot(QWidget *p = nullptr) : QWidget(p) {
        setMinimumHeight(210);
    }
    void paintEvent(QPaintEvent *) override {
        QPainter p(this);
        p.fillRect(rect(), QColor("#15191e"));
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(QColor("#bbc4d0"));
        if (detail) {
            struct Bar {
                QString name;
                qint64 a, b, origin;
                QColor color;
            };
            QVector<Bar> bars;
            auto add = [&](QString label, QString a, QString b) {
                if (frame.host.contains(a) && frame.host.contains(b))
                    bars.append({label, frame.host[a], frame.host[b], frame.host.value("input"),
                                 QColor("#67d1e8")});
            };
            add("Host 编码", "encode_start", "encode_end");
            add("Host 首包→回图", "tx_first", "rx");
            add("Host 解码", "decode_start", "decode_end");
            add("Host 渲染", "render_start", "render_end");
            add("Host 绘制", "paint_start", "paint_end");
            const QString names[] = {"PS 前向解码", "PS 后向解码", "PS 校正", "PS 编码"};
            for (int i = 0; i < 4; ++i) {
                auto a = QString("stage_%1").arg(i * 2), b = QString("stage_%1").arg(i * 2 + 1);
                if (frame.ps.contains(a) && frame.ps.contains(b))
                    bars.append({names[i], frame.ps[a], frame.ps[b], 0, QColor("#e9bf68")});
            }
            if (bars.isEmpty()) {
                p.drawText(rect(), Qt::AlignCenter, "暂无可用帧时间线");
                return;
            }
            qint64 hostMax = 1, psMax = 1;
            for (auto b : bars) {
                auto &maximum = b.color == QColor("#67d1e8") ? hostMax : psMax;
                maximum = qMax(maximum, b.b - b.origin);
            }
            p.drawText(12, 20, "Host 时间线 / PS 时间线（分别缩放）");
            int y = 43;
            for (auto b : bars) {
                p.setPen(QColor("#bbc4d0"));
                p.drawText(12, y + 12, b.name);
                double scale =
                    double(width() - 320) / double(b.color == QColor("#67d1e8") ? hostMax : psMax);
                double x = 150 + (b.a - b.origin) * scale, w = qMax(2., (b.b - b.a) * scale);
                p.fillRect(QRectF(x, y, w, 14), b.color);
                p.drawText(width() - 150, y + 12,
                           QString("%1–%2 ms")
                               .arg((b.a - b.origin) / 1000., 0, 'f', 2)
                               .arg((b.b - b.origin) / 1000., 0, 'f', 2));
                y += 23;
            }
            return;
        }
        QVector<double> means(60, -1);
        QVector<QVector<double>> bins(60);
        qint64 end = data.end, start = qMax(data.begin, end - 60000000);
        for (const auto &f : data.frames) {
            auto v = LatencyTracker::values(f);
            if (!v.contains(metric))
                continue;
            qint64 at = metric == "ps_pipe" ? f.processedAt : f.host.value("input");
            int bin = int((at - (end - 60000000)) / 1000000);
            if (at >= start && bin >= 0 && bin < 60)
                bins[bin].append(v[metric]);
        }
        double top = 1;
        for (int i = 0; i < 60; ++i)
            if (!bins[i].isEmpty()) {
                auto s = LatencyTracker::summarize(bins[i]);
                means[i] = s.mean;
                top = qMax(top, s.mean);
            }
        QRectF area(60, 30, width() - 85, height() - 65);
        p.drawText(12, 20, "ms · 每秒均值（青色）；无样本处断开");
        for (int i = 0; i <= 4; ++i) {
            double y = area.bottom() - area.height() * i / 4.;
            p.setPen(QColor("#303944"));
            p.drawLine(QPointF(area.left(), y), QPointF(area.right(), y));
            p.setPen(QColor("#bbc4d0"));
            p.drawText(QRectF(0, y - 9, 54, 18), Qt::AlignRight,
                       QString::number(top * i / 4., 'f', 1));
        }
        {
            const auto &values = means;
            p.setPen(QPen(QColor("#67d1e8"), 2));
            QPainterPath path;
            bool connected = false;
            for (int i = 0; i < 60; ++i) {
                if (values[i] < 0) {
                    connected = false;
                    continue;
                }
                QPointF point(area.left() + area.width() * i / 59.,
                              area.bottom() - area.height() * values[i] / top);
                if (connected)
                    path.lineTo(point);
                else
                    path.moveTo(point);
                p.drawEllipse(point, 2, 2);
                connected = true;
            }
            p.drawPath(path);
        }
        p.setPen(QColor("#bbc4d0"));
        p.drawText(60, height() - 10, "−60 秒");
        p.drawText(width() - 65, height() - 10, "窗口末尾");
    }
};
static QString ms(double value) {
    return QString::number(value, 'f', 2);
}
LatencyAnalysisPage::LatencyAnalysisPage(LatencyTracker *t, QWidget *parent)
    : QWidget(parent), tracker(t) {
    setObjectName("latencyAnalysisPage");
    auto *layout = new QVBoxLayout(this);
    auto *title = new QLabel("图像链路 · 延迟分析");
    title->setObjectName("title");
    layout->addWidget(title);
    auto *row = new QHBoxLayout;
    const QString labels[] = {"输入→输出平均", "板端 PIPE 平均", "PL 校正核平均"};
    for (int i = 0; i < 3; ++i) {
        cards[i] = new QLabel(labels[i] + "\n—");
        cards[i]->setObjectName(QString("latencyCard%1").arg(i));
        cards[i]->setStyleSheet(
            "padding:10px;color:#f1f5fa;background:#252a31;border-radius:6px;font-size:16px;");
        row->addWidget(cards[i]);
    }
    layout->addLayout(row);
    rates = new QLabel;
    rates->setWordWrap(true);
    rates->setObjectName("latencyRates");
    layout->addWidget(rates);
    auto *controls = new QHBoxLayout;
    duration = new QComboBox;
    for (int seconds : {60, 180, 600})
        duration->addItem(QString("%1 秒").arg(seconds), seconds);
    controls->addWidget(new QLabel("预热 10 秒 · 正式采样"));
    controls->addWidget(duration);
    startButton = new QPushButton("开始采样");
    stopButton = new QPushButton("停止");
    auto *reset = new QPushButton("重置");
    auto *save = new QPushButton("导出 CSV / JSON");
    formal = new QCheckBox("查看正式采样");
    controls->addWidget(startButton);
    controls->addWidget(stopButton);
    controls->addWidget(reset);
    controls->addWidget(formal);
    controls->addStretch();
    controls->addWidget(save);
    layout->addLayout(controls);
    auto *tabs = new QTabWidget;
    layout->addWidget(tabs, 1);
    const QString groups[] = {"Host", "PS/PL", "板端"};
    for (int i = 0; i < 3; ++i) {
        auto *table = new QTableWidget(0, 5);
        tables[i] = table;
        table->setObjectName(QString("latencyTable%1").arg(i));
        table->setHorizontalHeaderLabels({"测量项 / ms", "最新", "平均", "最大", "样本数"});
        table->setEditTriggers(QAbstractItemView::NoEditTriggers);
        table->verticalHeader()->hide();
        table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
        for (int col = 1; col < 5; ++col)
            table->horizontalHeader()->setSectionResizeMode(col, QHeaderView::ResizeToContents);
        for (auto d : LatencyTracker::definitions())
            if (d.group == groups[i]) {
                int row = table->rowCount();
                table->insertRow(row);
                auto *name = new QTableWidgetItem(d.label);
                name->setData(Qt::UserRole, d.key);
                table->setItem(row, 0, name);
                for (int col = 1; col < 5; ++col)
                    table->setItem(row, col, new QTableWidgetItem("—"));
            }
        tabs->addTab(table, groups[i]);
    }
    auto *graphs = new QWidget;
    auto *gl = new QVBoxLayout(graphs);
    metric = new QComboBox;
    metric->addItem("输入→输出总延迟", "total");
    metric->addItem("板端 PIPE", "ps_pipe");
    gl->addWidget(metric);
    plot = new LatencyPlot;
    gl->addWidget(plot);
    tabs->addTab(graphs, "延迟趋势");
    auto *detail = new QWidget;
    auto *dl = new QVBoxLayout(detail);
    frame = new QComboBox;
    dl->addWidget(frame);
    details = new QLabel;
    details->setWordWrap(true);
    details->setTextInteractionFlags(Qt::TextSelectableByMouse);
    dl->addWidget(details);
    timeline = new LatencyPlot;
    timeline->detail = true;
    timeline->setMinimumHeight(275);
    dl->addWidget(timeline);
    dl->addStretch();
    tabs->addTab(detail, "逐帧详情");
    auto *stateScroll = new QScrollArea;
    stateScroll->setWidgetResizable(true);
    stateScroll->setMaximumHeight(105);
    status = new QLabel;
    status->setWordWrap(true);
    status->setTextInteractionFlags(Qt::TextSelectableByMouse);
    status->setObjectName("latencyStatus");
    stateScroll->setWidget(status);
    layout->addWidget(stateScroll);
    auto *back = new QPushButton("返回工作台");
    layout->addWidget(back);
    connect(back, &QPushButton::clicked, this, &LatencyAnalysisPage::backRequested);
    connect(startButton, &QPushButton::clicked, this, [this] {
        if (tracker->start(duration->currentData().toInt()))
            formal->setChecked(true);
        else
            QMessageBox::information(this, "采样",
                                     "请连接板卡并保持正常视频运行；已有采样结束后才能重新开始。");
        refresh();
    });
    connect(stopButton, &QPushButton::clicked, this, [this] {
        tracker->stop();
        refresh();
    });
    connect(reset, &QPushButton::clicked, this, [this] {
        tracker->reset();
        formal->setChecked(false);
        refresh();
    });
    connect(formal, &QCheckBox::toggled, this, [this] { refresh(); });
    connect(metric, &QComboBox::currentIndexChanged, this, [this] {
        plot->metric = metric->currentData().toString();
        plot->update();
    });
    connect(frame, &QComboBox::currentIndexChanged, this, [this] { showFrame(); });
    connect(save, &QPushButton::clicked, this, [this] {
        auto snapshot = tracker->snapshot(formal->isChecked());
        QString folder = df::selectProjectDirectory(this, "选择导出父目录");
        if (folder.isEmpty())
            return;
        folder += "/latency_" + QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss_zzz");
        QString error;
        if (!LatencyTracker::exportSnapshot(snapshot, folder, &error))
            QMessageBox::warning(this, "导出失败", error);
        else
            QMessageBox::information(this, "导出完成", folder);
    });
    auto *timer = new QTimer(this);
    timer->setInterval(1000);
    connect(timer, &QTimer::timeout, this, &LatencyAnalysisPage::refresh);
    timer->start();
    refresh();
}
void LatencyAnalysisPage::refresh() {
    current = tracker->snapshot(formal->isChecked());
    auto value = [&](QString key) {
        auto m = current.metrics.value(key);
        return m.n ? ms(m.mean) + " ms" : QString("—");
    };
    cards[0]->setText("输入→输出平均\n" + value("total"));
    cards[1]->setText("板端 PIPE 平均\n" + value("ps_pipe"));
    cards[2]->setText("PL 校正核平均\n" + value("pl_cycles"));
    double seconds = qMax<qint64>(0, current.end - current.begin) / 1000000.;
    auto fps = [&](QString k) {
        return seconds > 0 ? QString::number(current.counts.value(k) / seconds, 'f', 2)
                           : QString("—");
    };
    rates->setText(
        QString("样本观测：输入 / 处理 / 回传 / 绘制：%1 / %2 / %3 / %4 FPS　|　输入 %5 · "
                "板端完成 %6 · 已绘制 %7　|　窗口 %8 秒")
            .arg(fps("host_inputs"), fps("processed"), fps("sent"), fps("painted"))
            .arg(current.counts.value("host_inputs"))
            .arg(current.counts.value("processed"))
            .arg(current.counts.value("painted"))
            .arg(seconds, 0, 'f', 1));
    for (auto *table : tables)
        for (int row = 0; row < table->rowCount(); ++row) {
            auto key = table->item(row, 0)->data(Qt::UserRole).toString();
            auto m = current.metrics.value(key);
            QStringList cells =
                m.n ? QStringList{ms(m.latest), ms(m.mean), ms(m.maximum), QString::number(m.n)}
                    : QStringList{"—", "—", "—", "0"};
            if (!m.n && !current.extended && current.fallback.contains(key))
                cells[0] = ms(current.fallback[key]) + "*";
            for (int col = 1; col < 5; ++col)
                table->item(row, col)->setText(cells[col - 1]);
        }
    const QMap<QString, QString> labels = {{"host_inputs", "原图输入"},
                                           {"jpeg_inputs", "直接 JPEG 输入"},
                                           {"processed", "板端处理完成"},
                                           {"sent", "板端回传完成"},
                                           {"painted", "首次绘制完成"},
                                           {"host_received", "Host 收齐"},
                                           {"host_unfinished", "未绘制/未完成"},
                                           {"raw_queue_replaced", "原图替换"},
                                           {"encoded_queue_replaced", "编码结果替换"},
                                           {"upload_queue_replaced", "待上传替换"},
                                           {"decode_queue_replaced", "待解码替换"},
                                           {"display_queue_replaced", "待显示替换"},
                                           {"paint_queue_replaced", "待绘制替换"},
                                           {"return_timeout", "回图超时"},
                                           {"upload_aborted", "上传中止"},
                                           {"upload_failed", "上传失败"},
                                           {"upload_rejected", "上传拒绝"},
                                           {"upload_paused", "上传暂停"},
                                           {"upload_budget_exceeded", "上传预算超限"},
                                           {"upload_ledger_full", "在途帧已满"},
                                           {"encode_failed", "Host 编码失败"},
                                           {"decode_failed", "Host 解码失败"},
                                           {"render_failed", "Host 渲染失败"},
                                           {"missing_process_report", "处理报告待到达/缺失"},
                                           {"missing_egress_report", "回传报告待到达/缺失"},
                                           {"ps_decode_completed", "PS 解码完成计数增量"},
                                           {"ps_correct_completed", "PS 校正完成计数增量"},
                                           {"ps_encode_completed", "PS 编码完成计数增量"},
                                           {"jpeg_painted", "直接 JPEG 绘制"},
                                           {"jpeg_received", "直接 JPEG 回图"},
                                           {"ps_ready_replaced", "PS READY 替换"},
                                           {"ps_output_replaced", "PS 输出替换"},
                                           {"ps_output_failed", "PS 发送失败"},
                                           {"ps_input_incomplete", "PS 输入不完整"},
                                           {"ps_input_crc", "PS 校验失败"},
                                           {"ps_stale_epoch", "PS 过期输入"},
                                           {"ps_no_slot", "PS 无空槽"}};
    QStringList counts;
    for (auto it = current.counts.begin(); it != current.counts.end(); ++it)
        counts << labels.value(it.key(), it.key()) + "=" + QString::number(it.value());
    status->setText(QString("%1 %2　|　%3\n遥测尝试 %4 / 发送失败 %5（session 累计）；%6")
                        .arg(current.state, current.reason,
                             current.extended ? "LAT1 逐帧遥测" : "等待逐帧遥测 / 旧 PS 回退")
                        .arg(current.telemetryAttempts)
                        .arg(current.telemetryFailures)
                        .arg(counts.join(" · ")));
    const auto sampling = current.samplingState;
    if (sampling == "预热" && current.formal)
        status->setText(status->text() +
                        QString("\n预热剩余 %1 秒")
                            .arg(qMax<qint64>(
                                0, (current.begin - LatencyTracker::nowUs() + 999999) / 1000000)));
    if (sampling == "采集中" && current.formal)
        status->setText(
            status->text() +
            QString("\n已采集 %1 / %2 秒").arg(seconds, 0, 'f', 0).arg(current.requestedSeconds));
    if (!current.formal && (sampling == "预热" || sampling == "采集中" || sampling == "补齐中"))
        status->setText(status->text() + "\n正式采样正在进行");
    bool running = sampling == "预热" || sampling == "采集中" || sampling == "补齐中";
    stopButton->setEnabled(running);
    startButton->setEnabled(!running);
    plot->data = current;
    plot->update();
    auto selected = frame->currentData().toULongLong();
    QSignalBlocker block(frame);
    frame->clear();
    frame->addItem("最新帧", quint64(0));
    for (int i = current.frames.size() - 1; i >= qMax(0, int(current.frames.size()) - 200); --i) {
        auto &f = current.frames[i];
        frame->addItem(QString("样本 %1 · session %2 / epoch %3 / frame %4")
                           .arg(f.id)
                           .arg(f.session)
                           .arg(f.epoch)
                           .arg(f.frame),
                       f.id);
    }
    int index = frame->findData(selected);
    frame->setCurrentIndex(index < 0 ? 0 : index);
    showFrame();
}
void LatencyAnalysisPage::showFrame() {
    LatencyFrame f;
    auto id = frame->currentData().toULongLong();
    if (!current.frames.isEmpty())
        f = current.frames.last();
    if (id)
        for (auto x : current.frames)
            if (x.id == id) {
                f = x;
                break;
            }
    details->setText(QString("样本 %1 · Host %2 · PS %3 · 处理报告 %4 / 回传结束报告 %5\n校正时钟 "
                             "%6 Hz · 周期 %7；Host 与板端统计分母独立。")
                         .arg(f.id)
                         .arg(f.hostStatus.isEmpty() ? "未绘制/未完成" : f.hostStatus,
                              f.boardStatus.isEmpty() ? "未收到" : f.boardStatus)
                         .arg(bool(f.processedAt))
                         .arg(bool(f.egressAt))
                         .arg(f.clockHz ? QString::number(f.clockHz) : QString("未知"))
                         .arg(f.cyclesValid ? QString::number(f.cycles) : QString("—")));
    timeline->frame = f;
    timeline->update();
}
