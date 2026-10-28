#include "core/types.h"
#include "signal_shutdown.h"
#include "ui/main_window.h"
#include <QApplication>
#include <QCommandLineParser>
#include <QDir>
#include <QFile>
#include <QFont>
#include <QMessageBox>
#include <QTimer>
#include <cerrno>
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
int main(int argc, char **argv) {
    QApplication app(argc, argv);
    app.setApplicationName("DoubleFisheyeHost");
    app.setOrganizationName("FisheyeLab");
    app.setFont(QFont("Noto Sans CJK SC", 10));
    app.setStyle("Fusion");
    QCommandLineParser parser;
    parser.setApplicationDescription("C++ Qt 双鱼眼X5实时采集与PS双路JPEG传输");
    parser.addHelpOption();
    parser.addOption({"camera", "实机工作台模式（默认），不自动连接相机"});
    parser.addOption({"simulation", "显式启用本地数据仿真"});
    parser.addOption({"screenshot", "保存窗口截图后退出", "file"});
    parser.process(app);
    QString root = df::projectRoot();
    QFile style(root + "/resources/theme.qss");
    if (style.open(QIODevice::ReadOnly))
        app.setStyleSheet(QString::fromUtf8(style.readAll()));
    QDir().mkpath(root + "/.runtime");
    // Containers have different hostnames/PID namespaces. Use a kernel lock
    // on the shared inode; it is released even after a crash or container kill.
    struct InstanceLock {
        int fd;
        ~InstanceLock() { if (fd >= 0) ::close(fd); }
    } lock{::open(QFile::encodeName(root + "/.runtime/qt_host.instance.lock").constData(),
                  O_RDWR | O_CREAT | O_CLOEXEC, 0600)};
    if (lock.fd < 0 || ::flock(lock.fd, LOCK_EX | LOCK_NB) != 0) {
        const int reason = errno;
        if (lock.fd >= 0 && (reason == EWOULDBLOCK || reason == EAGAIN)) {
            QMessageBox::information(nullptr, "双鱼眼Qt上位机", "程序已经运行，请使用已打开的窗口。");
        } else {
            QMessageBox::critical(nullptr, "双鱼眼Qt上位机", "无法创建或获取启动锁，请检查.runtime目录权限。");
        }
        return 1;
    }
    df::MainWindow window(root, parser.isSet("simulation") && !parser.isSet("camera"));
    df::SignalShutdown shutdown(&window, [&window] { window.close(); });
    window.show();
    if (parser.isSet("screenshot")) {
        QTimer::singleShot(3500, &window, [&] {
            window.saveScreenshot(parser.value("screenshot"));
            window.close();
        });
    }
    return app.exec();
}
