#include "frameratepage.h"

#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QTimer>
#include <QVBoxLayout>
#include <algorithm>

namespace {
QLabel *textLabel(const QString &value, const char *name = nullptr) {
    auto *label = new QLabel(value);
    if (name)
        label->setObjectName(QString::fromLatin1(name));
    label->setWordWrap(true);
    label->setStyleSheet(QStringLiteral("color:#e2e8f0;border:none;background:transparent;"));
    return label;
}

QString attainmentText(double actualFps, double referenceFps) {
    if (actualFps < 0.0 || referenceFps <= 0.0)
        return QStringLiteral("未测试");
    const double percent = actualFps * 100.0 / referenceFps;
    const QString verdict = percent >= 95.0   ? QStringLiteral("正常")
                            : percent >= 80.0 ? QStringLiteral("偏低")
                                              : QStringLiteral("不足");
    return QStringLiteral("%1 · 达成率 %2%").arg(verdict).arg(percent, 0, 'f', 1);
}

QString equivalentCapacity(double milliseconds) {
    return milliseconds > 0.0
               ? QStringLiteral("%1 FPS（1/帧耗时估算）").arg(1000.0 / milliseconds, 0, 'f', 1)
               : QStringLiteral("待计算");
}
} // namespace

FrameRatePage::FrameRatePage(QWidget *parent) : QWidget(parent) {
    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(24, 20, 24, 20);
    auto *heading = new QHBoxLayout;
    auto *title = textLabel(QStringLiteral("视频链路 · 帧率计算与瓶颈"));
    QFont font = title->font();
    font.setPointSize(18);
    font.setBold(true);
    title->setFont(font);
    heading->addWidget(title);
    heading->addStretch();
    m_cameraTestButton = new QPushButton(QStringLiteral("开始相机输入测试"));
    m_cameraTestButton->setObjectName(QStringLiteral("cameraInputTestButton"));
    connect(m_cameraTestButton, &QPushButton::clicked, this,
            [this] { emit cameraTestRequested(!m_cameraTestRunning); });
    heading->addWidget(m_cameraTestButton);
    m_hostPsTestButton = new QPushButton(QStringLiteral("开始Host→PS测试"));
    m_hostPsTestButton->setObjectName(QStringLiteral("hostPsRateTestButton"));
    connect(m_hostPsTestButton, &QPushButton::clicked, this,
            [this] { emit hostPsTestRequested(!m_hostPsTestRunning); });
    heading->addWidget(m_hostPsTestButton);
    m_psPipelineTestButton = new QPushButton(QStringLiteral("开始PS/PL处理测试"));
    m_psPipelineTestButton->setObjectName(QStringLiteral("psPipelineRateTestButton"));
    connect(m_psPipelineTestButton, &QPushButton::clicked, this,
            [this] { emit psPipelineTestRequested(!m_psPipelineTestRunning); });
    heading->addWidget(m_psPipelineTestButton);
    m_psHostTestButton = new QPushButton(QStringLiteral("开始PS→Host测试"));
    m_psHostTestButton->setObjectName(QStringLiteral("psHostRateTestButton"));
    connect(m_psHostTestButton, &QPushButton::clicked, this,
            [this] { emit psHostTestRequested(!m_psHostTestRunning); });
    heading->addWidget(m_psHostTestButton);
    auto *back = new QPushButton(QStringLiteral("← 返回实时显示"));
    back->setObjectName(QStringLiteral("frameRateBack"));
    connect(back, &QPushButton::clicked, this, [this] {
        if (m_cameraTestRunning)
            emit cameraTestRequested(false);
        if (m_hostPsTestRunning)
            emit hostPsTestRequested(false);
        if (m_psPipelineTestRunning)
            emit psPipelineTestRequested(false);
        if (m_psHostTestRunning)
            emit psHostTestRequested(false);
        emit backRequested();
    });
    heading->addWidget(back);
    outer->addLayout(heading);

    m_summary = textLabel(QStringLiteral("等待相机采集；未连接的链路不参与瓶颈判断。"),
                          "bottleneckSummary");
    m_summary->setStyleSheet(
        QStringLiteral("color:#93c5fd;padding:12px;background:#172235;border:1px solid #31518a;"));
    outer->addWidget(m_summary);
    outer->addWidget(textLabel(QStringLiteral("一对前后JPEG计一帧。实测值按最近约 3 秒窗口，每 1 "
                                              "秒刷新。阶段耗时是单帧时间，不能直接当作该阶段最大 "
                                              "FPS；完整链路定位还需单独隔离测试。Host→PS、PS/"
                                              "PL、PS→Host测试会暂时暂停正常视频，结束后恢复。"),
                               "measurementWindowNote"));

    m_uploadWait =
        textLabel(QStringLiteral("Host上传等待：等待连接和正常视频样本"), "uploadWaitSummary");
    m_psWait = textLabel(QStringLiteral("PS等待：等待心跳统计"), "psWaitSummary");
    outer->addWidget(m_uploadWait);
    outer->addWidget(m_psWait);
    m_psStageWait = textLabel(QStringLiteral("PS阶段启动等待：等待板端样本"), "psStageWaitSummary");
    outer->addWidget(m_psStageWait);
    auto *scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    auto *content = new QWidget;
    auto *rows = new QVBoxLayout(content);
    rows->setSpacing(7);
    m_flow=textLabel(QStringLiteral("完整流水诊断：等待新版PS的FLOW统计"), "flowDiagnostics");
    rows->addWidget(m_flow);
    constexpr int actualWidth = 170;
    constexpr int referenceWidth = 210;
    constexpr int statusWidth = 190;
    auto *columns = new QHBoxLayout;
    columns->setContentsMargins(14, 0, 14, 0);
    columns->addStretch(1);
    auto addColumnHeader = [columns](const QString &title, const char *id, int width) {
        auto *label = textLabel(title, id);
        QFont font = label->font();
        font.setBold(true);
        label->setFont(font);
        label->setStyleSheet(QStringLiteral("color:#93c5fd;border:none;background:transparent;"));
        label->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        label->setFixedWidth(width);
        columns->addWidget(label);
    };
    addColumnHeader(QStringLiteral("实测（墙钟）"), "actualColumnHeader", actualWidth);
    addColumnHeader(QStringLiteral("标称/估算能力"), "referenceColumnHeader", referenceWidth);
    addColumnHeader(QStringLiteral("状态"), "statusColumnHeader", statusWidth);
    rows->addLayout(columns);
    auto addRow = [rows](const QString &name, const QString &detail, const char *actualId,
                         QLabel **actual, const char *referenceId, QLabel **reference,
                         const char *statusId, QLabel **status) {
        auto *card = new QFrame;
        card->setStyleSheet(QStringLiteral(
            "QFrame{background:#1b2432;border:1px solid #2d405d;border-radius:5px;}"));
        auto *layout = new QHBoxLayout(card);
        layout->setContentsMargins(14, 10, 14, 10);
        auto *left = new QVBoxLayout;
        auto *label = textLabel(name);
        QFont font = label->font();
        font.setBold(true);
        label->setFont(font);
        left->addWidget(label);
        auto *hint = textLabel(detail);
        hint->setStyleSheet(QStringLiteral("color:#94a3b8;border:none;background:transparent;"));
        left->addWidget(hint);
        layout->addLayout(left, 1);
        *actual = textLabel(QStringLiteral("待采集"), actualId);
        (*actual)->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        (*actual)->setFixedWidth(actualWidth);
        layout->addWidget(*actual);
        *reference = textLabel(QStringLiteral("待计算"), referenceId);
        (*reference)->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        (*reference)->setFixedWidth(referenceWidth);
        layout->addWidget(*reference);
        *status = textLabel(QStringLiteral("未测试"), statusId);
        (*status)->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        (*status)->setWordWrap(true);
        (*status)->setFixedWidth(statusWidth);
        layout->addWidget(*status);
        rows->addWidget(card);
    };
    addRow(QStringLiteral("① 相机视频输入"),
           QStringLiteral("X5完整2656×1328双鱼眼；按解码输出帧序号统计"), "cameraRate", &m_camera,
           "cameraReference", &m_cameraReference, "cameraStatus", &m_cameraStatus);
    addRow(QStringLiteral("② Host JPEG420 转换"),
           QStringLiteral("前后各1328×1328 JPEG420；一对为一帧，统计编码耗时与替换数"),
           "transcodeRate", &m_transcode, "transcodeReference", &m_transcodeReference,
           "transcodeStatus", &m_transcodeStatus);
    addRow(QStringLiteral("③ Host→PS 上传"),
           QStringLiteral("双JPEG成对对象/UDP完整发送；不代表PS已校验接收"), "uploadRate",
           &m_upload, "uploadReference", &m_uploadReference, "uploadStatus", &m_uploadStatus);
    addRow(QStringLiteral("④ PS 接收与重组"),
           QStringLiteral("需要板端独立完整接收计数；当前不拿发送数代替"), "psReceive",
           &m_psReceive, "psReceiveReference", &m_psReceiveReference, "psReceiveStatus",
           &m_psReceiveStatus);
    addRow(QStringLiteral("⑤ PL前后JPEG并行解码"), QStringLiteral("板端心跳完成计数 / 配对帧耗时"),
           "psDecode", &m_psDecode, "psDecodeReference", &m_psDecodeReference, "psDecodeStatus",
           &m_psDecodeStatus);
    addRow(QStringLiteral("⑥ PL全景校正拼接"), QStringLiteral("板端心跳完成计数 / 配对帧耗时"),
           "psCorrect", &m_psCorrect, "psCorrectReference", &m_psCorrectReference,
           "psCorrectStatus", &m_psCorrectStatus);
    addRow(QStringLiteral("⑦ PS/PL JPEG 编码"), QStringLiteral("板端心跳完成计数 / 配对帧耗时"),
           "psEncode", &m_psEncode, "psEncodeReference", &m_psEncodeReference, "psEncodeStatus",
           &m_psEncodeStatus);
    addRow(QStringLiteral("⑧ PS→Host 回传"), QStringLiteral("Host 已收到并重组完整 JPEG"),
           "returnRate", &m_return, "returnReference", &m_returnReference, "returnStatus",
           &m_returnStatus);
    addRow(QStringLiteral("⑨ Host 回图解码"),
           QStringLiteral("板卡完整全景JPEG解码成功；球体/透视显示映射另行处理"), "decodeRate",
           &m_decode, "decodeReference", &m_decodeReference, "decodeStatus", &m_decodeStatus);
    if (!m_cameraTestHasResult)
        m_cameraReference->setText(QStringLiteral("30.0 FPS（协商）"));
    m_psReceive->setText(QStringLiteral("待测"));
    m_psDecode->setText(QStringLiteral("待测"));
    m_psCorrect->setText(QStringLiteral("待测"));
    m_psEncode->setText(QStringLiteral("待测"));
    m_stageTimer = new QTimer(this);
    m_stageTimer->setSingleShot(true);
    m_stageTimer->setInterval(3000);
    connect(m_stageTimer, &QTimer::timeout, this, [this] {
        if (m_psPipelineTestRunning || m_psPipelineTestHasResult || m_havePsPipelineWindow)
            return;
        m_psDecode->setText(QStringLiteral("待测（3秒无结果）"));
        m_psCorrect->setText(QStringLiteral("待测（3秒无结果）"));
        m_psEncode->setText(QStringLiteral("待测（3秒无结果）"));
    });
    rows->addStretch();
    scroll->setWidget(content);
    outer->addWidget(scroll, 1);
}

void FrameRatePage::setCameraRates(double inputFps, double outputFps, double transcodeMs,
                                   quint64 dropped) {
    m_cameraFps = m_cameraWindow.add(inputFps);
    m_transcodeFps = m_transcodeWindow.add(outputFps);
    m_camera->setText(QStringLiteral("%1 FPS").arg(m_cameraFps, 0, 'f', 1));
    if (!m_cameraTestHasResult)
        m_cameraReference->setText(QStringLiteral("30.0 FPS（协商）"));
    m_cameraStatus->setText(attainmentText(m_cameraFps, 30.0));
    m_transcode->setText(QStringLiteral("%1 FPS").arg(m_transcodeFps, 0, 'f', 1));
    if (!m_transcodeCapacityHasResult)
        m_transcodeReference->setText(QStringLiteral("待独立测试"));
    const double capacity = transcodeMs > 0.0 ? 1000.0 / transcodeMs : 0.0;
    const QString margin =
        capacity >= outputFps * 1.1 ? QStringLiteral("有余量") : QStringLiteral("余量偏低");
    m_transcodeStatus->setText(QStringLiteral("%1 · %2 ms/帧 · 丢%3")
                                   .arg(margin)
                                   .arg(transcodeMs, 0, 'f', 1)
                                   .arg(dropped));
    updateSummary();
}

void FrameRatePage::setPsFlow(const QString &text) {
    m_flow->setText(text);
}

void FrameRatePage::setPsStageWait(quint32 samples, double decodeMs, double correctMs,
                                   double encodeMs) {
    m_psStageWait->setText(
        QStringLiteral("PS本窗 %1 帧启动等待平均：接纳→解码启动 %2 ms · 解码完成→校正启动 %3 ms · "
                       "解码完成→编码启动 %4 ms（流式区间重叠，不能相加）")
            .arg(samples)
            .arg(decodeMs, 0, 'f', 2)
            .arg(correctMs, 0, 'f', 2)
            .arg(encodeMs, 0, 'f', 2));
}
void FrameRatePage::setUploadWait(double creditMs, double activeMs, double sendingMs,
                                  double sourceMs, double readyMs, double longestCreditMs,
                                  int freeSlots, int totalSlots) {
    if (activeMs <= 0) {
        m_uploadWait->setText(QStringLiteral("Host上传等待：暂停或隔离测试中，不计入正常视频统计"));
        return;
    }
    m_uploadWait->setText(
        QStringLiteral("Host最近窗口 %1 ms：等待PS空槽 %2 ms（%3%） · 发包 %4 ms · 等待编码帧 %5 "
                       "ms · 可发送/调度 %6 ms · 最长连续空槽等待 %7 ms · 当前额度 %8/%9")
            .arg(activeMs, 0, 'f', 0)
            .arg(creditMs, 0, 'f', 0)
            .arg(creditMs * 100 / activeMs, 0, 'f', 1)
            .arg(sendingMs, 0, 'f', 0)
            .arg(sourceMs, 0, 'f', 0)
            .arg(readyMs, 0, 'f', 0)
            .arg(longestCreditMs, 0, 'f', 0)
            .arg(freeSlots)
            .arg(totalSlots));
}
void FrameRatePage::setPsWait(bool supported, quint32 samples, double readyMs, double pipelineMs,
                              double outQueueMs) {
    if (!supported) {
        m_psStageWait->setText(QStringLiteral("PS阶段启动等待：当前PS未提供统计"));
        m_psWait->setText(QStringLiteral("PS等待：当前PS未提供等待统计；需更新PS，PL无需改动"));
        return;
    }
    if (!samples) {
        m_psWait->setText(QStringLiteral("PS等待：已支持统计，等待下一窗口完整回传帧"));
        return;
    }
    m_psWait->setText(QStringLiteral("PS本窗 %1 帧平均：接收完成→接纳处理 %2 ms · 接纳→输出就绪 %3 "
                                     "ms（含内部排队/重叠处理） · 输出就绪→首包 %4 ms")
                          .arg(samples)
                          .arg(readyMs, 0, 'f', 2)
                          .arg(pipelineMs, 0, 'f', 2)
                          .arg(outQueueMs, 0, 'f', 2));
}

void FrameRatePage::setX5InputRate(double fps, double deliveredFps) {
    m_cameraFps = m_cameraWindow.add(fps);
    m_camera->setText(QStringLiteral("%1 FPS · 完整双鱼眼").arg(m_cameraFps, 0, 'f', 1));
    if (!m_cameraTestHasResult)
        m_cameraReference->setText(QStringLiteral("30.0 FPS（请求）"));
    m_cameraStatus->setText(deliveredFps >= 0 ? QStringLiteral("完整帧读取 %1 FPS · %2")
                                                    .arg(deliveredFps, 0, 'f', 2)
                                                    .arg(attainmentText(m_cameraFps, 30.0))
                                              : attainmentText(m_cameraFps, 30.0));
    updateSummary();
}

void FrameRatePage::setPairEncodingRate(double fps, double ms, quint64 replaced) {
    m_transcodeFps = m_transcodeWindow.add(fps);
    m_transcode->setText(QStringLiteral("%1 帧对/s").arg(m_transcodeFps, 0, 'f', 2));
    m_transcodeReference->setText(QStringLiteral("实时编码统计"));
    m_transcodeStatus->setText(
        QStringLiteral("%1 ms/帧对 · 替换%2").arg(ms, 0, 'f', 2).arg(replaced));
    updateSummary();
}

void FrameRatePage::setBoardRates(double uploadFps, double returnFps, double decodedFps) {
    m_uploadFps = m_uploadWindow.add(uploadFps);
    m_upload->setText(QStringLiteral("%1 FPS").arg(m_uploadFps, 0, 'f', 1));
    if (!m_hostPsTestFailed)
        m_uploadStatus->setText(QStringLiteral("本窗上传 %1 FPS").arg(uploadFps, 0, 'f', 2));
    // The isolated PS->Host test pauses the normal video stream. Ignore its
    // zero/partial normal-stream windows; keep the last live values until the
    // stream resumes, then refresh both actual columns normally.
    if (!m_psHostTestRunning) {
        if (!m_haveNormalReturnWindow) {
            m_returnFps = m_returnWindow.add(returnFps);
            m_return->setText(QStringLiteral("%1 FPS").arg(m_returnFps, 0, 'f', 1));
        }
        m_decodeFps = m_decodeWindow.add(decodedFps);
        if (!m_psHostTestHasResult)
            m_returnStatus->setText(QStringLiteral("已采集"));
        m_decode->setText(QStringLiteral("%1 FPS").arg(m_decodeFps, 0, 'f', 1));
        m_decodeStatus->setText(QStringLiteral("本窗解码 %1 FPS").arg(decodedFps, 0, 'f', 2));
    }
    updateSummary();
}

void FrameRatePage::setNormalReturnWindow(double psFps, double psMBps, double hostFps,
                                          quint64 psFrames, quint64 hostFrames,
                                          quint32 supersededDrop, quint32 sendDrop) {
    if (m_psHostTestRunning)
        return;
    m_haveNormalReturnWindow = true;
    m_returnFps = m_returnWindow.add(hostFps);
    m_return->setText(QStringLiteral("%1 FPS").arg(m_returnFps, 0, 'f', 2));
    const quint64 missing = psFrames > hostFrames ? psFrames - hostFrames : 0;
    m_returnStatus->setText(
        QStringLiteral(
            "PS发完%1 FPS/%2 MB/s · Host完整%3 FPS · 本窗%4/%5 · 覆盖%6/发送失败%7/边界缺失%8")
            .arg(psFps, 0, 'f', 2)
            .arg(psMBps, 0, 'f', 2)
            .arg(hostFps, 0, 'f', 2)
            .arg(psFrames)
            .arg(hostFrames)
            .arg(supersededDrop)
            .arg(sendDrop)
            .arg(missing));
    m_summary->setText(QStringLiteral("正常PS→Host回传边界：PS完整发送 %1 FPS，Host完整重组 %2 "
                                      "FPS；本窗覆盖%3、发送失败%4、边界缺失%5。")
                           .arg(psFps, 0, 'f', 2)
                           .arg(hostFps, 0, 'f', 2)
                           .arg(supersededDrop)
                           .arg(sendDrop)
                           .arg(missing));
}

void FrameRatePage::setPsReceiveWindow(double fps, double mbps, quint64 frames,
                                       quint32 incompleteDrop, quint32 crcDrop, quint32 staleDrop,
                                       quint32 noSlotDrop) {
    m_psReceiveFps = m_psReceiveWindow.add(fps);
    m_psReceive->setText(QStringLiteral("%1 FPS").arg(m_psReceiveFps, 0, 'f', 2));
    if (!m_hostPsTestFailed)
        m_psReceiveStatus->setText(QStringLiteral("本窗 %1帧/%2 MB/s · 残%3/CRC%4/过期%5/无槽%6")
                                       .arg(frames)
                                       .arg(mbps, 0, 'f', 2)
                                       .arg(incompleteDrop)
                                       .arg(crcDrop)
                                       .arg(staleDrop)
                                       .arg(noSlotDrop));
    updateSummary();
}

void FrameRatePage::setPsPipelineWindow(const PsPipelineRateResult &r) {
    m_havePsPipelineWindow = true;
    m_psDecodeFps = m_psDecodeWindow.add(r.decodeFps);
    m_psCorrectFps = m_psCorrectWindow.add(r.correctFps);
    m_psEncodeFps = m_psEncodeWindow.add(r.encodeFps);
    m_psDecode->setText(QStringLiteral("%1 FPS").arg(m_psDecodeFps, 0, 'f', 2));
    m_psCorrect->setText(QStringLiteral("%1 FPS").arg(m_psCorrectFps, 0, 'f', 2));
    m_psEncode->setText(QStringLiteral("%1 FPS").arg(m_psEncodeFps, 0, 'f', 2));
    if (!m_psPipelineTestFailed) {
        m_psDecodeStatus->setText(QStringLiteral("本窗 开始%1/完成%2/边界差%3 · 失败%4/超时%5")
                                      .arg(r.decodeStarted)
                                      .arg(r.decodeCompleted)
                                      .arg(r.decodeBoundaryLoss)
                                      .arg(r.decodeFailed)
                                      .arg(r.decodeTimeout));
        m_psCorrectStatus->setText(QStringLiteral("本窗 开始%1/完成%2/边界差%3 · 失败%4/超时%5")
                                       .arg(r.correctStarted)
                                       .arg(r.correctCompleted)
                                       .arg(r.correctBoundaryLoss)
                                       .arg(r.correctFailed)
                                       .arg(r.correctTimeout));
        m_psEncodeStatus->setText(QStringLiteral("本窗 开始%1/完成%2/边界差%3 · 失败%4/超时%5")
                                      .arg(r.encodeStarted)
                                      .arg(r.encodeCompleted)
                                      .arg(r.encodeBoundaryLoss)
                                      .arg(r.encodeFailed)
                                      .arg(r.encodeTimeout));
    }
    updateSummary();
}

void FrameRatePage::setBoardStages(quint32 decodeUs, quint32 correctUs, quint32 encodeUs) {
    m_stageTimer->start();
    if (!m_psPipelineTestHasResult && !m_psPipelineTestFailed) {
        m_psDecodeReference->setText(equivalentCapacity(decodeUs / 1000.0));
        m_psCorrectReference->setText(equivalentCapacity(correctUs / 1000.0));
        m_psEncodeReference->setText(equivalentCapacity(encodeUs / 1000.0));
    }
    if (m_psPipelineTestRunning || m_psPipelineTestHasResult || m_havePsPipelineWindow)
        return;
    m_psDecodeStatus->setText(
        QStringLiteral("%1 ms/帧 · 估算值").arg(decodeUs / 1000.0, 0, 'f', 1));
    m_psCorrect->setText(QStringLiteral("待测（仅收到耗时）"));
    m_psCorrectStatus->setText(
        QStringLiteral("%1 ms/帧 · 估算值").arg(correctUs / 1000.0, 0, 'f', 1));
    m_psEncode->setText(QStringLiteral("待测（仅收到耗时）"));
    m_psEncodeStatus->setText(
        QStringLiteral("%1 ms/帧 · 估算值").arg(encodeUs / 1000.0, 0, 'f', 1));
}

void FrameRatePage::setPsPipelineTestRunning(bool running) {
    m_psPipelineTestRunning = running;
    if (running) {
        m_psPipelineTestHasResult = false;
        m_psPipelineTestFailed = false;
    }
    m_psPipelineTestButton->setText(running ? QStringLiteral("停止PS/PL处理测试")
                                            : QStringLiteral("开始PS/PL处理测试"));
    m_cameraTestButton->setEnabled(!running);
    m_hostPsTestButton->setEnabled(!running);
    m_psPipelineTestButton->setEnabled(true);
    m_psHostTestButton->setEnabled(!running);
    if (running) {
        m_psDecodeReference->setText(QStringLiteral("测试中（等待PS计数）"));
        m_psCorrectReference->setText(QStringLiteral("测试中（等待PS计数）"));
        m_psEncodeReference->setText(QStringLiteral("测试中（等待PS计数）"));
        m_summary->setText(
            QStringLiteral("PS/PL处理联合实测正在等待80字节心跳基线；视频流保持运行。"));
    }
}

void FrameRatePage::setPsPipelineTestProgress(int elapsedSeconds, int totalSeconds) {
    if (!m_psPipelineTestRunning)
        return;
    m_psDecodeReference->setText(
        QStringLiteral("测试中 %1/%2秒").arg(elapsedSeconds).arg(totalSeconds));
    m_psCorrectReference->setText(
        QStringLiteral("测试中 %1/%2秒").arg(elapsedSeconds).arg(totalSeconds));
    m_psEncodeReference->setText(
        QStringLiteral("测试中 %1/%2秒").arg(elapsedSeconds).arg(totalSeconds));
    m_summary->setText(QStringLiteral("PS/PL处理联合实测：%1/%2秒；三阶段使用同一墙钟窗口。")
                           .arg(elapsedSeconds)
                           .arg(totalSeconds));
}

void FrameRatePage::setPsPipelineTestCompleted(const PsPipelineRateResult &r) {
    m_psPipelineTestRunning = false;
    m_psPipelineTestHasResult = true;
    m_psPipelineTestFailed = false;
    m_psPipelineTestButton->setText(QStringLiteral("开始PS/PL处理测试"));
    m_cameraTestButton->setEnabled(true);
    m_hostPsTestButton->setEnabled(true);
    m_psHostTestButton->setEnabled(true);
    m_psDecodeReference->setText(
        QStringLiteral("%1 FPS（阶段独立实测）").arg(r.decodeFps, 0, 'f', 2));
    m_psCorrectReference->setText(
        QStringLiteral("%1 FPS（阶段独立实测）").arg(r.correctFps, 0, 'f', 2));
    m_psEncodeReference->setText(
        QStringLiteral("%1 FPS（阶段独立实测）").arg(r.encodeFps, 0, 'f', 2));
    m_psDecodeStatus->setText(QStringLiteral("开始%1/完成%2/边界差%3 · 失败%4/超时%5")
                                  .arg(r.decodeStarted)
                                  .arg(r.decodeCompleted)
                                  .arg(r.decodeBoundaryLoss)
                                  .arg(r.decodeFailed)
                                  .arg(r.decodeTimeout));
    m_psCorrectStatus->setText(QStringLiteral("开始%1/完成%2/边界差%3 · 失败%4/超时%5")
                                   .arg(r.correctStarted)
                                   .arg(r.correctCompleted)
                                   .arg(r.correctBoundaryLoss)
                                   .arg(r.correctFailed)
                                   .arg(r.correctTimeout));
    m_psEncodeStatus->setText(QStringLiteral("开始%1/完成%2/边界差%3 · 失败%4/超时%5")
                                  .arg(r.encodeStarted)
                                  .arg(r.encodeCompleted)
                                  .arg(r.encodeBoundaryLoss)
                                  .arg(r.encodeFailed)
                                  .arg(r.encodeTimeout));
    const double minimum = std::min({r.decodeFps, r.correctFps, r.encodeFps});
    const QString stage = minimum == r.decodeFps    ? QStringLiteral("JPEG解码")
                          : minimum == r.correctFps ? QStringLiteral("PL校正")
                                                    : QStringLiteral("JPEG编码");
    m_summary->setText(
        QStringLiteral(
            "PS/PL处理实测完成：窗口%1秒，最低完成率为%2 %3 FPS；查看各行边界差、失败和超时。")
            .arg(r.elapsedSeconds, 0, 'f', 2)
            .arg(stage)
            .arg(minimum, 0, 'f', 2));
}

void FrameRatePage::setPsPipelineTestError(const QString &reason) {
    m_psPipelineTestRunning = false;
    m_psPipelineTestFailed = true;
    m_psPipelineTestButton->setText(QStringLiteral("开始PS/PL处理测试"));
    m_cameraTestButton->setEnabled(true);
    m_hostPsTestButton->setEnabled(true);
    m_psHostTestButton->setEnabled(true);
    m_psDecodeReference->setText(QStringLiteral("测试失败"));
    m_psCorrectReference->setText(QStringLiteral("测试失败"));
    m_psEncodeReference->setText(QStringLiteral("测试失败"));
    m_psDecodeStatus->setText(QStringLiteral("测试失败：%1").arg(reason));
    m_psCorrectStatus->setText(QStringLiteral("测试失败：%1").arg(reason));
    m_psEncodeStatus->setText(QStringLiteral("测试失败：%1").arg(reason));
    m_summary->setText(QStringLiteral("PS/PL处理联合实测未完成：%1").arg(reason));
}

void FrameRatePage::setPsHostTestRunning(bool running) {
    m_psHostTestRunning = running;
    if (running)
        m_psHostTestHasResult = false;
    m_psHostTestButton->setText(running ? QStringLiteral("停止PS→Host测试")
                                        : QStringLiteral("开始PS→Host测试"));
    m_cameraTestButton->setEnabled(!running);
    m_hostPsTestButton->setEnabled(!running);
    m_psPipelineTestButton->setEnabled(!running);
    m_psHostTestButton->setEnabled(true);
    if (running) {
        m_returnReference->setText(QStringLiteral("测试中（等待PS发送统计）"));
        m_returnStatus->setText(QStringLiteral("隔离实时链路；不解码、不显示"));
        m_decodeReference->setText(QStringLiteral("等待回传样本"));
        m_decodeStatus->setText(QStringLiteral("第8阶段完成后进行5秒纯JPEG解码压测"));
    }
}
void FrameRatePage::setPsHostTestProgress(int elapsedSeconds, int totalSeconds) {
    if (m_psHostTestRunning)
        m_summary->setText(
            QStringLiteral("PS→Host独立回传实测：%1/%2秒；PS DDR预置JPEG，Host只重组校验。")
                .arg(elapsedSeconds)
                .arg(totalSeconds));
}
void FrameRatePage::setPsHostTestCompleted(double elapsedSeconds, double hostFps, double hostMBps,
                                           double psFps, double psMBps, quint64 hostFrames,
                                           quint64 psFrames, quint64 missingFrames,
                                           quint64 partialFrames, quint64 badFrames,
                                           quint32 psSendFailures) {
    Q_UNUSED(psFps);
    m_psHostTestHasResult = true;
    m_returnReference->setText(QStringLiteral("%1 FPS · %2 MB/s（独立实测）")
                                   .arg(hostFps, 0, 'f', 2)
                                   .arg(hostMBps, 0, 'f', 2));
    m_returnStatus->setText(
        QStringLiteral("Host %1 / PS %2 MB/s · 帧%3/%4 · 缺失%5/残%6/坏%7/PS失败%8")
            .arg(hostMBps, 0, 'f', 2)
            .arg(psMBps, 0, 'f', 2)
            .arg(hostFrames)
            .arg(psFrames)
            .arg(missingFrames)
            .arg(partialFrames)
            .arg(badFrames)
            .arg(psSendFailures));
    m_decodeReference->setText(QStringLiteral("测试中"));
    m_decodeStatus->setText(
        QStringLiteral("正在对固定完整JPEG做5秒纯解码容量测试；不接收网络、不显示"));
    m_summary->setText(
        QStringLiteral(
            "PS→Host回传容量完成：%1 FPS，窗口%2秒，缺失%3帧；正在测试Host JPEG解码容量。")
            .arg(hostFps, 0, 'f', 2)
            .arg(elapsedSeconds, 0, 'f', 2)
            .arg(missingFrames));
}

void FrameRatePage::setHostDecodeCapacityCompleted(double elapsedSeconds, double fps,
                                                   double averageMs, quint64 attempts,
                                                   quint64 completed, quint64 failures,
                                                   quint64 sizeErrors) {
    m_psHostTestRunning = false;
    m_psHostTestButton->setText(QStringLiteral("开始PS→Host测试"));
    m_cameraTestButton->setEnabled(true);
    m_hostPsTestButton->setEnabled(true);
    m_psPipelineTestButton->setEnabled(true);
    m_decodeReference->setText(QStringLiteral("%1 FPS · %2 ms/帧（独立实测）")
                                   .arg(fps, 0, 'f', 2)
                                   .arg(averageMs, 0, 'f', 1));
    m_decodeStatus->setText(QStringLiteral("尝试%1/完成%2 · 失败%3/尺寸错%4 · 平均%5 ms/帧")
                                .arg(attempts)
                                .arg(completed)
                                .arg(failures)
                                .arg(sizeErrors)
                                .arg(averageMs, 0, 'f', 1));
    m_summary->setText(
        QStringLiteral("Host回图JPEG解码容量测试完成：窗口%1秒，%2 FPS，平均%3 ms/帧。")
            .arg(elapsedSeconds, 0, 'f', 2)
            .arg(fps, 0, 'f', 2)
            .arg(averageMs, 0, 'f', 1));
}
void FrameRatePage::setPsHostTestError(const QString &reason) {
    m_psHostTestRunning = false;
    m_psHostTestButton->setText(QStringLiteral("开始PS→Host测试"));
    m_cameraTestButton->setEnabled(true);
    m_hostPsTestButton->setEnabled(true);
    m_psPipelineTestButton->setEnabled(true);
    m_returnReference->setText(QStringLiteral("测试失败"));
    m_returnStatus->setText(QStringLiteral("测试失败：%1").arg(reason));
    m_summary->setText(QStringLiteral("PS→Host独立回传测试未完成：%1").arg(reason));
}

void FrameRatePage::resetCamera() {
    m_cameraFps = m_transcodeFps = -1;
    m_cameraWindow.clear();
    m_transcodeWindow.clear();
    m_camera->setText(QStringLiteral("待采集"));
    if (!m_cameraTestHasResult)
        m_cameraReference->setText(QStringLiteral("30.0 FPS（协商）"));
    m_cameraStatus->setText(QStringLiteral("未测试"));
    m_transcode->setText(QStringLiteral("待采集"));
    if (!m_transcodeCapacityHasResult)
        m_transcodeReference->setText(QStringLiteral("待独立测试"));
    m_transcodeStatus->setText(QStringLiteral("未测试"));
    updateSummary();
}

void FrameRatePage::resetBoard() {
    m_flow->setText(QStringLiteral("完整流水诊断：等待新版PS的FLOW统计"));
    m_psStageWait->setText(QStringLiteral("PS阶段启动等待：等待板端样本"));
    m_uploadWait->setText(QStringLiteral("Host上传等待：等待连接和正常视频样本"));
    m_psWait->setText(QStringLiteral("PS等待：等待心跳统计"));
    m_transcodeFps = -1;
    m_transcodeWindow.clear();
    m_transcode->setText(QStringLiteral("等待PS连接"));
    m_transcodeReference->setText(QStringLiteral("实时编码统计"));
    m_transcodeStatus->setText(QStringLiteral("未执行"));
    m_cameraTestButton->setEnabled(true);
    for (auto *button : {m_hostPsTestButton, m_psPipelineTestButton, m_psHostTestButton})
        button->setEnabled(!m_cameraTestRunning);
    m_stageTimer->stop();
    m_uploadFps = m_psReceiveFps = m_returnFps = m_decodeFps = -1;
    m_psDecodeFps = m_psCorrectFps = m_psEncodeFps = -1;
    m_uploadWindow.clear();
    m_psReceiveWindow.clear();
    m_psDecodeWindow.clear();
    m_psCorrectWindow.clear();
    m_psEncodeWindow.clear();
    m_returnWindow.clear();
    m_decodeWindow.clear();
    m_haveNormalReturnWindow = false;
    m_havePsPipelineWindow = false;
    m_hostPsTestRunning = false;
    m_hostPsTestHasResult = false;
    m_hostPsTestFailed = false;
    m_psPipelineTestRunning = false;
    m_psPipelineTestHasResult = false;
    m_psPipelineTestFailed = false;
    m_psHostTestRunning = false;
    m_psHostTestHasResult = false;
    m_hostPsTestButton->setText(QStringLiteral("开始Host→PS测试"));
    m_psPipelineTestButton->setText(QStringLiteral("开始PS/PL处理测试"));
    m_psHostTestButton->setText(QStringLiteral("开始PS→Host测试"));
    m_upload->setText(QStringLiteral("待连接"));
    m_uploadReference->setText(QStringLiteral("待计算"));
    m_uploadStatus->setText(QStringLiteral("未测试"));
    m_return->setText(QStringLiteral("待连接"));
    m_returnReference->setText(QStringLiteral("待计算"));
    m_returnStatus->setText(QStringLiteral("未测试"));
    m_decode->setText(QStringLiteral("待连接"));
    m_decodeReference->setText(QStringLiteral("待计算"));
    m_decodeStatus->setText(QStringLiteral("未测试"));
    m_psDecode->setText(QStringLiteral("待测"));
    m_psDecodeReference->setText(QStringLiteral("待计算"));
    m_psDecodeStatus->setText(QStringLiteral("未测试"));
    m_psCorrect->setText(QStringLiteral("待测"));
    m_psCorrectReference->setText(QStringLiteral("待计算"));
    m_psCorrectStatus->setText(QStringLiteral("未测试"));
    m_psEncode->setText(QStringLiteral("待测"));
    m_psEncodeReference->setText(QStringLiteral("待计算"));
    m_psEncodeStatus->setText(QStringLiteral("未测试"));
    updateSummary();
}

void FrameRatePage::setCameraTestRunning(bool running) {
    const bool wasRunning = m_cameraTestRunning;
    m_cameraTestRunning = running;
    m_cameraTestButton->setText(running ? QStringLiteral("停止测试")
                                        : QStringLiteral("开始相机输入测试"));
    m_hostPsTestButton->setEnabled(!running);
    m_psPipelineTestButton->setEnabled(!running);
    m_psHostTestButton->setEnabled(!running);
    if (running) {
        m_cameraTestHasSample = false;
        m_cameraTestHasResult = false;
        m_transcodeCapacityHasResult = false;
        m_cameraReference->setText(QStringLiteral("测试中（X5完整双鱼眼）"));
        m_cameraStatus->setText(QStringLiteral("测试中"));
        m_transcodeReference->setText(QStringLiteral("测试中（等待相机样本）"));
        m_transcodeStatus->setText(QStringLiteral("待输入测试完成"));
        m_summary->setText(QStringLiteral("相机输入统计运行10秒：比较解码帧序号、相机时间戳和主机墙"
                                          "钟；转码行显示实时双JPEG编码完成率。"));
    } else if (wasRunning) {
        if (!m_cameraTestHasSample) {
            m_cameraReference->setText(QStringLiteral("测试已停止（无有效样本）"));
            m_cameraStatus->setText(QStringLiteral("无有效样本"));
            m_summary->setText(QStringLiteral("相机输入测试已停止；本次没有收到有效数据。"));
        } else {
            m_summary->setText(m_summary->text() + QStringLiteral(" 测试已停止。"));
        }
    }
}

void FrameRatePage::setTranscodeCapacityCompleted(double elapsedSeconds, double fps,
                                                  quint64 attempts, quint64 completed,
                                                  quint64 failures, int workers) {
    m_transcodeCapacityHasResult = fps > 0.0;
    m_transcodeReference->setText(
        m_transcodeCapacityHasResult
            ? QStringLiteral("%1 FPS（%2线程独立实测）").arg(fps, 0, 'f', 2).arg(workers)
            : QStringLiteral("测试失败"));
    m_transcodeStatus->setText(QStringLiteral("%1秒 · 尝试%2/完成%3/失败%4")
                                   .arg(elapsedSeconds, 0, 'f', 2)
                                   .arg(attempts)
                                   .arg(completed)
                                   .arg(failures));
    m_summary->setText(
        m_transcodeCapacityHasResult
            ? QStringLiteral(
                  "Host 422→420 三线程独立容量 %1 FPS；中列结果固定，左列继续显示实时完成率。")
                  .arg(fps, 0, 'f', 2)
            : QStringLiteral("Host 422→420 独立容量测试失败。"));
}

void FrameRatePage::setCameraInputSample(quint64 frames, double mediaSeconds, double timestampFps,
                                         double deliveryFps) {
    m_cameraTestHasSample = true;
    m_cameraTestHasResult = deliveryFps > 0.0;
    const QString measured =
        deliveryFps > 0.0 ? QStringLiteral("%1 FPS（输入窗口实测）").arg(deliveryFps, 0, 'f', 2)
                          : QStringLiteral("预热中");
    m_cameraReference->setText(measured);
    m_cameraStatus->setText(deliveryFps > 0.0 ? QStringLiteral("%1 · %2帧/%3秒")
                                                    .arg(attainmentText(deliveryFps, timestampFps))
                                                    .arg(frames)
                                                    .arg(mediaSeconds, 0, 'f', 2)
                                              : QStringLiteral("预热中"));
    m_summary->setText(
        deliveryFps > 0.0
            ? QStringLiteral("相机→Host 实际接收 %1 FPS；时间戳测得 %2 FPS。瓶颈判断使用实际值。")
                  .arg(deliveryFps, 0, 'f', 2)
                  .arg(timestampFps, 0, 'f', 2)
            : QStringLiteral("相机输入测试正在预热；首个样本仅建立墙钟基准，不参与瓶颈判断。"));
}

void FrameRatePage::setCameraTestError(const QString &reason) {
    setCameraTestRunning(false);
    m_cameraTestHasResult = true;
    m_cameraReference->setText(QStringLiteral("测试失败"));
    m_cameraStatus->setText(QStringLiteral("失败"));
    m_summary->setText(
        QStringLiteral("相机输入测试未完成：%1；左列保留最后一个在线窗口。").arg(reason));
    if (!m_transcodeCapacityHasResult) {
        m_transcodeReference->setText(QStringLiteral("测试失败"));
        m_transcodeStatus->setText(QStringLiteral("未取得容量结果"));
    }
}

void FrameRatePage::setHostPsTestRunning(bool running) {
    m_hostPsTestRunning = running;
    m_hostPsTestButton->setText(running ? QStringLiteral("停止Host→PS测试")
                                        : QStringLiteral("开始Host→PS测试"));
    m_cameraTestButton->setEnabled(!running);
    m_psPipelineTestButton->setEnabled(!running);
    m_psHostTestButton->setEnabled(!running);
    if (running) {
        m_hostPsTestHasResult = false;
        m_hostPsTestFailed = false;
        m_uploadReference->setText(QStringLiteral("测试中（等待统计窗口）"));
        m_psReceiveReference->setText(QStringLiteral("测试中（等待PS心跳）"));
        m_uploadStatus->setText(QStringLiteral("以PS心跳对齐起止边界"));
        m_psReceiveStatus->setText(QStringLiteral("以CRC/JPEG校验完成计数"));
        m_summary->setText(QStringLiteral("Host→PS联合实测正在等待PS起始统计；视频流保持运行。"));
    }
}

void FrameRatePage::setHostPsTestProgress(int elapsedSeconds, int totalSeconds) {
    if (!m_hostPsTestRunning)
        return;
    m_summary->setText(QStringLiteral("Host→PS联合实测：%1/%2秒；结束时等待下一次PS心跳统一结算。")
                           .arg(elapsedSeconds)
                           .arg(totalSeconds));
}

void FrameRatePage::setHostPsTestCompleted(
    double elapsedSeconds, double hostFps, double hostMBps, double psFps, double psMBps,
    quint64 hostFrames, quint64 psFrames, quint64 missingFrames, quint32 incompleteDrop,
    quint32 crcDrop, quint32 staleDrop, quint32 noSlotDrop, quint64 hostAttempts,
    quint64 hostTimeouts, quint64 hostWriteFailures, quint64 hostReplacements,
    double hostAverageUploadMs, quint64 hostMaxUploadMs, quint64 hostWriteStalls) {
    m_hostPsTestRunning = false;
    m_hostPsTestHasResult = true;
    m_hostPsTestFailed = false;
    m_hostPsTestButton->setText(QStringLiteral("开始Host→PS测试"));
    m_cameraTestButton->setEnabled(true);
    m_psPipelineTestButton->setEnabled(true);
    m_psHostTestButton->setEnabled(true);
    m_uploadReference->setText(QStringLiteral("%1 FPS · %2 MB/s（独立实测）")
                                   .arg(hostFps, 0, 'f', 2)
                                   .arg(hostMBps, 0, 'f', 2));
    m_uploadStatus->setText(
        QStringLiteral("%1 MB/s · 尝试%2/完整%3 · 阻塞%4/失败%5/超时%6/覆盖%7 · 平均%8/最大%9 ms")
            .arg(hostMBps, 0, 'f', 2)
            .arg(hostAttempts)
            .arg(hostFrames)
            .arg(hostWriteStalls)
            .arg(hostWriteFailures)
            .arg(hostTimeouts)
            .arg(hostReplacements)
            .arg(hostAverageUploadMs, 0, 'f', 1)
            .arg(hostMaxUploadMs));
    m_psReceiveReference->setText(QStringLiteral("%1 FPS · %2 MB/s（独立实测）")
                                      .arg(psFps, 0, 'f', 2)
                                      .arg(psMBps, 0, 'f', 2));
    m_psReceiveStatus->setText(QStringLiteral("%1 MB/s · %2帧 · 缺失%3 · 残%4/CRC%5/过期%6/无槽%7")
                                   .arg(psMBps, 0, 'f', 2)
                                   .arg(psFrames)
                                   .arg(missingFrames)
                                   .arg(incompleteDrop)
                                   .arg(crcDrop)
                                   .arg(staleDrop)
                                   .arg(noSlotDrop));
    if (hostTimeouts || hostWriteFailures || incompleteDrop || crcDrop || staleDrop || noSlotDrop ||
        missingFrames) {
        m_summary->setText(
            QStringLiteral("Host上传/"
                           "UDP分片→PS重组边界存在损失：窗口%1秒，尝试%2、Host完整%3、PS完整%"
                           "4；阻塞%5、失败%6、超时%7、覆盖%8、PS残帧%9、边界缺失%10。")
                .arg(elapsedSeconds, 0, 'f', 2)
                .arg(hostAttempts)
                .arg(hostFrames)
                .arg(psFrames)
                .arg(hostWriteStalls)
                .arg(hostWriteFailures)
                .arg(hostTimeouts)
                .arg(hostReplacements)
                .arg(incompleteDrop)
                .arg(missingFrames));
    } else {
        m_summary->setText(QStringLiteral("Host→PS完整帧边界正常：窗口%1秒，Host %2 FPS，PS %3 "
                                          "FPS，完整帧缺失0；临时阻塞%4次均已重试恢复。")
                               .arg(elapsedSeconds, 0, 'f', 2)
                               .arg(hostFps, 0, 'f', 2)
                               .arg(psFps, 0, 'f', 2)
                               .arg(hostWriteStalls));
    }
}

void FrameRatePage::setHostPsTestError(const QString &reason) {
    m_hostPsTestRunning = false;
    m_hostPsTestFailed = true;
    m_hostPsTestButton->setText(QStringLiteral("开始Host→PS测试"));
    m_cameraTestButton->setEnabled(true);
    m_psPipelineTestButton->setEnabled(true);
    m_psHostTestButton->setEnabled(true);
    m_uploadReference->setText(QStringLiteral("测试失败"));
    m_psReceiveReference->setText(QStringLiteral("测试失败"));
    m_uploadStatus->setText(QStringLiteral("测试失败：%1").arg(reason));
    m_psReceiveStatus->setText(QStringLiteral("测试失败：%1").arg(reason));
    m_summary->setText(QStringLiteral("Host→PS联合实测未完成：%1").arg(reason));
}

void FrameRatePage::updateSummary() {
    if (m_cameraTestRunning || m_hostPsTestRunning || m_psPipelineTestRunning ||
        m_psHostTestRunning)
        return;
    if (m_hostPsTestFailed || m_psPipelineTestFailed)
        return;
    if (m_cameraFps < 0) {
        m_summary->setText(QStringLiteral("等待相机采集；未连接的链路不参与瓶颈判断。"));
        return;
    }
    QString name = QStringLiteral("相机输入");
    double minimum = m_cameraFps;
    const struct {
        double fps;
        const char *name;
    } stages[] = {{m_transcodeFps, "Host 转码"},   {m_uploadFps, "Host→PS 上传"},
                  {m_psReceiveFps, "PS 接收重组"}, {m_psDecodeFps, "PS JPEG 解码"},
                  {m_psCorrectFps, "PL 校正"},     {m_psEncodeFps, "PS JPEG 编码"},
                  {m_returnFps, "PS→Host 回传"},   {m_decodeFps, "Host 回图解码"}};
    for (const auto &stage : stages) {
        if (stage.fps >= 0 && stage.fps < minimum) {
            minimum = stage.fps;
            name = QString::fromUtf8(stage.name);
        }
    }
    if (minimum == 0) {
        m_summary->setText(
            QStringLiteral("当前有环节没有完整帧（0 "
                           "FPS）：%1。先确认采集、板卡连接和视频运行状态，再判断性能瓶颈。")
                .arg(name));
        return;
    }
    m_summary->setText(
        QStringLiteral(
            "当前已测链路最低完整帧率：%1 · %2 FPS。它是定位线索，不等于该环节独立容量上限。")
            .arg(name)
            .arg(minimum, 0, 'f', 1));
}
