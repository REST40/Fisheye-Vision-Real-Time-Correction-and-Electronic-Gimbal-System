#include "video_probe.h"
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QProcess>
#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <vector>
namespace df {
std::array<QString, 2> pairedVideoFiles(const QString &directory) {
    QDir folder(directory);
    if (folder.exists("front.mp4") && folder.exists("rear.mp4"))
        return {"front.mp4", "rear.mp4"};
    std::array<QString, 2> result;
    for (const auto &front : folder.entryList({"front_*.mp4"}, QDir::Files, QDir::Name)) {
        const auto rear = "rear_" + front.mid(6);
        if (!folder.exists(rear))
            continue;
        // Multiple recording pairs need an explicit choice; never mix different suffixes.
        if (!result[0].isEmpty())
            return {};
        result = {front, rear};
    }
    return result;
}
QJsonObject probeVideo(const QString &file) {
    QProcess p;
    p.start("ffprobe",
            {"-v", "error", "-select_streams", "v:0", "-show_packets", "-show_streams",
             "-show_entries", "stream=codec_name,width,height,duration,nb_frames:packet=pts_time",
             "-of", "json", file});
    if (!p.waitForStarted(2000) || !p.waitForFinished(15000) || p.exitCode() != 0)
        throw std::runtime_error(("视频读取失败：" + file).toStdString());
    QJsonParseError e;
    auto d = QJsonDocument::fromJson(p.readAllStandardOutput(), &e);
    if (e.error != QJsonParseError::NoError || !d.isObject())
        throw std::runtime_error("ffprobe输出不正确");
    return d.object();
}

QJsonObject verifyVideoPair(const QString &directory) {
    auto pair = pairedVideoFiles(directory);
    if (pair[0].isEmpty())
        pair = {"fisheye.mp4", "corrected.mp4"};
    const auto &a = pair[0], &b = pair[1];
    auto one = probeVideo(directory + "/" + a), two = probeVideo(directory + "/" + b);
    auto timestamps = [](const QJsonObject &info) {
        std::vector<double> result;
        for (auto packet : info["packets"].toArray()) {
            bool valid = false;
            double t = packet.toObject()["pts_time"].toString().toDouble(&valid);
            if (!valid || !std::isfinite(t))
                throw std::runtime_error("视频时间戳缺失");
            result.push_back(t);
        }
        std::sort(result.begin(), result.end());
        return result;
    };
    auto x = timestamps(one), y = timestamps(two);
    if (x.empty() || x.size() != y.size())
        throw std::runtime_error("两路视频帧数不一致或为空");
    for (size_t i = 0; i < x.size(); ++i)
        if (fabs(x[i] - y[i]) > 1e-6)
            throw std::runtime_error("两路逐帧显示时间戳不一致");
    return {{"passed", true},
            {"paired_frames", int(x.size())},
            {"first_pts_seconds", x.front()},
            {"last_pts_seconds", x.back()}};
}
} // namespace df
