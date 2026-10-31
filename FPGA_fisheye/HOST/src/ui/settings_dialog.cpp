#include "settings_dialog.h"
#include "path_dialog.h"
#include "media/paired_source.h"
#include <QFileDialog>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QMessageBox>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QVBoxLayout>
namespace df {
static QLabel *caption(const QString &text, const QString &kind = "muted") {
    auto *label = new QLabel(text);
    label->setObjectName(kind);
    label->setWordWrap(true);
    return label;
}
static QPushButton *action(const QString &text) {
    return new QPushButton(text);
}
SettingsDialog::SettingsDialog(const QString &path, QWidget *parent) : QDialog(parent), root(path) {
    setWindowTitle("参数设置 · 双鱼眼 Qt 上位机");
    resize(800, 760);
    setMinimumSize(680, 560);
    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(18, 18, 18, 18);
    outer->addWidget(caption("参数设置", "title"));
    outer->addWidget(caption("窗口可保持打开，主画面拖动后角度同步更新。"));
    tabs = new QTabWidget;
    outer->addWidget(tabs, 1);
    auto page = [&](const QString &title) {
        auto *panel = new QWidget;
        auto *layout = new QVBoxLayout(panel);
        layout->setContentsMargins(18, 18, 18, 18);
        layout->setSpacing(12);
        auto *scroll = new QScrollArea;
        scroll->setWidgetResizable(true);
        scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        scroll->setWidget(panel);
        tabs->addTab(scroll, title);
        return layout;
    };
    auto *form = page("软件仿真");
    form->addWidget(caption("前后同步输入", "section"));
    sourceDirectory = new QLineEdit(defaultDataset(root));
    sourceDirectory->setReadOnly(true);
    form->addWidget(sourceDirectory);
    sourceBrowse = action("选择仿真数据目录");
    connect(sourceBrowse, &QPushButton::clicked, this, [this] {
        auto path = selectProjectDirectory(this, "选择前后照片与视频目录",
                                                      sourceDirectory->text());
        if (!path.isEmpty())
            sourceDirectory->setText(path);
    });
    form->addWidget(sourceBrowse);
    sourceMode = new QComboBox;
    sourceMode->addItem("前后视频 · 循环播放", true);
    sourceMode->addItem("前后照片 · 静态仿真", false);
    form->addWidget(sourceMode);
    form->addWidget(caption("离线视频上传画质"));
    offlineQuality = new QComboBox;
    offlineQuality->setObjectName("offlineJpegQuality");
    offlineQuality->addItem("低细节预览 · 平滑与JPEG压缩", 50);
    offlineQuality->addItem("均衡画质与帧率", 65);
    offlineQuality->addItem("原画质 · JPEG质量85", 85);
    offlineQuality->setCurrentIndex(2);
    offlineQuality->setToolTip("低细节预览会用13×13高斯平滑（σ=3）减少细节，并使用JPEG质量50；均衡为65，原画质为85且不平滑。前后镜头仍各为1328×1328，PS/PL处理不变。");
    form->addWidget(offlineQuality);
    loadButton = action("加载软件仿真");
    loadButton->setObjectName("yellow");
    connect(loadButton, &QPushButton::clicked, this, &SettingsDialog::loadRequested);
    form->addWidget(loadButton);
    simulationStatus = caption("无需连接相机，加载前后原图或同步MJPEG视频。");
    form->addWidget(simulationStatus);
    form->addWidget(caption("暂停、时间轴和高清图像/LUT导出位于本页。"));
    form->addStretch();
    form = page("云台控制");
    form->addWidget(caption("云台视角", "section"));
    yaw = new QSlider(Qt::Horizontal);
    yaw->setRange(-1800, 1800);
    pitch = new QSlider(Qt::Horizontal);
    pitch->setRange(-900, 900);
    hfov = new QSlider(Qt::Horizontal);
    hfov->setRange(20, 140);
    hfov->setValue(90);
    int idx = 0;
    for (auto *slider : {yaw, pitch, hfov}) {
        auto *row = new QHBoxLayout;
        auto *text = caption("");
        text->setFixedWidth(124);
        angleLabels[idx++] = text;
        row->addWidget(text);
        row->addWidget(slider, 1);
        form->addLayout(row);
        connect(slider, &QSlider::valueChanged, this, [this] {
            refreshAngles();
            emit viewChanged();
        });
    }
    resetButton = action("视角回中 · 90°");
    connect(resetButton, &QPushButton::clicked, this, [this] {
        setView({});
        emit viewChanged();
    });
    form->addWidget(resetButton);
    auto *tuning = new QGridLayout;
    step = new QDoubleSpinBox;
    step->setRange(.1, 45);
    step->setValue(5);
    step->setDecimals(1);
    step->setSuffix("°");
    sensitivity = new QDoubleSpinBox;
    sensitivity->setRange(.02, 2);
    sensitivity->setValue(.2);
    sensitivity->setDecimals(2);
    sensitivity->setSingleStep(.01);
    sensitivity->setSuffix(" °/像素");
    tuning->addWidget(caption("方向按钮步长"), 0, 0);
    tuning->addWidget(caption("鼠标拖动灵敏度"), 0, 1);
    tuning->addWidget(step, 1, 0);
    tuning->addWidget(sensitivity, 1, 1);
    form->addLayout(tuning);
    auto *grid = new QGridLayout;
    QStringList names{"左上", "上", "右上", "左", "回中", "右", "左下", "下", "右下"};
    for (int i = 0; i < 9; ++i) {
        directions[i] = action(names[i]);
        grid->addWidget(directions[i], i / 3, i % 3);
        connect(directions[i], &QPushButton::clicked, this,
                [this, i] { emit directionRequested(i); });
    }
    form->addWidget(caption("八方向控制", "section"));
    form->addLayout(grid);
    form->addWidget(caption("水平视场20°～140°；球体保持圆形，云台按16:9显示。"));
    form->addStretch();
    form = page("拼接对齐");
    form->addWidget(caption("后镜头相对旋转微调", "section"));
    QStringList angleNames{"偏航", "俯仰", "滚转"};
    for (int i = 0; i < 3; ++i) {
        form->addWidget(caption("后镜头" + angleNames[i]));
        rear[i] = new QDoubleSpinBox;
        rear[i]->setRange(-180, 180);
        rear[i]->setDecimals(1);
        rear[i]->setSingleStep(.1);
        rear[i]->setSuffix("°");
        connect(rear[i], qOverload<double>(&QDoubleSpinBox::valueChanged), this,
                &SettingsDialog::alignmentChanged);
        form->addWidget(rear[i]);
    }
    blend = new QSpinBox;
    blend->setRange(1, 60);
    blend->setValue(20);
    blend->setSuffix("° 融合宽度");
    connect(blend, qOverload<int>(&QSpinBox::valueChanged), this,
            &SettingsDialog::alignmentChanged);
    form->addWidget(blend);
    form->addWidget(
        caption("使用原有Mei＋5项Brown参数；前后外参按相向180°假设，未做工厂验证和近景视差补偿。"));
    form->addStretch();
    form = page("X5采集");
    form->addWidget(caption("连接相机前设置；连接后锁定曝光与编码参数。"));
    auto folder = [&](const QString &title, QLineEdit *&edit, QPushButton *&browse,
                      const QString &initial) {
        form->addWidget(caption(title));
        edit = new QLineEdit(initial);
        edit->setReadOnly(true);
        form->addWidget(edit);
        browse = action("选择目录");
        connect(browse, &QPushButton::clicked, this, [this, edit] {
            auto value = selectProjectDirectory(this, "选择保存目录", edit->text());
            if (!value.isEmpty())
                edit->setText(value);
        });
        form->addWidget(browse);
    };
    folder("前后照片和10秒视频采集目录", datasetDirectory, dataBrowse, root + "/data/captures");
    folder("双路录像保存目录", recordDirectory, recordBrowse, root + "/output/recordings");
    resolution = new QComboBox;
    resolution->addItem("2656 × 1328 · 高清", 2656);
    resolution->addItem("1920 × 960 · 轻量", 1920);
    form->addWidget(caption("实机全景输出尺寸"));
    form->addWidget(resolution);
    encoder = new QComboBox;
    encoder->addItem("自动 · 优先显卡", "auto");
    encoder->addItem("NVIDIA显卡编码", "h264_nvenc");
    encoder->addItem("CPU编码", "libx264");
    form->addWidget(caption("双路录像编码"));
    form->addWidget(encoder);
    bitrate = new QComboBox;
    bitrate->addItem("24 Mbps · 标准", 24);
    bitrate->addItem("40 Mbps · 高画质", 40);
    bitrate->setCurrentIndex(1);
    form->addWidget(caption("每路码率"));
    form->addWidget(bitrate);
    seconds = new QSpinBox;
    seconds->setRange(0, 86400);
    seconds->setSingleStep(10);
    seconds->setSpecialValueText("连续录制");
    seconds->setSuffix(" 秒");
    form->addWidget(caption("双路录制时长（前后采集按钮固定10秒）"));
    form->addWidget(seconds);
    exposure = new QComboBox;
    exposure->addItem("保留相机设置", "keep");
    exposure->addItem("自动 · 调节EV", "auto");
    exposure->addItem("手动 · ISO / 快门", "manual");
    form->addWidget(caption("曝光设置", "section"));
    form->addWidget(exposure);
    iso = new QComboBox;
    for (int value : {100, 200, 400, 800, 1600, 3200})
        iso->addItem(QString::number(value), value);
    form->addWidget(caption("ISO"));
    form->addWidget(iso);
    shutter = new QComboBox;
    for (int value : {30, 50, 60, 100, 120, 200, 240, 500, 1000, 2000, 4000, 8000})
        shutter->addItem("1/" + QString::number(value), value);
    shutter->setCurrentIndex(shutter->findData(60));
    form->addWidget(caption("快门 / 秒"));
    form->addWidget(shutter);
    ev = new QDoubleSpinBox;
    ev->setRange(-4, 4);
    ev->setDecimals(1);
    ev->setSingleStep(.5);
    ev->setSuffix(" EV");
    form->addWidget(caption("曝光补偿"));
    form->addWidget(ev);
    exposureActual = caption("连接后显示相机参数回读。自动ISO不提供逐帧实时回读。");
    form->addWidget(exposureActual);
    connect(exposure, qOverload<int>(&QComboBox::currentIndexChanged), this,
            [this] { refreshExposure(); });
    auto *help = action("连接指南");
    connect(help, &QPushButton::clicked, this, [this] {
        QMessageBox::information(
            this, "X5连接指南",
            "开启X5并选择Android "
            "USB模式，用数据线连接本电脑。\n关闭其他占用相机的程序，退出软件仿真后点击连接相机。\n"
            "预览出现后，顶部可双路录像、成对截图或前后10秒采集。");
    });
    form->addWidget(help);
    auto *parameters = action("查看相机回读与采集参数");
    parameters->setObjectName("cameraParametersButton");
    form->addWidget(parameters);
    form->addStretch();
    form = page("FPGA接口");
    form->addWidget(caption("PS 双鱼眼实时链路", "section"));
    form->addWidget(caption("上位机 192.168.10.100 → PS 192.168.10.10\n"
                            "UDP：控制5500 / 输入5501 / 回图5502\n"
                            "前后各1328×1328 Baseline JPEG 4:2:0，同帧成对发送。\n"
                            "回图2656×1328为完整360°全景；上位机展示球体及任意视角。"));
    auto *connectBoard = action("连接PS并启动实时传输");
    connectBoard->setObjectName("connectBoardButton");
    form->addWidget(connectBoard);
    auto *disconnectBoard = action("停止传输并断开PS");
    disconnectBoard->setObjectName("disconnectBoardButton");
    form->addWidget(disconnectBoard);
    auto *boardStatus = caption("PS未连接");
    boardStatus->setObjectName("boardStatus");
    boardStatus->setWordWrap(true);
    form->addWidget(boardStatus);
    form->addStretch();
    auto *bottom = new QHBoxLayout;
    bottom->addStretch();
    auto *back = action("返回工作台");
    connect(back, &QPushButton::clicked, this, &QDialog::close);
    bottom->addWidget(back);
    outer->addLayout(bottom);
    restore();
    refreshAngles();
    refreshExposure();
    setSimulationEnabled(false);
}
View SettingsDialog::view() const {
    return {yaw->value() / 10., pitch->value() / 10., double(hfov->value())};
}
Alignment SettingsDialog::alignment() const {
    return {{rear[0]->value(), rear[1]->value(), rear[2]->value()}, double(blend->value())};
}
void SettingsDialog::setView(View value) {
    QSignalBlocker a(yaw), b(pitch), c(hfov);
    yaw->setValue(qRound(value.yaw * 10));
    pitch->setValue(qRound(value.pitch * 10));
    hfov->setValue(qRound(value.hfov));
    refreshAngles();
}
void SettingsDialog::refreshAngles() {
    auto v = view();
    angleLabels[0]->setText(QString("偏航 %1°").arg(v.yaw, 0, 'f', 1));
    angleLabels[1]->setText(QString("俯仰 %1°").arg(v.pitch, 0, 'f', 1));
    angleLabels[2]->setText(QString("水平视场 %1°").arg(v.hfov, 0, 'f', 0));
}
void SettingsDialog::setSimulationEnabled(bool enabled) {
    for (auto *slider : {yaw, pitch, hfov})
        slider->setEnabled(enabled);
    for (auto *value : rear)
        value->setEnabled(enabled);
    for (auto *button : directions)
        button->setEnabled(enabled);
    step->setEnabled(enabled);
    sensitivity->setEnabled(enabled);
    blend->setEnabled(enabled);
    resetButton->setEnabled(enabled);
}
void SettingsDialog::setCaptureLocked(bool locked) {
    captureLocked = locked;
    for (auto *combo : {resolution, encoder, bitrate, exposure, iso, shutter})
        combo->setEnabled(!locked);
    seconds->setEnabled(!locked);
    ev->setEnabled(!locked);
    recordBrowse->setEnabled(!locked);
    dataBrowse->setEnabled(!locked);
    sourceBrowse->setEnabled(!locked);
    sourceMode->setEnabled(!locked);
    loadButton->setEnabled(!locked);
    refreshExposure();
}
void SettingsDialog::refreshExposure() {
    auto mode = exposure->currentData().toString();
    iso->setEnabled(!captureLocked && mode == "manual");
    shutter->setEnabled(!captureLocked && mode == "manual");
    ev->setEnabled(!captureLocked && mode == "auto");
}
QJsonObject SettingsDialog::captureSettings() const {
    int width = resolution->currentData().toInt();
    return {{"width", width},
            {"height", width / 2},
            {"encoder", encoder->currentData().toString()},
            {"bitrate", bitrate->currentData().toInt()},
            {"seconds", seconds->value()},
            {"exposure_mode", exposure->currentData().toString()},
            {"iso", iso->currentData().toInt()},
            {"shutter", shutter->currentData().toInt()},
            {"ev", ev->value()},
            {"output_root", recordDirectory->text()},
            {"dataset_output_root", datasetDirectory->text()}};
}
void SettingsDialog::restore() {
    try {
        auto saved = readJson(root + "/config/capture_settings.json");
        for (const auto &pair :
             std::initializer_list<std::pair<QComboBox *, QString>>{{resolution, "width"},
                                                                    {encoder, "encoder"},
                                                                    {bitrate, "bitrate"},
                                                                    {exposure, "exposure_mode"},
                                                                    {iso, "iso"},
                                                                    {shutter, "shutter"}}) {
            auto val = saved[pair.second];
            int index = pair.first->findData(val.isString() ? QVariant(val.toString())
                                                            : QVariant(val.toInt()));
            if (index >= 0)
                pair.first->setCurrentIndex(index);
        }
        seconds->setValue(saved["seconds"].toInt());
        ev->setValue(saved["ev"].toDouble());
        auto restorePath = [&](QLineEdit *edit, const QString &key) {
            const auto path = resolveProjectPath(saved[key].toString(), root);
            if (!path.isEmpty() && isPersistentPath(path))
                edit->setText(path);
        };
        restorePath(recordDirectory, "output_root");
        restorePath(datasetDirectory, "dataset_output_root");
    } catch (const std::exception &) {
    }
}
void SettingsDialog::openPage(int index) {
    if (index >= 0)
        tabs->setCurrentIndex(index);
    show();
    raise();
    activateWindow();
}
} // namespace df
