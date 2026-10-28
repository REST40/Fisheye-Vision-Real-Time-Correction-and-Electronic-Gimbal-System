#pragma once
#include "core/types.h"
#include <QObject>
namespace df {
struct RawLensPair {
    cv::Mat front, rear;
    qint64 timestampUs = 0;
};
// Transport/protocol boundary. No guessed packet layout or board traffic.
class FpgaLink : public QObject {
    Q_OBJECT
  public:
    explicit FpgaLink(QObject *parent = nullptr) : QObject(parent) {}
    virtual bool protocolAvailable() const {
        return false;
    }
    virtual bool connectBoard(const QString &, quint16) {
        emit unavailable("FPGA协议尚未接入，请先提供控制/视频协议。");
        return false;
    }
    virtual bool sendRawPair(const RawLensPair &) {
        emit unavailable("FPGA原始帧封包格式待定义；输入应保持原始1328×1328尺寸。");
        return false;
    }
    virtual bool sendView(const View &) {
        emit unavailable("FPGA云台控制格式待定义。");
        return false;
    }
    virtual bool uploadLut(const QString &) {
        emit unavailable("FPGA LUT格式、定点位宽及地址布局待定义。");
        return false;
    }
    virtual void disconnectBoard() {}
  signals:
    void unavailable(QString reason);
    void returnedFrame(QImage frame, qint64 timestamp);
    void stateChanged(bool connected);
};
} // namespace df
