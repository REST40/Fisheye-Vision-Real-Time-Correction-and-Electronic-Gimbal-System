#include "main_window.h"
#include "fpga/network/boardvideoservice.h"
#include "frameratepage.h"
#include "imagequalitydialog.h"
#include "latencyanalysispage.h"
#include "media/video_probe.h"
#include "path_dialog.h"
#include "settings_dialog.h"
#include "simulation/simulation_service.h"
#include "view_panel.h"
#include "x5/capture_service.h"
#include <QApplication>
#include <QCloseEvent>
#include <QDateTime>
#include <QDesktopServices>
#include <QDialog>
#include <QDirIterator>
#include <QFile>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QPlainTextEdit>
#include <QScrollArea>
#include <QShortcut>
#include <QSpinBox>
#include <QThread>
#include <QUrl>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>
namespace df {
static QLabel *text(const QString &value, const QString &kind = {}) {
    auto *l = new QLabel(value);
    l->setObjectName(kind);
    return l;
}
static QPushButton *button(const QString &value, const QString &name = {}) {
    auto *b = new QPushButton(value);
    b->setObjectName(name);
    b->setCursor(Qt::PointingHandCursor);
    return b;
}
MainWindow::MainWindow(QString path, bool autoplay, QWidget *parent)
    : QMainWindow(parent), root(std::move(path)) {
    setWindowTitle("双鱼眼 · FPGA开发上位机（C++ Qt）");
    setWindowIcon(QIcon(root + "/resources/icon.svg"));
    resize(1560, 940);
    setMinimumSize(1180, 760);
    settings = new SettingsDialog(root, this);
    simulation = new SimulationService(root + "/config/calibration.json", this);
    capture = new CaptureService(root, this);
    board = new BoardVideoService(this);
    buildUi();
    setupAnalysis();
    connect(settings->findChild<QPushButton *>("connectBoardButton"), &QPushButton::clicked, this,
            [this] {
                if (simActive && !offlineBoard) {
                    message("请先退出仿真再连接PS", true);
                    return;
                }
                auto view = settings->view();
                board->requestPose(view.yaw, view.pitch, view.hfov);
                board->connectToBoard();
            });
    connect(settings->findChild<QPushButton *>("disconnectBoardButton"), &QPushButton::clicked,
            this, [this] { board->disconnectFromBoard(); });
    connect(board, &BoardVideoService::statusChanged, this, [this](const QString &text) {
        settings->findChild<QLabel *>("boardStatus")->setText(text);
    });
    connect(board, &BoardVideoService::connectedChanged, this, [this](bool connected) {
        settings->findChild<QPushButton *>("connectBoardButton")->setEnabled(!connected);
        settings->hfov->setEnabled(true);
        if (!connected) {
            boardReturnFps = 0;
            if (offlineBoard) {
                for (int i : {1, 2, 3}) {
                    panels[i]->viewport->clear();
                    panels[i]->timestamp->setText("—");
                    panels[i]->sizeLabel->setText("等待PS/PL回图");
                }
            }
            panels[2]->viewport->clear();
            panels[3]->viewport->clear();
            panels[2]->sizeLabel->setText("等待PS全景回图");
            panels[3]->sizeLabel->setText("等待PS全景回图");
        }
        if (connected && offlineBoard && displayed && !displayed->fullRaw.isNull())
            board->submit(displayed->fullRaw, qRound64(displayed->pts * 1000));
        message(connected ? "PS已连接，准备固定360°全景LUT" : "PS已断开");
    });
    connect(board, &BoardVideoService::poseResolved, this,
            [this](bool accepted, double, double, double, quint32, int reason) {
                if (!accepted)
                    message(QString("PS拒绝云台角度，原因码 %1").arg(reason), true);
            });
    connect(settings->findChild<QPushButton *>("cameraParametersButton"), &QPushButton::clicked,
            this, [this] { showParameters(); });
    clock.start();
    connect(settings, &SettingsDialog::loadRequested, this, &MainWindow::loadSimulation);
    connect(settings->offlineQuality, qOverload<int>(&QComboBox::currentIndexChanged), this,
            [this] {
                if (offlineBoard)
                    board->setInputJpegQuality(settings->offlineQuality->currentData().toInt(),
                                               settings->offlineQuality->currentData().toInt() == 50);
            });
    connect(settings->sourceMode, qOverload<int>(&QComboBox::currentIndexChanged), this, [this] {
        if (simActive)
            loadSource(offlineBoard);
    });
    connect(settings, &SettingsDialog::viewChanged, this, &MainWindow::viewChanged);
    connect(settings, &SettingsDialog::alignmentChanged, this, [this] {
        if (simActive)
            simulation->setAlignment(settings->alignment());
    });
    connect(settings, &SettingsDialog::directionRequested, this, &MainWindow::direction);
    connect(simulation, &SimulationService::failed, this,
            [this](quint64 gen, const QString &error) {
                if (gen != generation || !simActive)
                    return;
                exitSimulation();
                settings->simulationStatus->setText("仿真失败：" + error);
                message(error, true);
            });
    connect(simulation, &SimulationService::exported, this,
            [this](quint64 gen, const QString &directory) {
                if (gen != generation || !simActive)
                    return;
                exportPending = false;
                lastSaved = directory;
                message("高清图像和NPZ组合LUT已导出。");
                refresh();
            });
    connect(simulation, &SimulationService::exportFailed, this,
            [this](quint64 gen, const QString &error) {
                if (gen != generation || !simActive)
                    return;
                exportPending = false;
                message("导出失败：" + error, true);
                refresh();
            });
    connect(simulation, &QThread::finished, this, [this] {
        if (closing)
            QTimer::singleShot(0, this, &QWidget::close);
    });
    connect(capture, &CaptureService::event, this, &MainWindow::captureEvent);
    connect(capture, &CaptureService::error, this, [this](const QString &error) {
        operationMessage = error;
        message(error, true);
    });
    connect(capture, &CaptureService::ended, this, [this](int code) {
        phase = "idle";
        pending.clear();
        snapshotPending = lensRequested = false;
        refresh();
        if (code == 0)
            message("采集已停止，文件保存完成。");
        if (closing)
            QTimer::singleShot(0, this, &QWidget::close);
    });
    cameraPoll.setTimerType(Qt::PreciseTimer);
    cameraPoll.setInterval(4);
    connect(&cameraPoll, &QTimer::timeout, this, &MainWindow::pollCameraRaw);
    cameraPoll.start();
    timer.setInterval(16);
    connect(&timer, &QTimer::timeout, this, &MainWindow::tick);
    timer.start();
    refresh();
    if (autoplay)
        QTimer::singleShot(150, this, &MainWindow::loadSimulation);
}
MainWindow::~MainWindow() {
    if (qualityDialog) {
        delete qualityDialog;
        qualityDialog = nullptr;
    }
    delete board;
    board = nullptr;
    simulation->stop();
    simulation->wait();
}
void MainWindow::setupAnalysis() {
    qualityDialog = new ImageQualityDialog(board, this);
    qualityDialog->entryError = [this]() -> QString {
        if (simActive && !offlineBoard)
            return "请先退出软件仿真";
        if (offlineBoard && (!displayed || displayed->fullRaw.isNull()))
            return "离线视频尚未读取到完整双鱼眼帧，请等待加载完成后重试";
        if (live["recording"].toBool() || phase == "recording" || lensRequested ||
            live["lens_busy"].toBool() || snapshotPending || !pending.isEmpty())
            return "请等待录制、截图或十秒采集完成后再进入独立测试";
        if (cameraSampleActive || qualityHostPsBusy || qualityPipelineBusy || qualityReturnBusy ||
            linkDialog->isVisible())
            return "请先结束容量测试并关闭链路分析窗口";
        return {};
    };
    qualityDialog->takeRaw = [this](QImage &image, qint64 &timestamp) {
        if (offlineBoard && displayed && !displayed->fullRaw.isNull()) {
            image = displayed->fullRaw;
            timestamp = qRound64(displayed->pts * 1000);
            return true;
        }
        return capture->running() && capture->fullRaw(image, timestamp);
    };
    connect(qualityButton, &QPushButton::clicked, this, [this] {
        qualityDialog->show();
        qualityDialog->raise();
        qualityDialog->activateWindow();
    });
    connect(qualityDialog, &ImageQualityDialog::modeChanged, this, [this](bool active) {
        qualityActive = active;
        if (active) {
            qualityOfflineWasPlaying = offlineBoard && displayed && displayed->playing;
            if (offlineBoard) {
                simulation->setPlaying(false);
                simulation->seek(displayed->pts);
            }
            cameraPoll.stop();
            timer.stop();
        } else {
            if (offlineBoard)
                simulation->setPlaying(qualityOfflineWasPlaying);
            qualityOfflineWasPlaying = false;
            frameTimes.clear();
            cameraRateClock.invalidate();
            lastCameraSequence = 0;
            cameraPoll.start();
            timer.start();
        }
        refresh();
    });
    connect(board, &BoardVideoService::hostPsRateTestRunningChanged, this,
            [this](bool v) { qualityHostPsBusy = v; });
    connect(board, &BoardVideoService::psPipelineRateTestRunningChanged, this,
            [this](bool v) { qualityPipelineBusy = v; });
    connect(board, &BoardVideoService::psHostRateTestRunningChanged, this,
            [this](bool v) { qualityReturnBusy = v; });
    connect(capture, &CaptureService::ended, this,
            [this](int) {
                if (!offlineBoard)
                    qualityDialog->cancelSample("相机断开，当前采样已取消");
            });

    linkDialog = new QDialog(this);
    linkDialog->setObjectName("linkAnalysisDialog");
    linkDialog->setWindowTitle("双鱼眼 · 链路分析");
    linkDialog->resize(1420, 850);
    linkDialog->setMinimumSize(1120, 650);
    auto *layout = new QVBoxLayout(linkDialog);
    layout->setContentsMargins(0, 0, 0, 0);
    linkPage = new FrameRatePage(linkDialog);
    layout->addWidget(linkPage);
    linkPage->resetBoard();
    latencyDialog = new QDialog(this);
    latencyDialog->setObjectName("latencyAnalysisDialog");
    latencyDialog->setWindowTitle("双鱼眼 · 延迟分析");
    latencyDialog->resize(1250, 820);
    auto *latencyLayout = new QVBoxLayout(latencyDialog);
    auto *latencyPage = new LatencyAnalysisPage(&board->latencyTracker(), latencyDialog);
    latencyLayout->addWidget(latencyPage);
    connect(latencyPage, &LatencyAnalysisPage::backRequested, latencyDialog, &QDialog::close);
    connect(panels[3]->viewport, &Viewport::imageReplaced, this,
            [this](quint64 id) { board->latencyTracker().status(id, "paint_queue_replaced"); });
    connect(panels[3]->viewport, &Viewport::painted, this,
            [this](quint64 id, qint64 rs, qint64 re, qint64 take, qint64 ps, qint64 pe) {
                auto &tracker = board->latencyTracker();
                tracker.mark(id, "render_start", rs);
                tracker.mark(id, "render_end", re);
                tracker.mark(id, "ui_take", take);
                tracker.mark(id, "paint_start", ps);
                tracker.mark(id, "paint_end", pe);
            });
    connect(linkAnalysisButton, &QPushButton::clicked, this, [this] {
        linkDialog->show();
        linkDialog->raise();
        linkDialog->activateWindow();
    });
    connect(latencyAnalysisButton, &QPushButton::clicked, this, [this] {
        latencyDialog->show();
        latencyDialog->raise();
        latencyDialog->activateWindow();
    });
    connect(linkPage, &FrameRatePage::backRequested, linkDialog, &QDialog::close);
    connect(linkDialog, &QDialog::finished, this, [this] {
        cameraInputTest(false);
        board->stopHostPsRateTest();
        board->stopPsPipelineRateTest();
        board->stopPsHostRateTest();
    });
    connect(linkPage, &FrameRatePage::cameraTestRequested, this, &MainWindow::cameraInputTest);
    connect(linkPage, &FrameRatePage::hostPsTestRequested, this, [this](bool start) {
        if (start)
            board->startHostPsRateTest();
        else
            board->stopHostPsRateTest();
    });
    connect(linkPage, &FrameRatePage::psPipelineTestRequested, this, [this](bool start) {
        if (start)
            board->startPsPipelineRateTest();
        else
            board->stopPsPipelineRateTest();
    });
    connect(linkPage, &FrameRatePage::psHostTestRequested, this, [this](bool start) {
        if (start)
            board->startPsHostRateTest();
        else
            board->stopPsHostRateTest();
    });
    connect(capture, &CaptureService::ended, this, [this](int) {
        cameraInputTest(false);
        linkPage->resetCamera();
        cameraRateClock.invalidate();
        lastCameraSequence = 0;
    });
    connect(board, &BoardVideoService::connectedChanged, this, [this](bool connected) {
        if (!connected)
            linkPage->resetBoard();
    });
    connect(board, &BoardVideoService::pipelineRatesChanged, linkPage,
            &FrameRatePage::setBoardRates);
    connect(board, &BoardVideoService::pipelineRatesChanged, this,
            [this](double, double returned, double) { boardReturnFps = returned; });
    connect(board, &BoardVideoService::psReceiveWindowChanged, linkPage,
            &FrameRatePage::setPsReceiveWindow);
    connect(board, &BoardVideoService::psPipelineWindowChanged, linkPage,
            &FrameRatePage::setPsPipelineWindow);
    connect(board, &BoardVideoService::normalReturnWindowChanged, linkPage,
            &FrameRatePage::setNormalReturnWindow);
    connect(board, &BoardVideoService::psStageWaitWindow, linkPage, &FrameRatePage::setPsStageWait);
    connect(board, &BoardVideoService::uploadWaitWindow, linkPage, &FrameRatePage::setUploadWait);
    connect(board, &BoardVideoService::psWaitWindow, linkPage, &FrameRatePage::setPsWait);
    connect(board, &BoardVideoService::psFlowWindow, linkPage, &FrameRatePage::setPsFlow);
    connect(board, &BoardVideoService::stageTimesChanged, linkPage, &FrameRatePage::setBoardStages);
    connect(board, &BoardVideoService::pairEncodingRatesChanged, linkPage,
            &FrameRatePage::setPairEncodingRate);
    connect(board, &BoardVideoService::hostPsRateTestRunningChanged, linkPage,
            &FrameRatePage::setHostPsTestRunning);
    connect(board, &BoardVideoService::hostPsRateTestProgress, linkPage,
            &FrameRatePage::setHostPsTestProgress);
    connect(board, &BoardVideoService::hostPsRateTestCompleted, linkPage,
            &FrameRatePage::setHostPsTestCompleted);
    connect(board, &BoardVideoService::hostPsRateTestError, linkPage,
            &FrameRatePage::setHostPsTestError);
    connect(board, &BoardVideoService::psPipelineRateTestRunningChanged, linkPage,
            &FrameRatePage::setPsPipelineTestRunning);
    connect(board, &BoardVideoService::psPipelineRateTestProgress, linkPage,
            &FrameRatePage::setPsPipelineTestProgress);
    connect(board, &BoardVideoService::psPipelineRateTestCompleted, linkPage,
            &FrameRatePage::setPsPipelineTestCompleted);
    connect(board, &BoardVideoService::psPipelineRateTestError, linkPage,
            &FrameRatePage::setPsPipelineTestError);
    connect(board, &BoardVideoService::psHostRateTestRunningChanged, linkPage,
            &FrameRatePage::setPsHostTestRunning);
    connect(board, &BoardVideoService::psHostRateTestProgress, linkPage,
            &FrameRatePage::setPsHostTestProgress);
    connect(board, &BoardVideoService::psHostRateTestCompleted, linkPage,
            &FrameRatePage::setPsHostTestCompleted);
    connect(board, &BoardVideoService::hostDecodeCapacityCompleted, linkPage,
            &FrameRatePage::setHostDecodeCapacityCompleted);
    connect(board, &BoardVideoService::psHostRateTestError, linkPage,
            &FrameRatePage::setPsHostTestError);
}
void MainWindow::cameraInputTest(bool running) {
    if (running) {
        if (!capture->running() || simActive) {
            linkPage->setCameraTestError("请连接X5并等待完整双鱼眼输入");
            return;
        }
        cameraSampleActive = true;
        cameraSampleFirst = cameraSampleLast = 0;
        cameraSampleClock.start();
        linkPage->setCameraTestRunning(true);
        return;
    }
    if (!cameraSampleActive)
        return;
    cameraSampleActive = false;
    if (cameraSampleFirst && cameraSampleLast > cameraSampleFirst &&
        cameraSampleLastTimestamp > cameraSampleFirstTimestamp) {
        const quint64 frames = cameraSampleLast - cameraSampleFirst;
        const double mediaSeconds =
            double(cameraSampleLastTimestamp - cameraSampleFirstTimestamp) / 1000.;
        const double wallSeconds = std::max(.001, cameraSampleClock.elapsed() / 1000.);
        linkPage->setCameraInputSample(frames, mediaSeconds, frames / mediaSeconds,
                                       frames / wallSeconds);
        linkPage->setCameraTestRunning(false);
    } else
        linkPage->setCameraTestError("没有足够的完整原始帧样本");
}
void MainWindow::buildUi() {
    auto *central = new QWidget;
    setCentralWidget(central);
    auto *outer = new QVBoxLayout(central);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);
    auto *header = new QFrame;
    header->setObjectName("header");
    auto *top = new QHBoxLayout(header);
    top->setContentsMargins(20, 12, 20, 12);
    top->setSpacing(8);
    top->addWidget(text("X5", "brand"));
    auto *brand = new QVBoxLayout;
    brand->addWidget(text("STUDIO / 工作台", "section"));
    brand->addWidget(text("C++ Qt · FPGA 开发", "muted"));
    top->addLayout(brand);
    top->addSpacing(14);
    connectButton = button("连接相机", "yellow");
    connectButton->setProperty("role", "connect");
    connect(connectButton, &QPushButton::clicked, this, &MainWindow::connectCamera);
    top->addWidget(connectButton);
    auto *offlineButton = button("选择离线视频");
    offlineButton->setObjectName("offlineVideoButton");
    connect(offlineButton, &QPushButton::clicked, this, [this] {
        if (qualityActive || capture->running() || phase != "idle") {
            message("请先结束当前任务并断开X5，再加载离线视频。");
            return;
        }
        auto path = selectProjectDirectory(this, "选择已处理的前后同步视频目录",
                                           settings->sourceDirectory->text());
        if (path.isEmpty())
            return;
        for (const auto &name : {"front.png", "rear.png"}) {
            if (!QFile::exists(path + "/" + name)) {
                message("目录缺少 " + QString(name) + "，请选择转换后的同步视频目录。");
                return;
            }
        }
        if (pairedVideoFiles(path)[0].isEmpty()) {
            message("目录需要一对同后缀的前后视频，例如 front_64s.mp4 和 rear_64s.mp4。");
            return;
        }
        settings->sourceDirectory->setText(path);
        settings->sourceMode->setCurrentIndex(0);
        loadOfflineVideo();
    });
    top->addWidget(offlineButton);
    recordButton = button("● 开始双路录制");
    recordButton->setObjectName("recordButton");
    connect(recordButton, &QPushButton::clicked, this, &MainWindow::record);
    top->addWidget(recordButton);
    snapshotButton = button("◎ 成对截图");
    snapshotButton->setObjectName("snapshotButton");
    connect(snapshotButton, &QPushButton::clicked, this, &MainWindow::snapshot);
    top->addWidget(snapshotButton);
    datasetButton = button("前后采集 · 10秒", "yellow");
    datasetButton->setProperty("role", "dataset");
    connect(datasetButton, &QPushButton::clicked, this, &MainWindow::dataset);
    top->addWidget(datasetButton);
    top->addStretch();
    linkAnalysisButton = button("链路分析", "linkAnalysisButton");
    latencyAnalysisButton = button("延迟分析", "latencyAnalysisButton");
    top->addWidget(linkAnalysisButton);
    top->addWidget(latencyAnalysisButton);
    qualityButton = button("图像质量", "imageQualityButton");
    top->addWidget(qualityButton);
    auto *params = button("参数设置");
    connect(params, &QPushButton::clicked, this, [this] { settings->openPage(); });
    top->addWidget(params);
    outer->addWidget(header);
    auto *body = new QHBoxLayout;
    body->setContentsMargins(12, 14, 14, 14);
    body->setSpacing(14);
    auto *nav = new QVBoxLayout;
    auto *liveButton = button("◉\n工作台");
    liveButton->setFixedWidth(78);
    nav->addWidget(liveButton);
    auto *libraryButton = button("▤\n录制库");
    libraryButton->setFixedWidth(78);
    nav->addWidget(libraryButton);
    auto *fpgaButton = button("FPGA\n接口");
    fpgaButton->setFixedWidth(78);
    connect(fpgaButton, &QPushButton::clicked, this, [this] { settings->openPage(4); });
    nav->addWidget(fpgaButton);
    presentationButton = button("演示\n模式");
    presentationButton->setObjectName("presentationButton");
    presentationButton->setCheckable(true);
    presentationButton->setFixedWidth(78);
    connect(presentationButton, &QPushButton::toggled, this, [this](bool enabled) {
        presentationTimer.stop();
        presentationAuto->setChecked(false);
        presentationControls->setVisible(enabled);
        pages->setCurrentIndex(0);
        setPresentationStep(enabled ? 0 : -1);
    });
    nav->addWidget(presentationButton);
    nav->addStretch();
    body->addLayout(nav);
    pages = new QStackedWidget;
    connect(pages, &QStackedWidget::currentChanged, this, [this](int page) {
        if (page != 0 && presentationAuto) {
            presentationTimer.stop();
            presentationAuto->setChecked(false);
        }
    });
    body->addWidget(pages, 1);
    outer->addLayout(body, 1);
    connect(liveButton, &QPushButton::clicked, this, [this] { pages->setCurrentIndex(0); });
    connect(libraryButton, &QPushButton::clicked, this, [this] {
        pages->setCurrentIndex(1);
        loadLibrary();
    });
    auto *page = new QWidget;
    auto *center = new QVBoxLayout(page);
    center->setContentsMargins(0, 0, 0, 0);
    center->setSpacing(12);
    auto *stats = new QFrame;
    statisticsBar = stats;
    stats->setObjectName("metrics");
    auto *grid = new QGridLayout(stats);
    grid->setContentsMargins(18, 8, 18, 8);
    grid->setVerticalSpacing(2);
    QStringList titles{"采集帧率", "已录制同步帧", "录制时间"};
    for (int i = 0; i < 3; ++i) {
        captions[i] = text(titles[i], "muted");
        metrics[i] = text(i == 1 ? "0" : "—", "metric");
        grid->addWidget(captions[i], 0, i);
        grid->addWidget(metrics[i], 1, i);
        grid->setColumnStretch(i, 1);
    }
    center->addWidget(stats);
    presentationControls = new QFrame;
    presentationControls->setObjectName("presentationControls");
    auto *presentationBar = new QHBoxLayout(presentationControls);
    presentationBar->setContentsMargins(0, 0, 0, 0);
    presentationTitle = text("", "section");
    presentationTitle->setObjectName("presentationTitle");
    presentationBar->addWidget(presentationTitle, 1);
    presentationPrevious = button("← 上一步", "presentationPrevious");
    presentationNext = button("下一步 →", "presentationNext");
    auto *overview = button("四屏总览", "presentationOverview");
    presentationAuto = button("自动播放", "presentationAuto");
    presentationAuto->setCheckable(true);
    presentationSeconds = new QSpinBox;
    presentationSeconds->setObjectName("presentationSeconds");
    presentationSeconds->setRange(2, 60);
    presentationSeconds->setValue(8);
    presentationSeconds->setSuffix(" 秒 / 步");
    for (auto *control : {presentationPrevious, presentationNext, overview, presentationAuto})
        presentationBar->addWidget(control);
    presentationBar->addWidget(presentationSeconds);
    auto *exitPresentation = button("退出演示", "exitPresentation");
    presentationBar->addWidget(exitPresentation);
    connect(presentationPrevious, &QPushButton::clicked, this, [this] { advancePresentation(-1); });
    connect(presentationNext, &QPushButton::clicked, this, [this] { advancePresentation(1); });
    connect(overview, &QPushButton::clicked, this, [this] {
        presentationTimer.stop();
        presentationAuto->setChecked(false);
        setPresentationStep(4);
    });
    connect(exitPresentation, &QPushButton::clicked, this, [this] {
        presentationButton->setChecked(false);
    });
    connect(presentationAuto, &QPushButton::toggled, this, [this](bool enabled) {
        presentationAuto->setText(enabled ? "停止自动" : "自动播放");
        if (enabled) {
            if (presentationStep == 4)
                setPresentationStep(0);
            presentationTimer.start(presentationSeconds->value() * 1000);
        } else
            presentationTimer.stop();
    });
    connect(presentationSeconds, &QSpinBox::valueChanged, this, [this](int seconds) {
        if (presentationTimer.isActive())
            presentationTimer.start(seconds * 1000);
    });
    connect(&presentationTimer, &QTimer::timeout, this, [this] {
        setPresentationStep(presentationStep + 1);
    });
    auto shortcut = [this](const QKeySequence &key, auto action) {
        auto *keys = new QShortcut(key, this);
        connect(keys, &QShortcut::activated, this, [this, action] {
            if (presentationButton->isChecked() && pages->currentIndex() == 0 &&
                isActiveWindow() && !QApplication::activeModalWidget())
                action();
        });
    };
    shortcut(QKeySequence(Qt::Key_Left), [this] { advancePresentation(-1); });
    shortcut(QKeySequence(Qt::Key_Right), [this] { advancePresentation(1); });
    shortcut(QKeySequence(Qt::Key_Escape), [this] { presentationButton->setChecked(false); });
    presentationControls->hide();
    center->addWidget(presentationControls);
    auto *views = viewGrid = new QGridLayout;
    views->setSpacing(12);
    panels = {new ViewPanel("校正前", "原始双鱼眼"), new ViewPanel("校正后", "360°全景"),
              new ViewPanel("球体显示", "外部全景球 · 与云台联动"),
              new ViewPanel("云台显示", "虚拟云台 · 透视视角")};
    for (int i = 0; i < 4; ++i)
        views->addWidget(panels[i], i / 2, i % 2);
    for (int i = 0; i < 2; ++i) {
        views->setRowStretch(i, 1);
        views->setColumnStretch(i, 1);
    }
    center->addLayout(views, 1);
    panels[2]->viewport->setSphere(true);
    for (int i : {2, 3}) {
        auto *viewport = panels[i]->viewport;
        viewport->interactive = true;
        viewport->setCursor(Qt::OpenHandCursor);
        connect(viewport, &Viewport::dragged, this, &MainWindow::drag);
        connect(viewport, &Viewport::zoomed, this, [this](int delta) {
            {
                auto view = settings->view();
                view.hfov = std::clamp(view.hfov - std::round(delta / 120.) * 5, 20., 140.);
                settings->setView(view);
                viewChanged();
            }
        });
        connect(viewport, &Viewport::resetRequested, this, [this] {
            settings->setView({});
            viewChanged();
        });
    }
    auto *transport = new QHBoxLayout;
    pauseButton = button("暂停", "pauseButton");
    connect(pauseButton, &QPushButton::clicked, this, [this] {
        if (displayed && displayed->video) {
            bool playing = pauseButton->text() == "播放";
            simulation->setPlaying(playing);
            pauseButton->setText(playing ? "暂停" : "播放");
        }
    });
    transport->addWidget(pauseButton);
    exitButton = button("退出仿真");
    connect(exitButton, &QPushButton::clicked, this, &MainWindow::exitSimulation);
    transport->addWidget(exitButton);
    seek = new QSlider(Qt::Horizontal);
    seek->setObjectName("simulationSeek");
    seek->setRange(0, 9999);
    connect(seek, &QSlider::sliderReleased, this, [this] {
        if (displayed && displayed->duration)
            simulation->seek(seek->value() / 9999. * displayed->duration);
    });
    transport->addWidget(seek, 1);
    exportButton = button("导出高清图像与 LUT", "exportButton");
    connect(exportButton, &QPushButton::clicked, this, [this] {
        if (displayed && !exportPending) {
            exportPending = true;
            simulation->exportFrame(root + "/output/simulation", *displayed);
            message("正在导出当前显示帧与视角…");
            refresh();
        }
    });
    transport->addWidget(exportButton);
    auto *simulationPage = qobject_cast<QScrollArea *>(settings->tabs->widget(0));
    auto *simulationLayout = qobject_cast<QVBoxLayout *>(simulationPage->widget()->layout());
    simulationLayout->insertLayout(simulationLayout->count() - 1, transport);
    pages->addWidget(page);
    auto *libraryPage = new QWidget;
    auto *list = new QVBoxLayout(libraryPage);
    list->addWidget(text("录制库", "title"));
    library = new QListWidget;
    list->addWidget(library, 1);
    auto *actions = new QHBoxLayout;
    for (auto value : QStringList{"刷新", "打开原图", "打开全景", "打开目录", "查看采集参数",
                                  "核验两路时间戳"}) {
        auto *action = button(value);
        connect(action, &QPushButton::clicked, this, [this, value] {
            if (value == "刷新")
                loadLibrary();
            else if (value == "核验两路时间戳")
                verifySelected();
            else if (value == "查看采集参数") {
                if (library->currentItem())
                    showParameters(library->currentItem()->data(Qt::UserRole).toString());
            } else
                openSelected(value);
        });
        actions->addWidget(action);
    }
    list->addLayout(actions);
    libraryStatus = text("可查看双路录像、成对截图和前后10秒采集。", "muted");
    libraryStatus->setWordWrap(true);
    list->addWidget(libraryStatus);
    pages->addWidget(libraryPage);
}
void MainWindow::loadOfflineVideo() {
    if (qualityActive || capture->running() || phase != "idle")
        return;
    loadSource(true);
}
void MainWindow::loadSimulation() {
    loadSource(false);
}
void MainWindow::loadSource(bool boardInput) {
    if (qualityActive)
        return;
    if (capture->running() || phase != "idle") {
        message("请先断开X5，再加载软件仿真。");
        return;
    }
    if (offlineBoard && !boardInput)
        board->disconnectFromBoard();
    offlineBoard = boardInput;
    boardReturnFps = 0;
    board->setInputJpegQuality(offlineBoard ? settings->offlineQuality->currentData().toInt() : 85,
                               offlineBoard && settings->offlineQuality->currentData().toInt() == 50);
    if (!simulation->isRunning())
        simulation->start();
    simActive = true;
    displayed.reset();
    frameTimes.clear();
    exportPending = false;
    generation = simulation->load(settings->sourceDirectory->text(),
                                  settings->sourceMode->currentData().toBool(), offlineBoard);
    simulation->setView(settings->view());
    simulation->setAlignment(settings->alignment());
    settings->simulationStatus->setText("正在核对前后时间戳并生成映射…");
    QStringList names{"显示帧率", "当前视频帧", "播放位置"};
    if (offlineBoard)
        names[0] = "输入 / 板卡回图 FPS";
    for (int i = 0; i < 3; ++i)
        captions[i]->setText(names[i]);
    if (offlineBoard) {
        for (int i : {1, 2, 3}) {
            panels[i]->viewport->clear();
            panels[i]->sizeLabel->setText("等待PS/PL回图");
            panels[i]->timestamp->setText("—");
        }
        panels[1]->caption->setText("PS/PL校正拼接全景 · 等待网线回传");
        board->requestPose(settings->view().yaw, settings->view().pitch, settings->view().hfov);
        if (!board->isConnected() && !board->isConnecting())
            board->connectToBoard();
        message("离线录像 → 网线上传PS/PL；校正结果仅显示板卡回图。");
    } else {
        message("正在加载C++软件仿真…");
    }
    refresh();
}
void MainWindow::exitSimulation() {
    simulation->unload();
    if (offlineBoard)
        board->disconnectFromBoard();
    offlineBoard = false;
    board->setInputJpegQuality(85);
    simActive = false;
    displayed.reset();
    frameTimes.clear();
    exportPending = false;
    for (auto *panel : panels) {
        panel->viewport->clear();
        panel->sizeLabel->setText("等待画面");
        panel->timestamp->setText("—");
    }
    QStringList names{"采集帧率", "已录制同步帧", "录制时间"};
    for (int i = 0; i < 3; ++i)
        captions[i]->setText(names[i]);
    metrics[0]->setText("—");
    metrics[1]->setText("0");
    metrics[2]->setText("00:00");
    panels[3]->caption->setText("虚拟云台 · 透视视角");
    message("仿真已退出，可连接X5或重新加载视频。");
    refresh();
}
void MainWindow::viewChanged() {
    if (simActive && !offlineBoard)
        simulation->setView(settings->view());
    else {
        auto view = settings->view();
        board->requestPose(view.yaw, view.pitch, view.hfov);
    }
}
void MainWindow::drag(double dx, double dy) {
    auto v = settings->view();
    double sensitivity = settings->sensitivity->value();
    v.yaw = fmod(v.yaw - dx * sensitivity + 180, 360);
    if (v.yaw < 0)
        v.yaw += 360;
    v.yaw -= 180;
    v.pitch = std::clamp(v.pitch + dy * sensitivity, -90., 90.);
    settings->setView(v);
    viewChanged();
}
void MainWindow::direction(int index) {
    if (index == 4) {
        settings->setView({});
        viewChanged();
        return;
    }
    double step = settings->step->value(), sensitivity = settings->sensitivity->value();
    drag(-(index % 3 - 1) * step / sensitivity, (1 - index / 3) * step / sensitivity);
}
void MainWindow::render(const FramePacket &frame) {
    if (!simActive || frame.generation != generation)
        return;
    bool first = !displayed;
    bool changed = first || frame.frame != displayed->frame;
    displayed = frame;
    if (first && !offlineBoard)
        message("C++软件仿真 · 前后按180°假设拼接；拖动球体或云台，滚轮调整视场。");
    if (offlineBoard) {
        panels[0]->viewport->setImage(frame.raw);
        panels[0]->sizeLabel->setText("前后各1328 × 1328 · 录像输入");
        panels[0]->caption->setText("前镜头（左） · 后镜头（右） → PS/PL");
        panels[0]->timestamp->setText(QString::number(frame.pts, 'f', 3) + " s");
        if (changed && !frame.fullRaw.isNull())
            board->submit(frame.fullRaw, qRound64(frame.pts * 1000));
    } else {
        std::array<QImage, 4> images{frame.raw, frame.panorama, frame.sphere, frame.gimbal};
        for (int i = 0; i < 4; ++i)
            panels[i]->viewport->setImage(images[i]);
        panels[0]->sizeLabel->setText("前后各1328 × 1328");
        panels[1]->sizeLabel->setText("1920 × 960 · 仿真");
        panels[2]->sizeLabel->setText("软件仿真 · 球体");
        panels[3]->sizeLabel->setText("640 × 360 · 软件仿真");
        for (int i : {0, 1})
            panels[i]->timestamp->setText(QString::number(frame.pts, 'f', 3) + " s");
        panels[0]->caption->setText("前镜头（左） · 后镜头（右）");
        panels[1]->caption->setText("软件校正与拼接全景");
        panels[2]->caption->setText("电脑软件校正拼接 · 球面展示");
        panels[2]->timestamp->setText(QString("偏航 %1°").arg(frame.view.yaw, 0, 'f', 1));
        panels[3]->caption->setText(QString("偏航%1° · 俯仰%2° · 视场%3°")
                                        .arg(frame.view.yaw, 0, 'f', 1)
                                        .arg(frame.view.pitch, 0, 'f', 1)
                                        .arg(frame.view.hfov, 0, 'f', 0));
        panels[3]->timestamp->setText("拖动 / 滚轮");
    }
    if (frame.playing && changed) {
        frameTimes.push_back(clock.elapsed() / 1000.);
        while (frameTimes.size() > 90)
            frameTimes.pop_front();
    }
    double rate = frame.playing && frameTimes.size() > 1
                      ? (frameTimes.size() - 1) / (frameTimes.back() - frameTimes.front())
                      : 0;
    metrics[0]->setText(offlineBoard
                           ? QString("%1 / %2").arg(rate, 0, 'f', 1).arg(boardReturnFps, 0, 'f', 1)
                           : QString::number(rate, 'f', 1));
    metrics[1]->setText(QString("%1 / %2").arg(frame.frame + 1).arg(frame.count));
    metrics[2]->setText(QString::number(frame.pts, 'f', 2) + " s");
    if (!seek->isSliderDown())
        seek->setValue(frame.duration ? qRound(frame.pts / frame.duration * 9999) : 0);
    pauseButton->setText(frame.playing ? "暂停" : "播放");
    if (offlineBoard)
        settings->simulationStatus->setText(
            QString("录像输入 %1 fps · 网线上传PS/PL · %2\n%3\n暂停与跳转控制输入录像；校正画面来自板卡回传。")
                .arg(frame.sourceFps, 0, 'f', 2)
                .arg(board->isConnected() ? "PS已连接" : "等待PS连接")
                .arg(settings->offlineQuality->currentText()));
    else
        settings->simulationStatus->setText(QString("%1\n渲染 %2 ms · 有效覆盖 %3%（不代表标定准确度）")
                                                .arg(frame.video
                                                         ? QString("视频源 %1 fps · 显示 %2 fps")
                                                               .arg(frame.sourceFps, 0, 'f', 2)
                                                               .arg(rate, 0, 'f', 1)
                                                         : "照片仿真 · 可拖动视角")
                                                .arg(frame.renderMs, 0, 'f', 1)
                                                .arg(frame.coverage * 100, 0, 'f', 1));
    refresh();
}
void MainWindow::pollCameraRaw() {
    if (qualityActive)
        return;
    QImage full;
    qint64 fullTs = 0;
    if (!simActive && capture->fullRaw(full, fullTs)) {
        board->submit(full, fullTs);
        ++cameraDelivered;
        const auto sequence = capture->fullRawFrameId();
        if (!cameraRateClock.isValid()) {
            cameraRateClock.start();
            lastCameraSequence = sequence;
            cameraDelivered = 0;
        } else if (cameraRateClock.elapsed() >= 1000) {
            linkPage->setX5InputRate(double(sequence - lastCameraSequence) * 1000. /
                                         cameraRateClock.elapsed(),
                                     double(cameraDelivered) * 1000. / cameraRateClock.elapsed());
            cameraDelivered = 0;
            cameraRateClock.restart();
            lastCameraSequence = sequence;
        }
        if (cameraSampleActive) {
            if (!cameraSampleFirst) {
                cameraSampleFirst = sequence;
                cameraSampleFirstTimestamp = fullTs;
                cameraSampleClock.restart();
            }
            cameraSampleLast = sequence;
            cameraSampleLastTimestamp = fullTs;
        }
    }
}
void MainWindow::tick() {
    if (qualityActive)
        return;
    // The codec worker samples the full PS panorama directly at display
    // resolution. Hidden panels keep the baseline rather than a stale
    // fullscreen allocation after leaving a presentation step.
    auto *sphereViewport = panels[2]->viewport;
    auto *perspectiveViewport = panels[3]->viewport;
    int sphereDiameter = sphereViewport->isVisible()
        ? qRound(std::min(sphereViewport->width(), sphereViewport->height()) *
                 sphereViewport->devicePixelRatioF()) : 0;
    int perspectiveWidth = perspectiveViewport->isVisible()
        ? qRound(std::min(double(perspectiveViewport->width()),
                          perspectiveViewport->height() * 16. / 9.) *
                 perspectiveViewport->devicePixelRatioF()) : 0;
    board->setDisplaySizes(sphereDiameter, perspectiveWidth);
    if (auto packet = simulation->poll())
        render(*packet);
    QImage raw, pano;
    qint64 rawTs, ts;
    if (capture->preview(raw, pano, rawTs, ts)) {
        panels[0]->viewport->setImage(raw);
        panels[1]->viewport->setImage(pano);
        panels[0]->timestamp->setText(QString::number(rawTs) + " ms");
        panels[1]->timestamp->setText(QString::number(ts) + " ms");
    }
    if (cameraSampleActive && cameraSampleClock.elapsed() >= 10000)
        cameraInputTest(false);
    if (!simActive || offlineBoard) {
        auto panorama = board->takePanorama();
        if (offlineBoard && !panorama.isNull()) {
            panels[1]->viewport->setImage(panorama);
            panels[1]->sizeLabel->setText("2656 × 1328 · PS/PL回图");
            panels[1]->caption->setText("PS/PL校正拼接全景 · 网线回传");
        }
        auto sphere = board->takeSphere();
        if (!sphere.isNull()) {
            panels[2]->viewport->setImage(sphere);
            panels[2]->sizeLabel->setText(QString("%1 × %2 · PS全景 → 球体")
                                            .arg(sphere.width()).arg(sphere.height()));
        }
        auto result = board->takeDisplay();
        if (!result.image.isNull()) {
            panels[3]->viewport->setTrackedImage(result.image, result.sampleId,
                                                 result.renderStartedUs, result.renderedAtUs,
                                                 LatencyTracker::nowUs());
            panels[3]->sizeLabel->setText(QString("%1 × %2 · PS全景 → 当前视角")
                                            .arg(result.image.width()).arg(result.image.height()));
            panels[3]->timestamp->setText(
                QString("PS帧 %1 · epoch %2").arg(result.frame).arg(result.epoch));
        }
    }
    if (!pending.isEmpty() && operationClock.elapsed() > 6000) {
        pending.clear();
        message("操作尚未完成，请查看相机状态。", true);
        refresh();
    }
    if (snapshotPending && snapshotClock.elapsed() > 10000) {
        snapshotPending = false;
        message("照片保存尚未完成，请查看日志。", true);
        refresh();
    }
    if (lensRequested && lensClock.elapsed() > 8000) {
        lensRequested = false;
        message("前后采集尚未启动，请查看日志。", true);
        refresh();
    }
}
void MainWindow::refresh() {
    bool running = phase != "idle", ready = phase == "ready" || phase == "recording",
         busy = phase == "stopping",
         lensBusy = lensRequested || (running && live["lens_busy"].toBool());
    bool recording = running && live["recording"].toBool();
    settings->setCaptureLocked(running);
    settings->setSimulationEnabled(simActive);
    if (offlineBoard) {
        for (auto *value : settings->rear)
            value->setEnabled(false);
        settings->blend->setEnabled(false);
    }
    if (!simActive) {
        settings->yaw->setEnabled(true);
        settings->pitch->setEnabled(true);
        settings->hfov->setEnabled(true);
        settings->step->setEnabled(true);
        settings->sensitivity->setEnabled(true);
        for (auto *button : settings->directions)
            button->setEnabled(true);
        settings->resetButton->setEnabled(true);
    }
    if (!simActive || offlineBoard) {
        panels[2]->caption->setText("PS/PL校正拼接全景 · 球面展示");
        panels[3]->caption->setText("PS/PL全景 · 任意方向透视视角");
    }
    connectButton->setEnabled(!simActive && !busy && !lensBusy);
    connectButton->setText(busy ? "正在保存…" : running ? "断开相机" : "连接相机");
    recordButton->setEnabled(ready && !lensBusy && pending.isEmpty() &&
                             (live["gop_ready"].toBool() || recording));
    recordButton->setText(recording ? "■ 停止并保存" : "● 开始双路录制");
    snapshotButton->setEnabled(ready && !snapshotPending && !live["snapshot_busy"].toBool() &&
                               !lensBusy);
    datasetButton->setEnabled(phase == "ready" && !lensBusy && pending.isEmpty() &&
                              !snapshotPending && !recording);
    pauseButton->setEnabled(simActive && displayed && displayed->video);
    seek->setEnabled(pauseButton->isEnabled());
    exitButton->setEnabled(simActive);
    exitButton->setText(offlineBoard ? "退出离线输入" : "退出仿真");
    exportButton->setEnabled(simActive && !offlineBoard && displayed && !exportPending);
    exportButton->setText(exportPending ? "正在导出…" : "导出高清图像与 LUT");
    for (int i : {2, 3})
        panels[i]->viewport->setEnabled(!qualityActive);
    if (qualityActive) {
        connectButton->setEnabled(false);
        recordButton->setEnabled(false);
        snapshotButton->setEnabled(false);
        datasetButton->setEnabled(false);
    }
}
void MainWindow::advancePresentation(int delta) {
    if (presentationStep < 0)
        return;
    presentationAuto->setChecked(false);
    setPresentationStep(std::clamp(presentationStep + delta, 0, 4));
}

void MainWindow::setPresentationStep(int step) {
    presentationStep = std::clamp(step, -1, 4);
    const bool overview = presentationStep < 0 || presentationStep == 4;
    constexpr int order[] = {0, 1, 2, 3};
    const QStringList titles{"校正前 · 原始双鱼眼", "校正后 · PS/PL全景",
                             "球体显示", "云台显示", "四屏总览"};
    setUpdatesEnabled(false);
    statisticsBar->setVisible(presentationStep < 0);
    for (auto *panel : panels)
        viewGrid->removeWidget(panel);
    for (int i = 0; i < 2; ++i) {
        viewGrid->setRowStretch(i, overview || i == 0 ? 1 : 0);
        viewGrid->setColumnStretch(i, overview || i == 0 ? 1 : 0);
        viewGrid->setRowMinimumHeight(i, 0);
        viewGrid->setColumnMinimumWidth(i, 0);
    }
    for (int i = 0; i < 4; ++i) {
        const bool visible = overview || order[presentationStep] == i;
        panels[i]->setVisible(visible);
        if (visible)
            viewGrid->addWidget(panels[i], overview ? i / 2 : 0, overview ? i % 2 : 0);
    }
    if (presentationStep >= 0)
        presentationTitle->setText(QString("%1 / 5  ·  %2").arg(presentationStep + 1)
                                       .arg(titles[presentationStep]));
    presentationPrevious->setEnabled(presentationStep > 0);
    presentationNext->setEnabled(presentationStep >= 0 && presentationStep < 4);
    if (overview) {
        presentationTimer.stop();
        presentationAuto->setChecked(false);
    }
    viewGrid->invalidate();
    setUpdatesEnabled(true);
    update();
}

void MainWindow::connectCamera() {
    if (qualityActive)
        return;
    if (simActive)
        return;
    if (capture->running()) {
        phase = "stopping";
        capture->shutdown();
        message("正在停止采集并完成文件保存…");
        refresh();
        return;
    }
    try {
        writeJson(root + "/config/capture_settings.json", settings->captureSettings());
    } catch (const std::exception &e) {
        message(e.what(), true);
        return;
    }
    phase = "starting";
    live = {};
    pending.clear();
    snapshotPending = lensRequested = false;
    sessionPath.clear();
    lastSnapshot.clear();
    lastClip.clear();
    lastDataset.clear();
    operationMessage.clear();
    for (auto *panel : panels)
        panel->viewport->clear();
    capture->start(settings->captureSettings());
    message("正在连接X5并等待同步预览…");
    refresh();
}
void MainWindow::send(const QString &command) {
    capture->send({{"cmd", command}});
}
void MainWindow::record() {
    if (!recordButton->isEnabled() || !pending.isEmpty())
        return;
    operationMessage.clear();
    pending = live["recording"].toBool() ? "stop" : "start";
    operationClock.restart();
    send(pending);
    message(pending == "start" ? "正在核对参数并开始双路录制…" : "正在完成文件保存…");
    refresh();
}
void MainWindow::snapshot() {
    if (!snapshotButton->isEnabled())
        return;
    operationMessage.clear();
    snapshotPending = true;
    snapshotClock.restart();
    send("snapshot");
    message("正在保存同步成对截图…");
    refresh();
}
void MainWindow::dataset() {
    if (!datasetButton->isEnabled())
        return;
    operationMessage.clear();
    lensRequested = true;
    lensClock.restart();
    capture->send({{"cmd", "lens_start"}, {"output_root", settings->datasetDirectory->text()}});
    message("正在核对参数并采集前后照片及10秒MJPEG420视频…");
    refresh();
}
void MainWindow::captureEvent(const QJsonObject &data) {
    QString event = data["event"].toString();
    if (event == "session") {
        sessionPath = data["path"].toString();
        message("相机预热中，等待同步画面…");
    } else if (event == "camera_metadata") {
        auto metadata = data["data"].toObject();
        auto exposure = metadata["exposure"].toObject();
        settings->exposureActual->setText(
            !metadata["valid"].toBool() ? "参数回读未通过，请重新连接。"
            : exposure["mode"].toString() == "manual"
                ? QString("相机回读：手动 ISO %1 · 快门 %2秒")
                      .arg(exposure["iso_setting"].toInt())
                      .arg(exposure["shutter_seconds_setting"].toDouble(), 0, 'g', 6)
                : QString("相机回读：自动测光 EV %1；无逐帧ISO回读。")
                      .arg(exposure["ev_stops"].toDouble(), 0, 'f', 1));
    } else if (event == "operation_error") {
        QString command = data["cmd"].toString();
        if (command == "start" || command == "stop")
            pending.clear();
        if (command == "snapshot")
            snapshotPending = false;
        if (command == "lens_start")
            lensRequested = false;
        operationMessage = data["message"].toString();
        message(operationMessage, true);
    } else if (event == "status") {
        live = data;
        if (phase != "stopping" && data["phase"] != "closed" && data["phase"] != "error" &&
            data["phase"] != "stopping")
            phase = data["recording"].toBool() ? "recording"
                    : (data["phase"].toString() == "preview" || data["total_pairs"].toDouble() > 0)
                        ? "ready"
                        : phase;
        if ((pending == "start" && data["recording"].toBool()) ||
            (pending == "stop" && !data["recording"].toBool()))
            pending.clear();
        if (!data["snapshot_path"].toString().isEmpty() &&
            data["snapshot_path"].toString() != lastSnapshot) {
            lastSnapshot = data["snapshot_path"].toString();
            snapshotPending = false;
            lastSaved = data["snapshot_path"].toString();
            message("成对截图已保存：" + QFileInfo(lastSaved).fileName());
        }
        if (!data["last_clip"].toString().isEmpty() && data["last_clip"].toString() != lastClip) {
            lastClip = data["last_clip"].toString();
            lastSaved = data["last_clip"].toString();
            message("双路录像已保存：" + QFileInfo(lastSaved).fileName());
        }
        if (data["lens_busy"].toBool()) {
            lensRequested = false;
            datasetButton->setText(
                QString("采集中 · %1 / 10秒").arg(data["lens_elapsed"].toDouble(), 0, 'f', 1));
        } else if (!data["lens_path"].toString().isEmpty() &&
                   data["lens_path"].toString() != lastDataset) {
            lastDataset = data["lens_path"].toString();
            lensRequested = false;
            datasetButton->setText("前后采集 · 10秒");
            lastSaved = data["lens_path"].toString();
            message("前后采集已保存：" + QFileInfo(lastSaved).fileName() +
                    " · 两张PNG＋两段10秒MP4");
            if (!data["lens_error"].toString().isEmpty())
                message(data["lens_error"].toString(), true);
        }
        metrics[0]->setText(QString::number(data["fps"].toDouble(), 'f', 1));
        metrics[1]->setText(QString::number(data["recorded_frames"].toInt()));
        int duration = data["recording"].toBool() && data["epoch_timestamp"].toDouble() > 0
                           ? std::max(0, int((data["camera_timestamp"].toDouble() -
                                              data["epoch_timestamp"].toDouble()) /
                                             1000))
                           : 0;
        metrics[2]->setText(QString("%1:%2")
                                .arg(duration / 60, 2, 10, QChar('0'))
                                .arg(duration % 60, 2, 10, QChar('0')));
        if (data["raw_width"].toInt() > 0)
            panels[0]->sizeLabel->setText(
                QString("%1 × %2").arg(data["raw_width"].toInt()).arg(data["raw_height"].toInt()));
        if (data["output_width"].toInt() > 0)
            panels[1]->sizeLabel->setText(QString("%1 × %2")
                                              .arg(data["output_width"].toInt())
                                              .arg(data["output_height"].toInt()));
        if (!data["error"].toString().isEmpty()) {
            pending.clear();
            snapshotPending = false;
            operationMessage = data["error"].toString();
        }
        if (!operationMessage.isEmpty())
            message(operationMessage, true);
        else if (data["lens_busy"].toBool())
            message(QString("前后采集 · %1帧 · %2/10秒")
                        .arg(data["lens_frames"].toInt())
                        .arg(data["lens_elapsed"].toDouble(), 0, 'f', 1));
        else if (phase == "ready" && pending.isEmpty() && !lensRequested && !snapshotPending)
            message("X5同步预览已就绪，可录制、截图或前后10秒采集。");
    } else if (event == "stopping") {
        phase = "stopping";
        message(data["message"].toString());
    } else if (event == "finished" && !data["okay"].toBool())
        message(data["status"].toObject()["problem"].toString(), true);
    refresh();
}
void MainWindow::message(const QString &value, bool error) {
    linkAnalysisButton->setToolTip(value);
    if (libraryStatus)
        libraryStatus->setText(value);
    linkAnalysisButton->setStyleSheet(error ? "color:#ff8b91;" : QString());
}
void MainWindow::loadLibrary() {
    library->clear();
    QStringList roots{settings->recordDirectory->text(), settings->datasetDirectory->text()};
    for (const auto &base : roots) {
        QDirIterator it(base, QStringList{"summary.json", "pair.json", "manifest.json"},
                        QDir::Files, QDirIterator::Subdirectories);
        while (it.hasNext()) {
            QString path = it.next();
            auto *item = new QListWidgetItem(QFileInfo(path).dir().absolutePath());
            item->setData(Qt::UserRole, QFileInfo(path).dir().absolutePath());
            library->addItem(item);
        }
    }
}
void MainWindow::openSelected(const QString &name) {
    auto *item = library->currentItem();
    if (!item)
        return;
    QString directory = item->data(Qt::UserRole).toString(), target = directory;
    if (name != "打开目录") {
        const auto pair = pairedVideoFiles(directory);
        QStringList candidates = name == "打开原图" ? QStringList{"fisheye.png", "fisheye.mp4",
                                                                  pair[0], "front.png"}
                                                    : QStringList{"corrected.png", "corrected.mp4"};
        bool found = false;
        for (const auto &file : candidates)
            if (!file.isEmpty() && QFile::exists(directory + "/" + file)) {
                target = directory + "/" + file;
                found = true;
                break;
            }
        if (!found) {
            message("此采集目录没有对应全景文件。");
            return;
        }
    }
    QDesktopServices::openUrl(QUrl::fromLocalFile(target));
}
void MainWindow::showLogs() {
    auto *dialog = new QDialog(this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowTitle("本次采集日志与参数");
    dialog->resize(850, 640);
    auto *layout = new QVBoxLayout(dialog);
    auto *text = new QPlainTextEdit;
    text->setReadOnly(true);
    QString value = capture->stderrText;
    for (const auto &name :
         {"sdk.log", "recorder.log", "process_status.json", "parameters/camera_metadata.json"}) {
        QFile f(sessionPath + "/" + name);
        if (f.open(QIODevice::ReadOnly)) {
            f.seek(std::max(qint64(0), f.size() - 16000));
            value += '\n' + QString(name) + '\n' + QString::fromUtf8(f.readAll());
        }
    }
    text->setPlainText(value.isEmpty() ? "连接相机后生成采集日志。" : value);
    layout->addWidget(text);
    dialog->show();
}
void MainWindow::showParameters(const QString &requested) {
    QString directory = requested.isEmpty() ? sessionPath + "/parameters" : requested;
    QJsonObject values;
    for (const auto &name :
         {"camera_metadata.json", "recording_parameters.json", "capture_settings.json",
          "calibration.json", "manifest.json", "simulation_parameters.json"}) {
        if (QFile::exists(directory + "/" + name)) {
            try {
                values[QString::fromLatin1(name)] = readJson(directory + "/" + name);
            } catch (const std::exception &e) {
                message(QString::fromUtf8(e.what()), true);
                return;
            }
        }
    }
    if (values.isEmpty()) {
        message("当前目录尚无采集参数；连接相机或选择已保存数据后查看。");
        return;
    }
    auto *dialog = new QDialog(this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowTitle("相机回读、标定与采集参数");
    dialog->resize(850, 640);
    auto *layout = new QVBoxLayout(dialog);
    auto *viewer = new QPlainTextEdit;
    viewer->setReadOnly(true);
    viewer->setPlainText(QString::fromUtf8(QJsonDocument(values).toJson()));
    layout->addWidget(viewer);
    dialog->show();
}
void MainWindow::verifySelected() {
    auto *item = library->currentItem();
    if (!item)
        return;
    if (live["recording"].toBool() && phase == "recording") {
        message("请先停止录制，再核验视频。");
        return;
    }
    if (verifier && verifier->isRunning())
        return;
    QString directory = item->data(Qt::UserRole).toString();
    message("正在核验两路视频帧数和显示时间戳…");
    verifier = QThread::create([this, directory] {
        try {
            auto report = verifyVideoPair(directory);
            QString result =
                QString("核验通过：%1对同步视频帧。").arg(report["paired_frames"].toInt());
            QMetaObject::invokeMethod(
                this, [this, result] { message(result); }, Qt::QueuedConnection);
        } catch (const std::exception &e) {
            QString error = QString::fromUtf8(e.what());
            QMetaObject::invokeMethod(
                this, [this, error] { message("核验失败：" + error, true); }, Qt::QueuedConnection);
        }
    });
    verifier->setParent(this);
    connect(verifier, &QThread::finished, this, [this] {
        verifier->deleteLater();
        verifier = nullptr;
        if (closing)
            QTimer::singleShot(0, this, &QWidget::close);
    });
    verifier->start();
}
void MainWindow::saveScreenshot(const QString &path) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    grab().save(path);
}
void MainWindow::closeEvent(QCloseEvent *event) {
    if (capture->running()) {
        closing = true;
        phase = "stopping";
        capture->shutdown();
        message("正在完成采集保存，请稍候…");
        event->ignore();
        return;
    }
    if (simulation->isRunning()) {
        closing = true;
        simulation->stop();
        event->ignore();
        return;
    }
    if (verifier && verifier->isRunning()) {
        closing = true;
        event->ignore();
        return;
    }
    settings->close();
    linkDialog->close();
    latencyDialog->close();
    event->accept();
}
} // namespace df
