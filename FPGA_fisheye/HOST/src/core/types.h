#pragma once
#include <QImage>
#include <QJsonObject>
#include <QString>
#include <array>
#include <opencv2/core.hpp>
namespace df {
struct View {
    double yaw = 0, pitch = 0, hfov = 90;
    bool operator==(const View &b) const {
        return yaw == b.yaw && pitch == b.pitch && hfov == b.hfov;
    }
};
struct Alignment {
    std::array<double, 3> rear{0, 0, 0};
    double blend = 20;
    bool operator==(const Alignment &b) const {
        return rear == b.rear && blend == b.blend;
    }
};
struct FramePacket {
    quint64 generation = 0;
    int frame = 0, count = 0;
    double pts = 0, duration = 0, sourceFps = 0, renderMs = 0, coverage = 0;
    bool video = false, playing = false;
    View view;
    Alignment alignment;
    QImage raw, panorama, sphere, gimbal;
};
QImage toImage(const cv::Mat &image);
QString projectRoot();
QString resolveProjectPath(const QString &path, const QString &root);
bool isPersistentPath(const QString &path);
QJsonObject readJson(const QString &path);
void writeJson(const QString &path, const QJsonObject &value);
} // namespace df
