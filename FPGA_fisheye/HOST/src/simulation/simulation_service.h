#pragma once
#include "core/types.h"
#include <QMutex>
#include <QThread>
#include <QWaitCondition>
#include <memory>
#include <optional>
namespace df {
class SimulationService : public QThread {
    Q_OBJECT
  public:
    explicit SimulationService(QString parameters, QObject *parent = nullptr);
    ~SimulationService() override;
    quint64 load(const QString &directory, bool video);
    void unload();
    void setView(View value);
    void setAlignment(Alignment value);
    void setPlaying(bool value);
    void seek(double seconds);
    void exportFrame(const QString &root, const FramePacket &displayed);
    void stop();
    std::optional<FramePacket> poll();
  signals:
    void failed(quint64 generation, QString error);
    void exported(quint64 generation, QString directory);
    void exportFailed(quint64 generation, QString error);

  protected:
    void run() override;

  private:
    struct State {
        QString directory;
        bool video = true, playing = true;
        View view;
        Alignment alignment;
        double seek = 0;
    };
    struct Export {
        QString root;
        FramePacket displayed;
    };
    QString parameters;
    QMutex mutex;
    QWaitCondition changed;
    State state;
    quint64 generation = 0, version = 0, seekRevision = 0;
    bool quitting = false;
    std::optional<FramePacket> latest;
    std::optional<Export> request;
};
} // namespace df
