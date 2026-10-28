#include "capture_service.h"
#include <QJsonDocument>
#include <QProcessEnvironment>
#include <QtEndian>
#include <cstring>
namespace df {
CaptureService::CaptureService(QString path, QObject *parent)
    : QObject(parent), root(std::move(path)) {
    connect(&process, &QProcess::readyReadStandardOutput, this, &CaptureService::readOutput);
    connect(&process, &QProcess::readyReadStandardError, this, [this] {
        stderrText = (stderrText + QString::fromUtf8(process.readAllStandardError())).right(8000);
    });
    connect(&process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
            [this](int code, QProcess::ExitStatus) {
                readOutput();
                closeMailbox();
                if (code && errorText.isEmpty()) {
                    errorText = "采集进程异常结束，请查看日志。";
                    emit error(errorText);
                }
                emit ended(code);
            });
    connect(&process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError e) {
        if (e == QProcess::FailedToStart) {
            errorText = "无法启动X5采集进程：" + process.errorString();
            emit error(errorText);
            emit ended(1);
        }
    });
}
CaptureService::~CaptureService() {
    if (running()) {
        shutdown();
        if (!process.waitForFinished(40000)) {
            process.terminate();
            if (!process.waitForFinished(5000)) {
                process.kill();
                process.waitForFinished();
            }
        }
    }
    closeMailbox();
}
void CaptureService::start(const QJsonObject &settings) {
    if (running())
        return;
    closeMailbox();
    buffer.clear();
    errorText.clear();
    stderrText.clear();
    sessionPath.clear();
    QStringList args{root + "/tools/x5/workbench_session.py"};
    for (const auto &pair :
         std::initializer_list<std::pair<QString, QString>>{{"output_root", "--output-root"},
                                                            {"width", "--width"},
                                                            {"height", "--height"},
                                                            {"encoder", "--encoder"},
                                                            {"bitrate", "--bitrate"},
                                                            {"seconds", "--seconds"},
                                                            {"exposure_mode", "--exposure-mode"},
                                                            {"iso", "--iso"},
                                                            {"shutter", "--shutter"},
                                                            {"ev", "--ev"}}) {
        args << pair.second;
        auto value = settings[pair.first];
        args << (value.isString() ? value.toString() : QString::number(value.toDouble(), 'g', 12));
    }
    if (recover)
        args << "--recover-usb";
    recover = false;
    auto env = QProcessEnvironment::systemEnvironment();
    env.remove("LD_LIBRARY_PATH");
    env.insert("PYTHONUNBUFFERED", "1");
    env.insert("TZ", "Asia/Shanghai");
    process.setProcessEnvironment(env);
    process.setProgram("python3");
    process.setArguments(args);
    process.start();
}
void CaptureService::send(const QJsonObject &command) {
    if (running())
        process.write(QJsonDocument(command).toJson(QJsonDocument::Compact) + '\n');
}
void CaptureService::shutdown() {
    send({{"cmd", "shutdown"}});
}
void CaptureService::readOutput() {
    buffer += process.readAllStandardOutput();
    if (buffer.size() > 262144) {
        buffer.clear();
        emit error("采集状态消息过长");
        return;
    }
    int index;
    while ((index = buffer.indexOf('\n')) >= 0) {
        auto line = buffer.left(index);
        buffer.remove(0, index + 1);
        QJsonParseError parse;
        auto document = QJsonDocument::fromJson(line, &parse);
        if (parse.error != QJsonParseError::NoError || !document.isObject())
            continue;
        auto data = document.object();
        QString name = data["event"].toString();
        if (name == "session") {
            sessionPath = data["path"].toString();
            mailbox.setFileName(data["preview"].toString());
            rawMailbox.setFileName(data["preview"].toString() + ".raw");
        }
        if (name == "error") {
            errorText = data["message"].toString();
            emit error(errorText);
        }
        if (name == "finished" && !data["okay"].toBool()) {
            auto status = data["status"].toObject();
            errorText = status["problem"].toString();
            recover = status["problem_code"].toString() == "usb_sync";
            emit error(errorText);
        }
        emit event(data);
    }
}
void CaptureService::closeMailbox() {
    if (rawMemory) {
        rawMailbox.unmap(rawMemory);
        rawMemory = nullptr;
    }
    rawMailbox.close();
    rawMailbox.setFileName({});
    rawFrameId = 0;
    if (memory) {
        mailbox.unmap(memory);
        memory = nullptr;
    }
    mailbox.close();
    mailbox.setFileName({});
    frameId = 0;
}
bool CaptureService::preview(QImage &raw, QImage &pano, qint64 &rawTimestamp, qint64 &timestamp) {
    constexpr int bytes = 64 + 1280 * 320 * 3;
    if (mailbox.fileName().isEmpty())
        return false;
    if (!memory) {
        if (!mailbox.open(QIODevice::ReadOnly))
            return false;
        if (mailbox.size() != bytes) {
            mailbox.close();
            return false;
        }
        memory = mailbox.map(0, bytes);
        if (!memory) {
            mailbox.close();
            return false;
        }
    }
    auto value64 = [this](int offset) { return qFromLittleEndian<quint64>(memory + offset); };
    quint64 sequence =
                __atomic_load_n(reinterpret_cast<const quint64 *>(memory + 8), __ATOMIC_SEQ_CST),
            id = value64(16);
    quint32 version = qFromLittleEndian<quint32>(memory + 4);
    if (qFromLittleEndian<quint32>(memory) != 0x50563558 || (version != 1 && version != 2) ||
        (sequence & 1) || !id || id == frameId)
        return false;
    QImage combined = QImage(memory + 64, 1280, 320, 3840, QImage::Format_RGB888).copy();
    timestamp = qint64(value64(24));
    rawTimestamp = qint64(version == 2 ? value64(56) : value64(24));
    if (sequence !=
        __atomic_load_n(reinterpret_cast<const quint64 *>(memory + 8), __ATOMIC_SEQ_CST))
        return false;
    raw = combined.copy(0, 0, 640, 320);
    pano = combined.copy(640, 0, 640, 320);
    frameId = id;
    return true;
}
bool CaptureService::fullRaw(QImage &raw, qint64 &timestamp) {
    constexpr qint64 bytes = 64 + 2656 * 1328 * 3;
    if (rawMailbox.fileName().isEmpty())
        return false;
    if (!rawMemory) {
        if (!rawMailbox.open(QIODevice::ReadOnly))
            return false;
        if (rawMailbox.size() != bytes) {
            rawMailbox.close();
            return false;
        }
        rawMemory = rawMailbox.map(0, bytes);
        if (!rawMemory) {
            rawMailbox.close();
            return false;
        }
    }
    auto *sequence = reinterpret_cast<const quint64 *>(rawMemory + 8);
    const auto seq = __atomic_load_n(sequence, __ATOMIC_SEQ_CST);
    const auto id = qFromLittleEndian<quint64>(rawMemory + 16);
    if ((seq & 1) || !id || id == rawFrameId ||
        qFromLittleEndian<quint32>(rawMemory) != 0x52573558 ||
        qFromLittleEndian<quint32>(rawMemory + 4) != 1)
        return false;
    const auto w = qFromLittleEndian<quint32>(rawMemory + 32),
               h = qFromLittleEndian<quint32>(rawMemory + 36);
    if (!w || !h || quint64(w) * h * 3 > quint64(bytes - 64))
        return false;
    auto copy = QImage(rawMemory + 64, int(w), int(h), int(w) * 3, QImage::Format_RGB888).copy();
    const auto ts = qFromLittleEndian<quint64>(rawMemory + 24);
    if (seq != __atomic_load_n(sequence, __ATOMIC_SEQ_CST))
        return false;
    raw = copy;
    timestamp = qint64(ts);
    rawFrameId = id;
    return true;
}
} // namespace df
