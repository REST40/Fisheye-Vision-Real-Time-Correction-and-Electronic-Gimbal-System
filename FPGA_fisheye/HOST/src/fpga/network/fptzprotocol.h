#pragma once
#include <QByteArray>
#include <QtGlobal>

namespace FptzProtocol {
constexpr quint16 HardwareFault = 0x0101;
struct Packet {
    quint8 type = 0;
    quint32 session = 0, txn = 0, epoch = 0;
    QByteArray payload;
    quint32 frame = 0;
};
quint32 crc32(const QByteArray &bytes);
QByteArray pack(const Packet &packet);
bool parse(const QByteArray &bytes, Packet *packet);
} // namespace FptzProtocol
