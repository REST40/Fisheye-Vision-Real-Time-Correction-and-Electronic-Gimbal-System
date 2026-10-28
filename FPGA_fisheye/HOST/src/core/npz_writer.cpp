#include "npz_writer.h"
#include <QJsonDocument>
#include <QtEndian>
#include <stdexcept>
#include <vector>
namespace df {
static QByteArray header(const QByteArray &dtype, const QByteArray &shape) {
    QByteArray text =
        "{'descr': '" + dtype + "', 'fortran_order': False, 'shape': " + shape + ", }";
    while ((10 + text.size() + 1) % 16)
        text += ' ';
    text += '\n';
    QByteArray result("\x93NUMPY\x01\x00", 8);
    quint16 n = qToLittleEndian(quint16(text.size()));
    result.append(reinterpret_cast<const char *>(&n), 2);
    return result + text;
}
NpzWriter::NpzWriter(const QString &path) {
    archive = zipOpen64(path.toUtf8().constData(), APPEND_STATUS_CREATE);
    if (!archive)
        throw std::runtime_error("无法创建NPZ文件");
}
NpzWriter::~NpzWriter() {
    if (archive)
        zipClose(archive, nullptr);
}
void NpzWriter::write(const QString &name, const QByteArray &head, const void *data,
                      quint64 bytes) {
    if (zipOpenNewFileInZip64(archive, (name + ".npy").toUtf8().constData(), nullptr, nullptr, 0,
                              nullptr, 0, nullptr, Z_DEFLATED, 6, bytes >= 0xffffffffULL) != ZIP_OK)
        throw std::runtime_error("NPZ写入失败");
    if (zipWriteInFileInZip(archive, head.constData(), unsigned(head.size())) != ZIP_OK ||
        zipWriteInFileInZip(archive, data, unsigned(bytes)) != ZIP_OK ||
        zipCloseFileInZip(archive) != ZIP_OK)
        throw std::runtime_error("NPZ压缩失败");
}
void NpzWriter::array(const char *name, const cv::Mat &data, bool boolean) {
    CV_Assert(data.channels() == 1 && (data.depth() == CV_32F || data.depth() == CV_8U));
    cv::Mat contiguous = data.isContinuous() ? data : data.clone();
    if (boolean) {
        contiguous = cv::Mat();
        cv::compare(data, 0, contiguous, cv::CMP_NE);
        contiguous /= 255;
    }
    write(QString::fromLatin1(name),
          header(boolean                  ? "|b1"
                 : data.depth() == CV_32F ? "<f4"
                                          : "|u1",
                 "(" + QByteArray::number(data.rows) + ", " + QByteArray::number(data.cols) + ")"),
          contiguous.data, contiguous.total() * contiguous.elemSize());
}
void NpzWriter::metadata(const QJsonObject &value) {
    auto chars = QString::fromUtf8(QJsonDocument(value).toJson(QJsonDocument::Compact)).toUcs4();
    std::vector<quint32> data;
    data.reserve(chars.size());
    for (auto c : chars)
        data.push_back(qToLittleEndian(quint32(c)));
    write("metadata_json", header("<U" + QByteArray::number(chars.size()), "()"), data.data(),
          data.size() * 4);
}
void NpzWriter::finish() {
    if (archive) {
        auto code = zipClose(archive, nullptr);
        archive = nullptr;
        if (code != ZIP_OK)
            throw std::runtime_error("NPZ关闭失败");
    }
}
} // namespace df
