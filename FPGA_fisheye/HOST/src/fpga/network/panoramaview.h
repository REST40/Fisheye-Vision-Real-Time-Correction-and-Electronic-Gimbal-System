#pragma once
#include "core/types.h"
#include <QImage>
#include <opencv2/core.hpp>
// Display projection only. The source is the PS/PL stitched panorama.
class PanoramaView {
  public:
    void render(const QImage &panorama, const df::View &view, QImage &sphere, QImage &perspective);

  private:
    QSize source;
    df::View previous;
    bool ready = false;
    cv::Mat sphereX, sphereY, viewX, viewY, domain;
    void maps(const cv::Mat &rays, cv::Mat &x, cv::Mat &y);
};
