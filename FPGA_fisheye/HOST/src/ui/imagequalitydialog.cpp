#include "path_dialog.h"
#include "imagequalitydialog.h"
#include "fpga/network/boardvideoservice.h"
#include <QCloseEvent>
#include <QDateTime>
#include <QDir>
#include <QFileDialog>
#include <QGraphicsPixmapItem>
#include <QGraphicsScene>
#include <QGraphicsView>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QScrollBar>
#include <QTimer>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <exception>
namespace {
class ImageView : public QGraphicsView {
  public:
    std::function<void(double)> zoom;
    using QGraphicsView::QGraphicsView;
    void wheelEvent(QWheelEvent *e) override {
        if (zoom)
            zoom(e->angleDelta().y() > 0 ? 1.2 : 1 / 1.2);
        e->accept();
    }
};
} // namespace
ImageQualityDialog::ImageQualityDialog(BoardVideoService *service, QWidget *parent)
    : QDialog(parent), board(service) {
    setObjectName("imageQualityDialog");
    setWindowTitle("双鱼眼 · 图像质量");
    setWindowModality(Qt::WindowModal);
    resize(1250, 760);
    setMinimumSize(950, 600);
    auto *layout = new QVBoxLayout(this);
    auto *title = new QLabel("图像质量 · 同帧全景对比");
    title->setObjectName("title");
    layout->addWidget(title);
    auto *controls = new QHBoxLayout;
    enter = new QPushButton("进入独立测试");
    enter->setObjectName("qualityEnter");
    capture = new QPushButton("采集并分析");
    capture->setObjectName("qualityCapture");
    capture->setToolTip("取当前相机或离线录像的完整双鱼眼帧，发送至PS/PL，与同帧软件参考比较");
    leave = new QPushButton("结束测试并恢复视频");
    leave->setObjectName("qualityLeave");
    save = new QPushButton("保存结果");
    save->setObjectName("qualitySave");
    open = new QPushButton("打开结果");
    open->setObjectName("qualityOpen");
    raw = new QPushButton("原始鱼眼图");
    raw->setObjectName("qualityRaw");
    raw->setToolTip("查看本次结果的同帧双鱼眼输入；保存结果时一并导出 fisheye.png");
    for (auto b : {enter, capture, leave, save, open, raw})
        controls->addWidget(b);
    controls->addStretch();
    layout->addLayout(controls);
    auto *metrics = new QHBoxLayout;
    psnr = new QLabel("PSNR\n—");
    psnr->setObjectName("qualityPsnr");
    ssim = new QLabel("SSIM\n—");
    ssim->setObjectName("qualitySsim");
    for (auto label : {psnr, ssim}) {
        label->setStyleSheet(
            "padding:12px;background:#252a31;border-radius:6px;font-size:20px;color:#f1f5fa;");
        metrics->addWidget(label);
    }
    layout->addLayout(metrics);
    auto *images = new QHBoxLayout;
    const QString names[] = {"软件参考全景", "FPGA 实际回图"};
    for (int i = 0; i < 2; ++i) {
        auto *column = new QVBoxLayout;
        column->addWidget(new QLabel(names[i]));
        auto *view = new ImageView;
        views[i] = view;
        view->setScene(new QGraphicsScene(view));
        view->setBackgroundBrush(QColor("#101214"));
        view->setDragMode(QGraphicsView::ScrollHandDrag);
        view->setRenderHint(QPainter::SmoothPixmapTransform);
        view->zoom = [this](double factor) {
            if (!active || !result)
                return;
            double scale = views[0]->transform().m11() * factor;
            if (scale < .05 || scale > 8)
                return;
            for (auto view : views)
                view->scale(factor, factor);
        };
        column->addWidget(view, 1);
        images->addLayout(column, 1);
    }
    for (auto get : {&QGraphicsView::horizontalScrollBar, &QGraphicsView::verticalScrollBar}) {
        auto *a = (views[0]->*get)(), *b = (views[1]->*get)();
        connect(a, &QScrollBar::valueChanged, b, &QScrollBar::setValue);
        connect(b, &QScrollBar::valueChanged, a, &QScrollBar::setValue);
    }
    layout->addLayout(images, 1);
    fit = new QPushButton("适应窗口");
    connect(fit, &QPushButton::clicked, this, &ImageQualityDialog::fitImages);
    layout->addWidget(fit);
    info = new QLabel;
    info->setWordWrap(true);
    info->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(info);
    status = new QLabel;
    status->hide();
    status->setWordWrap(true);
    status->setObjectName("qualityStatus");
    layout->addWidget(status);
    auto *back = new QPushButton("返回工作台");
    layout->addWidget(back);
    connect(back, &QPushButton::clicked, this, &QWidget::close);
    connect(enter, &QPushButton::clicked, this, &ImageQualityDialog::enterMode);
    connect(leave, &QPushButton::clicked, this, &ImageQualityDialog::leaveMode);
    connect(raw, &QPushButton::clicked, this, &ImageQualityDialog::showInputImage);
    connect(board, &BoardVideoService::qualitySuspended, this, [this] {
        if (!active || leaving)
            return;
        ready = true;
        status->setText("独立模式已就绪");
        if (result)
            showResult();
        updateControls();
    });
    connect(board, &BoardVideoService::qualityResumed, this, [this] {
        if (!active)
            return;
        active = ready = leaving = false;
        emit modeChanged(false);
        status->setText("独立测试已结束");
        updateControls();
        if (closeAfter) {
            closeAfter = false;
            QDialog::reject();
        }
    });
    connect(capture, &QPushButton::clicked, this, [this] {
        if (!active || !ready || job || leaving)
            return;
        if (!board->qualityCanCapture()) {
            status->setText("请先退出独立模式并连接板卡");
            return;
        }
        QImage raw;
        qint64 timestamp = 0;
        if (!takeRaw || !takeRaw(raw, timestamp)) {
            status->setText("当前输入没有完整双鱼眼帧，请先连接相机或加载离线视频。");
            return;
        }
        auto *next = new QualityJob(this);
        next->raw = raw;
        next->cameraTimestamp = timestamp;
        next->endpoints = board->qualityEndpoints();
        result.reset();
        showResult();
        startJob(next);
    });
    connect(open, &QPushButton::clicked, this, [this] {
        if (!active || !ready || job || leaving)
            return;
        auto dir = df::selectProjectDirectory(this, "选择含 quality.json 的结果目录");
        if (dir.isEmpty())
            return;
        openResultDirectory(dir);
    });
    connect(save, &QPushButton::clicked, this, [this] {
        if (!active || !result || job || leaving)
            return;
        auto dir = df::selectProjectDirectory(this, "选择保存父目录");
        if (dir.isEmpty())
            return;
        auto *next = new QualityJob(this);
        next->operation = QualityJob::Save;
        next->existing = result;
        QString base =
            dir + "/quality_" + QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss_zzz");
        next->directory = base;
        for (int n = 1; QDir(next->directory).exists(); ++n)
            next->directory = base + QString("_%1").arg(n);
        startJob(next);
    });
    updateControls();
}
ImageQualityDialog::~ImageQualityDialog() {
    shutdown();
}
void ImageQualityDialog::shutdown() {
    ++task;
    if (job) {
        job->requestInterruption();
        job->wait();
        delete job;
        job = nullptr;
    }
}
void ImageQualityDialog::cancelSample(const QString &reason) {
    if (!active || !job || job->operation != QualityJob::Capture)
        return;
    ++task;
    job->requestInterruption();
    result.reset();
    showResult();
    status->setText(reason);
}
void ImageQualityDialog::enterMode() {
    status->show();
    if (active)
        return;
    QString error = entryError ? entryError() : QString();
    if (!error.isEmpty()) {
        status->setText(error);
        return;
    }
    active = true;
    ready = false;
    leaving = false;
    emit modeChanged(true);
    if (!board->enterQualityMode()) {
        active = false;
        emit modeChanged(false);
        status->setText("无法进入独立模式");
    } else
        status->setText("正在停止正常传输并等待工作线程完成…");
    updateControls();
}
void ImageQualityDialog::leaveMode() {
    if (!active || leaving)
        return;
    leaving = true;
    ready = false;
    ++task;
    for (auto view : views)
        view->scene()->clear();
    status->setText("正在取消任务并恢复视频…");
    if (job)
        job->requestInterruption();
    else
        finishLeaving();
    updateControls();
}
void ImageQualityDialog::finishLeaving() {
    board->leaveQualityMode();
}
void ImageQualityDialog::closeEvent(QCloseEvent *event) {
    if (active) {
        closeAfter = true;
        leaveMode();
        event->ignore();
    } else
        event->accept();
}
void ImageQualityDialog::reject() {
    if (active) {
        closeAfter = true;
        leaveMode();
    } else
        QDialog::reject();
}
void ImageQualityDialog::startJob(QualityJob *next) {
    job = next;
    const quint64 id = ++task;
    status->setText("正在处理…");
    connect(next, &QualityJob::progress, this, [this, id](const QString &message) {
        if (id == task)
            status->setText(message);
    });
    connect(next, &QualityJob::failed, this, [this, id](const QString &reason) {
        if (id == task)
            status->setText("不可计算 / 操作失败：" + reason);
    });
    connect(next, &QualityJob::saved, this, [this, id](const QString &dir) {
        if (id == task)
            status->setText("已保存：" + dir);
    });
    connect(next, &QualityJob::resultReady, this,
            [this, id](std::shared_ptr<const Quality::Result> value) {
                if (id != task || !active || leaving)
                    return;
                result = std::move(value);
                showResult();
                status->setText("分析完成");
            });
    connect(next, &QThread::finished, this, [this, next] {
        if (job == next)
            job = nullptr;
        next->deleteLater();
        if (leaving)
            finishLeaving();
        updateControls();
    });
    updateControls();
    next->start();
}
void ImageQualityDialog::updateControls() {
    enter->setEnabled(!active);
    capture->setEnabled(active && ready && !job && !leaving);
    capture->setText(result ? "重新采集并分析" : "采集并分析");
    open->setEnabled(active && ready && !job && !leaving);
    save->setEnabled(active && ready && !job && !leaving && bool(result));
    raw->setEnabled(active && ready && !job && !leaving && bool(result));
    fit->setEnabled(active && ready && bool(result) && !leaving);
    leave->setEnabled(active && !leaving);
}
void ImageQualityDialog::showResult() {
    for (auto view : views)
        view->scene()->clear();
    if (!result || !active || leaving) {
        psnr->setText("PSNR\n—");
        ssim->setText("SSIM\n—");
        info->clear();
        return;
    }
    psnr->setText(
        "PSNR\n" +
        (result->scores.infinite ? QString("∞") : QString::number(result->scores.psnr, 'f', 3)) +
        " dB");
    ssim->setText("SSIM\n" + QString::number(result->scores.ssim, 'f', 6));
    views[0]->scene()->addPixmap(QPixmap::fromImage(result->reference));
    views[1]->scene()->addPixmap(QPixmap::fromImage(result->fpga));
    for (auto view : views)
        view->scene()->setSceneRect(0, 0, 2656, 1328);
    info->setText(QString("帧 %1 / %2 / %3 · 2656×1328 · 有效像素 %4 · SSIM 窗口 %5 · 覆盖率 %6%")
                      .arg(result->session)
                      .arg(result->epoch)
                      .arg(result->frame)
                      .arg(result->scores.pixels)
                      .arg(result->scores.windows)
                      .arg(result->scores.coverage * 100, 0, 'f', 2));
    fitImages();
    updateControls();
}
void ImageQualityDialog::fitImages() {
    if (!active || !result || leaving)
        return;
    // One scale for both views so pixel coordinates remain comparable.
    double scale = qMin(
        double(qMin(views[0]->viewport()->width(), views[1]->viewport()->width()) - 4) / 2656,
        double(qMin(views[0]->viewport()->height(), views[1]->viewport()->height()) - 4) / 1328);
    for (auto view : views) {
        view->setTransform(QTransform::fromScale(scale, scale));
        view->centerOn(1328, 664);
    }
}

void ImageQualityDialog::showInputImage() {
    if (!active || !ready || job || leaving || !result)
        return;
    QImage image;
    try {
        image = Quality::inputImage(result->front, result->rear);
    } catch (const std::exception &error) {
        QMessageBox::warning(this, "无法打开原始鱼眼图", QString::fromUtf8(error.what()));
        return;
    }
    QDialog preview(this);
    preview.setObjectName("qualityRawDialog");
    preview.setWindowTitle("原始鱼眼图 · 同帧输入");
    preview.resize(1150, 720);
    auto *layout = new QVBoxLayout(&preview);
    auto *identity = new QLabel(QString("帧 %1 / %2 / %3 · 2656×1328 · 左：前向镜头  右：后向镜头")
                                   .arg(result->session).arg(result->epoch).arg(result->frame));
    identity->setObjectName("qualityRawIdentity");
    identity->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(identity);
    auto *view = new ImageView;
    view->setObjectName("qualityRawView");
    view->setScene(new QGraphicsScene(view));
    view->scene()->addPixmap(QPixmap::fromImage(image));
    view->setSceneRect(image.rect());
    view->setBackgroundBrush(QColor("#101214"));
    view->setDragMode(QGraphicsView::ScrollHandDrag);
    view->setRenderHint(QPainter::SmoothPixmapTransform);
    view->zoom = [view](double factor) {
        const double scale = view->transform().m11() * factor;
        if (scale >= .05 && scale <= 8)
            view->scale(factor, factor);
    };
    layout->addWidget(view, 1);
    auto *description = new QLabel("本图由本次发送给板卡的前、后向 JPEG 解码合成，与软件参考图及 FPGA 回图属于同一帧。\n"
                                   "点击主窗口“保存结果”，可同时获得 fisheye.png、reference.png 和 fpga.jpg。");
    description->setWordWrap(true);
    layout->addWidget(description);
    auto *buttons = new QHBoxLayout;
    auto *fitInput = new QPushButton("适应窗口");
    auto fitView = [view] { view->fitInView(view->sceneRect(), Qt::KeepAspectRatio); };
    connect(fitInput, &QPushButton::clicked, &preview, fitView);
    auto *close = new QPushButton("关闭");
    connect(close, &QPushButton::clicked, &preview, &QDialog::accept);
    buttons->addWidget(fitInput);
    buttons->addStretch();
    buttons->addWidget(close);
    layout->addLayout(buttons);
    QTimer::singleShot(0, &preview, fitView);
    preview.exec();
}

void ImageQualityDialog::openResultDirectory(const QString &directory) {
    if (!active || !ready || job || leaving || directory.isEmpty())
        return;
    auto *next = new QualityJob(this);
    next->operation = QualityJob::Open;
    next->directory = directory;
    result.reset();
    showResult();
    startJob(next);
}
