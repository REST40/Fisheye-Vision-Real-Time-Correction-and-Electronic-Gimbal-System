#pragma once
#include <QJsonObject>
#include <QString>
#include <minizip/zip.h>
#include <opencv2/core.hpp>
namespace df {
// Native NumPy v1 .npy arrays inside a compressed ZIP, readable by numpy.load.
class NpzWriter {
  public:
    explicit NpzWriter(const QString &file);
    ~NpzWriter();
    void array(const char *name, const cv::Mat &data, bool boolean = false);
    void metadata(const QJsonObject &value);
    void finish();

  private:
    zipFile archive = nullptr;
    void write(const QString &name, const QByteArray &header, const void *data, quint64 bytes);
};
} // namespace df
