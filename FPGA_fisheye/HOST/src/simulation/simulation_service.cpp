#include "simulation_service.h"
#include "core/projection.h"
#include "media/paired_source.h"
#include <QDateTime>
#include <QMutexLocker>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <opencv2/imgproc.hpp>
namespace df {
static double now() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}
SimulationService::SimulationService(QString path, QObject *parent)
    : QThread(parent), parameters(std::move(path)) {}
SimulationService::~SimulationService() {
    stop();
    wait();
}
quint64 SimulationService::load(const QString &directory, bool video) {
    QMutexLocker lock(&mutex);
    state.directory = directory;
    state.video = video;
    state.playing = video;
    state.seek = 0;
    latest.reset();
    request.reset();
    ++generation;
    ++version;
    changed.wakeAll();
    return generation;
}
void SimulationService::unload() {
    QMutexLocker lock(&mutex);
    state.directory.clear();
    ++generation;
    ++version;
    latest.reset();
    request.reset();
    changed.wakeAll();
}
void SimulationService::setView(View value) {
    QMutexLocker lock(&mutex);
    state.view = value;
    ++version;
    changed.wakeAll();
}
void SimulationService::setAlignment(Alignment value) {
    QMutexLocker lock(&mutex);
    state.alignment = value;
    ++version;
    changed.wakeAll();
}
void SimulationService::setPlaying(bool value) {
    QMutexLocker lock(&mutex);
    state.playing = value;
    ++version;
    changed.wakeAll();
}
void SimulationService::seek(double seconds) {
    QMutexLocker lock(&mutex);
    state.seek = seconds;
    ++seekRevision;
    ++version;
    changed.wakeAll();
}
void SimulationService::exportFrame(const QString &root, const FramePacket &displayed) {
    QMutexLocker lock(&mutex);
    request = Export{root, displayed};
    ++version;
    changed.wakeAll();
}
void SimulationService::stop() {
    QMutexLocker lock(&mutex);
    quitting = true;
    changed.wakeAll();
}
std::optional<FramePacket> SimulationService::poll() {
    QMutexLocker lock(&mutex);
    auto result = std::move(latest);
    latest.reset();
    return result;
}
void SimulationService::run() {
    cv::setNumThreads(2);
    std::unique_ptr<PairedSource> source;
    std::unique_ptr<Projection> geometry;
    quint64 applied = ~quint64(0), seenVersion = ~quint64(0), seenSeek = 0;
    int lastFrame = -1, fullFrame = -1;
    double origin = now(), frozen = 0;
    bool wasPlaying = false;
    Alignment fullAlignment;
    QImage raw, panorama;
    while (true) {
        State current;
        quint64 gen, rev, seekRev;
        std::optional<Export> exporting;
        {
            QMutexLocker lock(&mutex);
            if (quitting)
                break;
            current = state;
            gen = generation;
            rev = version;
            seekRev = seekRevision;
            exporting = std::move(request);
            request.reset();
            if (current.directory.isEmpty()) {
                source.reset();
                changed.wait(&mutex, 100);
                continue;
            }
        }
        try {
            if (applied != gen) {
                if (!geometry)
                    geometry = std::make_unique<Projection>(parameters);
                source = std::make_unique<PairedSource>(current.directory, current.video,
                                                        geometry->inputSize);
                applied = gen;
                lastFrame = fullFrame = -1;
                frozen = 0;
                origin = now();
                wasPlaying = current.playing;
                seenSeek = seekRev;
            }
            double time = now();
            if (seenSeek != seekRev) {
                frozen = std::clamp(current.seek, 0., std::max(0., source->duration - 1e-6));
                origin = time - frozen;
                seenSeek = seekRev;
            } else if (wasPlaying && !current.playing)
                frozen = source->duration ? fmod(time - origin, source->duration) : 0;
            else if (!wasPlaying && current.playing)
                origin = time - frozen;
            wasPlaying = current.playing;
            double position =
                wasPlaying && source->duration ? fmod(time - origin, source->duration) : frozen;
            int index =
                std::max(0, int(std::upper_bound(source->pts.begin(), source->pts.end(), position) -
                                source->pts.begin()) -
                                1);
            if (lastFrame == index && seenVersion == rev && !exporting) {
                QMutexLocker lock(&mutex);
                changed.wait(&mutex, wasPlaying ? 2 : 100);
                continue;
            }
            double started = now();
            const auto &frames = source->read(index);
            geometry->setAlignment(current.alignment);
            geometry->setView(current.view);
            if (index != lastFrame) {
                cv::Mat a, b, combined;
                cv::resize(frames[0], a, {480, 480}, 0, 0, cv::INTER_LINEAR);
                cv::resize(frames[1], b, {480, 480}, 0, 0, cv::INTER_LINEAR);
                cv::hconcat(a, b, combined);
                raw = toImage(combined);
            }
            if (index != fullFrame || !(current.alignment == fullAlignment)) {
                panorama = toImage(geometry->panoramaMap.render(frames[0], frames[1]));
                fullFrame = index;
                fullAlignment = current.alignment;
            }
            cv::Mat sphere;
            cv::cvtColor(geometry->sphereMap.render(frames[0], frames[1]), sphere,
                         cv::COLOR_BGR2BGRA);
            std::vector<cv::Mat> channels;
            cv::split(sphere, channels);
            channels[3] = geometry->sphereDomain;
            cv::merge(channels, sphere);
            FramePacket packet;
            packet.generation = gen;
            packet.frame = index;
            packet.count = int(source->pts.size());
            packet.pts = source->pts[index];
            packet.duration = source->duration;
            packet.sourceFps = source->fps;
            packet.video = source->video;
            packet.playing = wasPlaying;
            packet.view = current.view;
            packet.alignment = current.alignment;
            packet.raw = raw;
            packet.panorama = panorama;
            packet.sphere = toImage(sphere);
            packet.gimbal = toImage(geometry->gimbalMap.render(frames[0], frames[1]));
            packet.coverage = double(cv::countNonZero(geometry->panoramaMap.mask)) /
                              geometry->panoramaMap.mask.total();
            packet.renderMs = (now() - started) * 1000;
            if (lastFrame < 0)
                origin = now() - position;
            {
                QMutexLocker lock(&mutex);
                if (gen == generation)
                    latest = packet;
            }
            lastFrame = index;
            seenVersion = rev;
            if (exporting && exporting->displayed.generation == gen) {
                try {
                    Projection hd(parameters, {2656, 1328}, {1920, 1080}, {720, 720});
                    const auto &shown = exporting->displayed;
                    hd.setAlignment(shown.alignment);
                    hd.setView(shown.view);
                    const auto &original = source->read(shown.frame);
                    QString folder = exporting->root + "/simulation_" +
                                     QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss_zzz");
                    QJsonObject meta{{"directory", source->directory},
                                     {"mode", source->video ? "video" : "photo"},
                                     {"frame_index", shown.frame},
                                     {"pts_seconds", source->pts[shown.frame]},
                                     {"paired_timestamps_verified", source->video}};
                    hd.exportFrame(folder, original[0], original[1], meta);
                    emit exported(gen, folder);
                } catch (const std::exception &e) {
                    emit exportFailed(gen, QString::fromUtf8(e.what()));
                }
            }
        } catch (const std::exception &e) {
            source.reset();
            applied = ~quint64(0);
            {
                QMutexLocker lock(&mutex);
                if (gen != generation)
                    continue;
                state.directory.clear();
                latest.reset();
            }
            emit failed(gen, QString::fromUtf8(e.what()));
        }
    }
}
} // namespace df
