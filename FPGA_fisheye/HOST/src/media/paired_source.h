#pragma once
#include <QString>
#include <array>
#include <opencv2/core.hpp>
#include <opencv2/videoio.hpp>
#include <vector>
namespace df {
class PairedSource {
  public:
    PairedSource(const QString &directory, bool video, cv::Size input);
    const std::array<cv::Mat, 2> &read(int index);
    std::vector<double> pts;
    double duration = 0, fps = 0;
    QString directory;
    bool video;

  private:
    std::array<cv::Mat, 2> photos, frames;
    std::array<cv::VideoCapture, 2> captures;
    int current = -1;
};
QString defaultDataset(const QString &root);
} // namespace df
