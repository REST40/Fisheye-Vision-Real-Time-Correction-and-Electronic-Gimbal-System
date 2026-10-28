#include "types.h"
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QSaveFile>
#include <opencv2/imgproc.hpp>
#include <stdexcept>
namespace df {
QImage toImage(const cv::Mat &image) {
    cv::Mat rgb;
    cv::cvtColor(image, rgb, image.channels() == 4 ? cv::COLOR_BGRA2RGBA : cv::COLOR_BGR2RGB);
    return QImage(rgb.data, rgb.cols, rgb.rows, int(rgb.step),
                  image.channels() == 4 ? QImage::Format_RGBA8888 : QImage::Format_RGB888)
        .copy();
}
QString projectRoot() {
    const auto env = qEnvironmentVariable("DF_HOST_ROOT");
    if (!env.isEmpty())
        return QDir(env).absolutePath();
    auto dir = QDir(QCoreApplication::applicationDirPath());
    for (int i = 0; i < 5; ++i) {
        if (dir.exists("config/calibration.json"))
            return dir.absolutePath();
        dir.cdUp();
    }
    return QStringLiteral(DF_SOURCE_ROOT);
}
// Accept legacy container paths and portable paths relative to the project.
QString resolveProjectPath(const QString &path, const QString &root) {
    if (path.isEmpty())
        return {};
    const auto clean = QDir::cleanPath(path);
    if (clean == "/project")
        return QDir(root).absolutePath();
    if (clean.startsWith("/project/"))
        return QDir(root).absoluteFilePath(clean.mid(9));
    return QDir(root).absoluteFilePath(clean);
}
bool isPersistentPath(const QString &path) {
    const auto mount = qEnvironmentVariable("DF_HOST_PERSISTENT_ROOT");
    if (mount.isEmpty())
        return true; // Native execution has no container-only directories.
    const auto resolved = resolveProjectPath(path, mount);
    if (resolved.isEmpty())
        return false;
    // Resolve the existing ancestor as well: a symlink can leave the mounted tree.
    auto dir = QDir(resolved);
    while (!dir.exists()) {
        const auto parent = QFileInfo(dir.absolutePath()).dir();
        if (parent.absolutePath() == dir.absolutePath())
            return false;
        dir = parent;
    }
    const auto canonical = dir.canonicalPath();
    const auto base = QDir(mount).canonicalPath();
    return !base.isEmpty() && (canonical == base || canonical.startsWith(base + "/"));
}
QJsonObject readJson(const QString &path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        throw std::runtime_error(("无法读取 " + path).toStdString());
    QJsonParseError error;
    auto document = QJsonDocument::fromJson(f.readAll(), &error);
    if (error.error != QJsonParseError::NoError || !document.isObject())
        throw std::runtime_error(("JSON格式不正确：" + path).toStdString());
    return document.object();
}
void writeJson(const QString &path, const QJsonObject &value) {
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(QJsonDocument(value).toJson()) < 0 ||
        !file.commit())
        throw std::runtime_error(("无法保存 " + path).toStdString());
}
} // namespace df
