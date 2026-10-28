#pragma once
#include <QFile>
#include <QImage>
#include <QJsonObject>
#include <QObject>
#include <QProcess>
namespace df {
class CaptureService : public QObject {
    Q_OBJECT
  public:
    explicit CaptureService(QString root, QObject *parent = nullptr);
    ~CaptureService() override;
    void start(const QJsonObject &settings);
    void send(const QJsonObject &command);
    void shutdown();
    bool running() const {
        return process.state() != QProcess::NotRunning;
    }
    bool preview(QImage &raw, QImage &panorama, qint64 &rawTimestamp, qint64 &timestamp);
    bool fullRaw(QImage &raw, qint64 &timestamp);
    quint64 fullRawFrameId() const {
        return rawFrameId;
    }
    QString sessionPath, errorText, stderrText;
  signals:
    void event(QJsonObject data);
    void ended(int code);
    void error(QString message);

  private:
    QString root;
    QProcess process;
    QByteArray buffer;
    QFile mailbox, rawMailbox;
    uchar *rawMemory = nullptr;
    quint64 rawFrameId = 0;
    uchar *memory = nullptr;
    quint64 frameId = 0;
    bool recover = false;
    void readOutput();
    void closeMailbox();
};
} // namespace df
