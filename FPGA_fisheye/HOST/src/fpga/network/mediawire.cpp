#include "mediawire.h"
#include <QBuffer>
#include <QImageWriter>
#include <QtEndian>
#include <chrono>
#include <cstring>
namespace MediaWire {
qint64 nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}
static bool validLens(const QByteArray &jpeg) {
    if (jpeg.size() < 4 || jpeg.size() > MaxJpeg || !jpeg.startsWith(QByteArray::fromHex("ffd8")) ||
        !jpeg.endsWith(QByteArray::fromHex("ffd9")))
        return false;
    const auto *b = reinterpret_cast<const uchar *>(jpeg.constData());
    int pos = 2;
    bool sof = false, dqt = false, dht = false;
    while (pos + 4 <= jpeg.size()) {
        if (b[pos++] != 255)
            return false;
        while (pos < jpeg.size() && b[pos] == 255)
            ++pos;
        if (pos + 3 > jpeg.size())
            return false;
        const int marker = b[pos++], len = qFromBigEndian<quint16>(b + pos);
        if (len < 2 || pos + len > jpeg.size())
            return false;
        const auto *v = b + pos;
        if (marker == 0xc0) {
            if (sof || len != 17 || v[2] != 8 || qFromBigEndian<quint16>(v + 3) != 1328 ||
                qFromBigEndian<quint16>(v + 5) != 1328 || v[7] != 3 || v[8] != 1 || v[9] != 0x22 ||
                v[11] != 2 || v[12] != 0x11 || v[14] != 3 || v[15] != 0x11)
                return false;
            sof = true;
        } else if (marker == 0xdb)
            dqt = true;
        else if (marker == 0xc4)
            dht = true;
        else if (marker == 0xda) {
            if (!(sof && dqt && dht && len == 12 && v[2] == 3 && v[3] == 1 && v[5] == 2 &&
                  v[7] == 3 && v[9] == 0 && v[10] == 63 && v[11] == 0))
                return false;
            for (int scan = pos + len; scan + 1 < jpeg.size();) {
                if (b[scan] != 255) {
                    ++scan;
                    continue;
                }
                if (b[scan + 1] == 0) {
                    scan += 2;
                    continue;
                }
                if (b[scan + 1] == 255) {
                    ++scan;
                    continue;
                }
                return b[scan + 1] == 0xd9 && scan == jpeg.size() - 2;
            }
            return false;
        } else if (marker != 0xfe && !(marker >= 0xe0 && marker <= 0xef))
            return false;
        pos += len;
    }
    return false;
}
static int blockSize(int n) {
    return (n + 127) & ~63;
}
bool validInput(const QByteArray &pair) {
    if (pair.size() < 64 || pair.size() > 64 + 2 * blockSize(MaxJpeg))
        return false;
    const auto *b = reinterpret_cast<const uchar *>(pair.constData());
    const quint32 n0 = qFromBigEndian<quint32>(b), n1 = qFromBigEndian<quint32>(b + 4);
    if (n0 < 4 || n1 < 4 || n0 > MaxJpeg || n1 > MaxJpeg)
        return false;
    const int off1 = 64 + blockSize(int(n0));
    if (pair.size() != off1 + blockSize(int(n1)))
        return false;
    for (int i = 8; i < 64; ++i)
        if (b[i])
            return false;
    const int offset[] = {64, off1}, length[] = {int(n0), int(n1)};
    for (int lens = 0; lens < 2; ++lens) {
        if (!validLens(pair.mid(offset[lens], length[lens])))
            return false;
        for (int i = length[lens]; i < blockSize(length[lens]); ++i)
            if (b[offset[lens] + i])
                return false;
    }
    return true;
}
QByteArray encode(const QImage &image, QString *error) {
    auto fail = [error](const QString &message) {
        if (error)
            *error = message;
        return QByteArray();
    };
    if (image.size() != QSize(2656, 1328))
        return fail(QStringLiteral("PS要求完整2656×1328双鱼眼，相机实际为%1×%2；不会缩放发送")
                        .arg(image.width())
                        .arg(image.height()));
    QByteArray lens[2];
    for (int i = 0; i < 2; ++i) {
        QBuffer buffer(&lens[i]);
        buffer.open(QIODevice::WriteOnly);
        QImageWriter writer(&buffer, "jpeg");
        writer.setQuality(85);
        writer.setOptimizedWrite(false);
        writer.setProgressiveScanWrite(false);
        // RGB888 camera frames can expose a read-only lens window without copying
        // its pixels. Keep the full image alive and preserve its original stride.
        const QImage lensImage = image.format() == QImage::Format_RGB888
                                     ? QImage(image.constBits() + i * 1328 * 3, 1328, 1328,
                                              image.bytesPerLine(), image.format())
                                     : image.copy(i * 1328, 0, 1328, 1328);
        if (!writer.write(lensImage))
            return fail(writer.errorString());
        if (!validLens(lens[i]))
            return fail(QStringLiteral("JPEG编码器未生成PL兼容的1328×1328 Baseline 4:2:0"));
    }
    const int off1 = 64 + blockSize(lens[0].size());
    QByteArray result(off1 + blockSize(lens[1].size()), 0);
    auto *b = reinterpret_cast<uchar *>(result.data());
    qToBigEndian<quint32>(lens[0].size(), b);
    qToBigEndian<quint32>(lens[1].size(), b + 4);
    std::memcpy(result.data() + 64, lens[0].constData(), lens[0].size());
    std::memcpy(result.data() + off1, lens[1].constData(), lens[1].size());
    if (error)
        error->clear();
    return result;
}
static bool shape(const Chunk &c) {
    return (c.type == 0x23 || c.type == 0x21) && c.session && c.frame && c.epoch && c.length >= 4 &&
           c.length <= quint32(c.type == 0x23 ? 64 + 2 * blockSize(MaxJpeg) : MaxJpeg) &&
           c.count == (c.length + 1399) / 1400 && c.index < c.count &&
           c.payload.size() == qMin(quint32(1400), c.length - c.index * 1400u);
}
QByteArray pack(const Chunk &c) {
    if (!shape(c))
        return {};
    QByteArray result(48, 0);
    auto *p = reinterpret_cast<uchar *>(result.data());
    std::memcpy(p, "FPTZ", 4);
    p[4] = 3;
    p[5] = c.type;
    qToBigEndian<quint16>(48, p + 6);
    qToBigEndian<quint16>(c.payload.size(), p + 10);
    qToBigEndian<quint32>(c.session, p + 12);
    qToBigEndian<quint32>(c.frame, p + 20);
    qToBigEndian<quint32>(c.epoch, p + 24);
    qToBigEndian<quint16>(c.index, p + 28);
    qToBigEndian<quint16>(c.count, p + 30);
    qToBigEndian<quint32>(c.length, p + 32);
    qToBigEndian<quint32>(c.crc, p + 36);
    qToBigEndian<quint32>(c.captureTimestampMs, p + 40);
    return result + c.payload;
}
bool parse(const QByteArray &raw, Chunk *c) {
    if (!c || raw.size() < 48 || raw.left(4) != "FPTZ")
        return false;
    const auto *p = reinterpret_cast<const uchar *>(raw.constData());
    if (p[4] != 3 || qFromBigEndian<quint16>(p + 6) != 48 || qFromBigEndian<quint16>(p + 8) ||
        qFromBigEndian<quint32>(p + 16) || qFromBigEndian<quint32>(p + 44) ||
        raw.size() != 48 + qFromBigEndian<quint16>(p + 10))
        return false;
    Chunk value{p[5],
                qFromBigEndian<quint32>(p + 12),
                qFromBigEndian<quint32>(p + 20),
                qFromBigEndian<quint32>(p + 24),
                qFromBigEndian<quint32>(p + 32),
                qFromBigEndian<quint32>(p + 36),
                qFromBigEndian<quint16>(p + 28),
                qFromBigEndian<quint16>(p + 30),
                raw.mid(48),
                qFromBigEndian<quint32>(p + 40)};
    if (!shape(value))
        return false;
    *c = value;
    return true;
}
QByteArray Collector::add(const Chunk &c, qint64 now) {
    for (auto i = m_partial.begin(); i != m_partial.end();) {
        if (now - i->started >= 2000)
            i = m_partial.erase(i);
        else
            ++i;
    }
    if (!shape(c) || c.type != 0x21)
        return {};
    if (!m_partial.contains(c.frame)) {
        if (m_partial.size() >= 4) {
            auto oldest = m_partial.begin();
            for (auto i = m_partial.begin(); i != m_partial.end(); ++i)
                if (i->started < oldest->started)
                    oldest = i;
            m_partial.erase(oldest);
        }
        m_partial.insert(c.frame, {c, QByteArray(c.length, 0), {}, now});
    }
    auto &part = m_partial[c.frame];
    const auto &id = part.identity;
    if (id.session != c.session || id.epoch != c.epoch || id.length != c.length ||
        id.crc != c.crc || id.count != c.count) {
        m_partial.remove(c.frame);
        return {};
    }
    const int offset = c.index * 1400;
    if (part.seen.contains(c.index)) {
        if (part.bytes.mid(offset, c.payload.size()) != c.payload)
            m_partial.remove(c.frame);
        return {};
    }
    std::memcpy(part.bytes.data() + offset, c.payload.constData(), c.payload.size());
    part.seen.insert(c.index);
    if (part.seen.size() != c.count)
        return {};
    auto data = part.bytes;
    m_partial.remove(c.frame);
    if (FptzProtocol::crc32(data) != c.crc || !data.startsWith(QByteArray::fromHex("ffd8")) ||
        !data.endsWith(QByteArray::fromHex("ffd9")))
        return {};
    return data;
}
void Collector::clear() {
    m_partial.clear();
}
} // namespace MediaWire
