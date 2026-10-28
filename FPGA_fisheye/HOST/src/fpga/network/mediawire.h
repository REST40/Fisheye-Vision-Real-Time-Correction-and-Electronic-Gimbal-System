#pragma once
#include "fptzprotocol.h"
#include <QHash>
#include <QImage>
#include <QSet>

namespace MediaWire {
constexpr int PayloadSize = 1400, MaxJpeg = 8 * 1024 * 1024;
qint64 nowMs();
struct Chunk {
    quint8 type = 0;
    quint32 session = 0, frame = 0, epoch = 0, length = 0, crc = 0;
    quint16 index = 0, count = 0;
    QByteArray payload;
    quint32 captureTimestampMs = 0;
};
QByteArray encode(const QImage &image, QString *error = nullptr);
bool validInput(const QByteArray &jpeg);
QByteArray pack(const Chunk &chunk);
bool parse(const QByteArray &wire, Chunk *chunk);
class Collector {
  public:
    QByteArray add(const Chunk &chunk, qint64 nowMs);
    void clear();
    int partialCount() const {
        return m_partial.size();
    }

  private:
    struct Partial {
        Chunk identity;
        QByteArray bytes;
        QSet<int> seen;
        qint64 started = 0;
    };
    QHash<quint32, Partial> m_partial;
};
} // namespace MediaWire
