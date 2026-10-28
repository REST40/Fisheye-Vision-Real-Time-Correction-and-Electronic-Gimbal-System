#include "paired_source.h"
#include "video_probe.h"
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <cmath>
#include <opencv2/imgcodecs.hpp>
#include <stdexcept>
namespace df {
PairedSource::PairedSource(const QString &path, bool useVideo, cv::Size input)
    : directory(path), video(useVideo) {
    std::array<QString, 2> names{"front", "rear"};
    for (int i = 0; i < 2; ++i) {
        QString file = path + "/" + names[i] + ".png";
        if (!QFile::exists(file))
            throw std::runtime_error("目录缺少前后PNG图片");
        photos[i] = cv::imread(file.toStdString());
        if (photos[i].empty() || photos[i].size() != input)
            throw std::runtime_error("照片尺寸与校正参数不一致");
    }
    if (!video) {
        pts = {0};
        return;
    }
    std::array<std::vector<double>, 2> times;
    std::array<double, 2> durations;
    for (int i = 0; i < 2; ++i) {
        auto info = probeVideo(path + "/" + names[i] + "_10s.mp4");
        auto streams = info["streams"].toArray();
        if (streams.size() != 1)
            throw std::runtime_error("视频流数量不正确");
        auto stream = streams[0].toObject();
        if (stream["codec_name"].toString() != "mjpeg")
            throw std::runtime_error("请选择前后采集保存的MJPEG/MP4视频");
        if (stream["width"].toInt() != input.width || stream["height"].toInt() != input.height)
            throw std::runtime_error("视频尺寸与校正参数不一致");
        durations[i] = stream["duration"].toString().toDouble();
        for (auto packet : info["packets"].toArray()) {
            auto value = packet.toObject()["pts_time"];
            bool okay = false;
            double t = value.toString().toDouble(&okay);
            if (!okay || !std::isfinite(t) || t < 0 || (!times[i].empty() && t <= times[i].back()))
                throw std::runtime_error("帧时间戳无效");
            times[i].push_back(t);
        }
        if (times[i].empty() || durations[i] <= times[i].back())
            throw std::runtime_error("视频时长无效");
    }
    if (times[0].size() != times[1].size() || fabs(durations[0] - durations[1]) > 1e-6)
        throw std::runtime_error("前后帧数或时长不同");
    for (size_t i = 0; i < times[0].size(); ++i)
        if (fabs(times[0][i] - times[1][i]) > 1e-6)
            throw std::runtime_error("前后帧时间戳不同");
    pts = times[0];
    duration = durations[0];
    fps = pts.size() / duration;
    QFile csv(path + "/frames.csv");
    if (csv.exists()) {
        if (!csv.open(QIODevice::ReadOnly))
            throw std::runtime_error("无法读取frames.csv");
        auto header = csv.readLine().trimmed().split(',');
        int column = header.indexOf("pts_us"), index = 0;
        if (column < 0)
            throw std::runtime_error("frames.csv缺少pts_us");
        while (!csv.atEnd()) {
            auto line = csv.readLine().trimmed();
            if (line.isEmpty())
                continue;
            auto fields = line.split(',');
            bool okay = false;
            double t = fields.value(column).toDouble(&okay) / 1e6;
            if (!okay || index >= int(pts.size()) || fabs(t - pts[index++]) > 1e-6)
                throw std::runtime_error("视频与frames.csv时间戳不同");
        }
        if (index != int(pts.size()))
            throw std::runtime_error("frames.csv帧数不同");
    }
    for (int i = 0; i < 2; ++i)
        if (!captures[i].open((path + "/" + names[i] + "_10s.mp4").toStdString(), cv::CAP_FFMPEG))
            throw std::runtime_error("OpenCV无法打开视频");
}
const std::array<cv::Mat, 2> &PairedSource::read(int index) {
    if (!video)
        return photos;
    if (index < 0 || index >= int(pts.size()))
        throw std::runtime_error("视频帧索引越界");
    if (index == current)
        return frames;
    if (index <= current) {
        for (auto &c : captures)
            if (!c.set(cv::CAP_PROP_POS_FRAMES, index))
                throw std::runtime_error("视频跳转失败");
        current = index - 1;
    }
    while (current + 1 < index) {
        for (auto &c : captures)
            if (!c.grab())
                throw std::runtime_error("跳帧失败");
        ++current;
    }
    for (int i = 0; i < 2; ++i)
        if (!captures[i].read(frames[i]) || frames[i].empty())
            throw std::runtime_error("前后视频解码失败");
    current = index;
    return frames;
}
QString defaultDataset(const QString &root) {
    QDir data(root + "/data/captures");
    auto entries = data.entryInfoList({"X5_data_*"}, QDir::Dirs | QDir::NoDotAndDotDot,
                                      QDir::Name | QDir::Reversed);
    for (const auto &entry : entries) {
        QDir folder(entry.absoluteFilePath());
        if (folder.exists("front.png") && folder.exists("rear.png") &&
            folder.exists("front_10s.mp4") && folder.exists("rear_10s.mp4"))
            return entry.absoluteFilePath();
    }
    return root + "/data/demo";
}
} // namespace df
