#include "fptzprotocol.h"
#include <QtEndian>
#include <array>
#include <cstring>
namespace FptzProtocol {
quint32 crc32(const QByteArray &bytes) {
    // Slicing by eight keeps full-frame validation from blocking UDP receive
    // for several milliseconds. Same reflected IEEE polynomial and wire CRC.
    static const std::array<std::array<quint32, 256>, 8> table = [] {
        std::array<std::array<quint32, 256>, 8> result{};
        for (quint32 byte = 0; byte < 256; ++byte) {
            quint32 value = byte;
            for (int bit = 0; bit < 8; ++bit)
                value = (value >> 1) ^ (0xedb88320u & (0u - (value & 1u)));
            result[0][byte] = value;
        }
        for (int slice = 1; slice < 8; ++slice)
            for (quint32 byte = 0; byte < 256; ++byte) {
                const auto value = result[slice - 1][byte];
                result[slice][byte] = (value >> 8) ^ result[0][value & 255u];
            }
        return result;
    }();
    quint32 crc = 0xffffffffu;
    const auto *p = reinterpret_cast<const uchar *>(bytes.constData());
    auto remaining = bytes.size();
    while (remaining >= 8) {
        const quint32 low = crc ^ qFromLittleEndian<quint32>(p);
        crc = table[7][low & 255u] ^ table[6][(low >> 8) & 255u] ^
              table[5][(low >> 16) & 255u] ^ table[4][low >> 24] ^
              table[3][p[4]] ^ table[2][p[5]] ^ table[1][p[6]] ^ table[0][p[7]];
        p += 8;
        remaining -= 8;
    }
    while (remaining--)
        crc = (crc >> 8) ^ table[0][(crc ^ *p++) & 255u];
    return ~crc;
}
QByteArray pack(const Packet &packet) {
    if (packet.type < 1 || packet.type > 17 || packet.payload.size() > 1400)
        return {};
    QByteArray result(48, 0);
    auto *p = reinterpret_cast<uchar *>(result.data());
    std::memcpy(p, "FPTZ", 4);
    p[4] = 3;
    p[5] = packet.type;
    qToBigEndian<quint16>(48, p + 6);
    qToBigEndian<quint16>(quint16(packet.payload.size()), p + 10);
    qToBigEndian<quint32>(packet.session, p + 12);
    qToBigEndian<quint32>(packet.txn, p + 16);
    qToBigEndian<quint32>(packet.epoch, p + 24);
    qToBigEndian<quint32>(packet.frame, p + 20);
    qToBigEndian<quint16>(1, p + 30);
    qToBigEndian<quint32>(quint32(packet.payload.size()), p + 32);
    qToBigEndian<quint32>(crc32(packet.payload), p + 36);
    return result + packet.payload;
}
bool parse(const QByteArray &bytes, Packet *packet) {
    if (!packet || bytes.size() < 48 || bytes.left(4) != "FPTZ")
        return false;
    const auto *p = reinterpret_cast<const uchar *>(bytes.constData());
    const auto length = qFromBigEndian<quint16>(p + 10);
    if (p[4] != 3 || p[5] < 1 || p[5] > 17 || qFromBigEndian<quint16>(p + 6) != 48 ||
        qFromBigEndian<quint16>(p + 8) != 0 || length > 1400 || bytes.size() != 48 + length ||
        qFromBigEndian<quint16>(p + 28) != 0 || qFromBigEndian<quint16>(p + 30) != 1 ||
        qFromBigEndian<quint32>(p + 32) != length || qFromBigEndian<quint32>(p + 44) != 0)
        return false;
    const QByteArray body = bytes.mid(48);
    if (crc32(body) != qFromBigEndian<quint32>(p + 36))
        return false;
    *packet = {p[5],
               qFromBigEndian<quint32>(p + 12),
               qFromBigEndian<quint32>(p + 16),
               qFromBigEndian<quint32>(p + 24),
               body,
               qFromBigEndian<quint32>(p + 20)};
    return true;
}
} // namespace FptzProtocol
